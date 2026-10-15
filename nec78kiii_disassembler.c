/*
 * SPDX-License-Identifier: GPL-3.0-only
 * NEC 78K/III Disassembler, version 1.0
 * Copyright (c) 2026 Dave, KY4WI
 *
 * Based in part on https://github.com/wonst719/nec78k3_disassembler
 * Copyright (c) 2021 wonst719 (MIT License; see LICENSE.MIT).
 *
 * This modified program is distributed under GNU GPL version 3 only.
 * See LICENSE for terms and warranty disclaimer.
 */

/* =====================================================================
 * Overview
 *
 * Two modes:
 *
 *   Disassemble:  78k_disasm ROM [-d DB] [--listing F] [--source F] [--xref F]
 *     Loads one or more ROM images and, optionally, a database (.inc)
 *     that holds everything a person has added: names, comments,
 *     constants, which bytes are code and which are data. Prints a
 *     listing (address and byte columns plus assembler source), the
 *     same text without the columns (--source), or a cross-reference.
 *
 *   Extract:      78k_disasm --extract LISTING [--db F] [--symbols F]
 *     Reads an edited listing and writes the database back out. The ROM
 *     is rebuilt from the listing's own byte columns, the listing is
 *     regenerated from the new database, and nothing is written unless
 *     the result is identical to the input.
 *
 * Everything the printer shows comes from four kinds of state, all
 * keyed by address:
 *   layout    which bytes are code, which are data and in what format
 *   items     lines that go before an address: comment text, labels,
 *             .equ definitions, in the order they were written
 *   eol       end-of-line comments for the line at an address
 *   bindings  per-operand expressions ("operand 2 at 0x0045 is
 *             SERIAL_COMMAND")
 * plus the symbol table. A fresh disassembly (no database) fills the
 * same state from a code walk, so there is a single printer.
 * ===================================================================== */

#define _XOPEN_SOURCE 700   /* POSIX plus realpath() */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Util.h"
#include "Expr.h"
#include "Decode.h"

#define DISASM_VERSION "1.0"
#define NADDR      65536
#define END_SLOT   NADDR          /* items after the last line */
#define FILL_MIN   32             /* 0xFF runs at least this long become fills */
#define COL_OPND   16             /* source column of operands */
#define COL_CMT    40             /* source column of end-of-line comments */
#define MARGIN     22             /* "AAAA  BB BB BB BB BB  " */
#define INDENT     "        "

static const char *const SFR_INCLUDE = "uPD78312A.inc";

/* =====================================================================
 * Errors
 * ===================================================================== */

static const char *g_errFile = NULL;   /* file the current errors refer to */

#if defined(__GNUC__)
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

static NORETURN void die(const char *fmt, ...);
static NORETURN void die_at(int line, const char *fmt, ...);

static void die(const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "78k_disasm: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

/* "file:line: message" (line 0 = no line) */
static void die_at(int line, const char *fmt, ...)
{
	va_list ap;
	if (g_errFile && line > 0)
		fprintf(stderr, "%s:%d: ", g_errFile, line);
	else if (g_errFile)
		fprintf(stderr, "%s: ", g_errFile);
	else
		fprintf(stderr, "78k_disasm: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void warn(const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "78k_disasm: warning: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* =====================================================================
 * Memory
 * ===================================================================== */

static byte g_mem[NADDR];
static bool g_pop[NADDR];

typedef struct Image
{
	char *path;
	unsigned long base, size, crc;
} Image;

static Image *g_img = NULL;
static int g_nimg = 0, g_capimg = 0;

static bool mem_read_byte(unsigned long a, byte *out)
{
	if (a >= NADDR || !g_pop[a])
		return false;
	*out = g_mem[a];
	return true;
}

static unsigned long run_end(unsigned long a)
{
	while (a < NADDR && g_pop[a])
		a++;
	return a;
}

static void add_image(const char *path, unsigned long base, unsigned long size)
{
	Image *im;
	GROW(g_img, g_nimg, g_capimg);
	im = &g_img[g_nimg++];
	im->path = xstrdup(path);
	im->base = base;
	im->size = size;
	im->crc = crc32_buf(&g_mem[base], size);
}

static void load_image(const char *path, unsigned long base)
{
	size_t n;
	char *buf = read_file(path, &n);
	unsigned long i;

	if (!buf)
		die("can't read '%s': %s", path, strerror(errno));
	if (base >= NADDR)
		die("'%s': base 0x%lX is outside the 16-bit address space", path, base);
	if (base + n > NADDR)
	{
		warn("'%s' is %lu bytes, which runs past 0xFFFF at base 0x%04lX; using the first %lu",
			path, (unsigned long)n, base, NADDR - base);
		n = NADDR - base;
	}
	for (i = 0; i < n; i++)
	{
		g_mem[base + i] = (byte)buf[i];
		g_pop[base + i] = true;
	}
	free(buf);
	add_image(path, base, (unsigned long)n);
}

static unsigned long parse_hex_addr(const char *s, bool *ok)
{
	char *e;
	unsigned long v;
	if (s[0] == '$')
		s++;
	else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	v = strtoul(s, &e, 16);
	*ok = (e != s && *e == '\0');
	return v;
}

static unsigned long need_addr(const char *s, const char *what)
{
	bool ok;
	unsigned long v = parse_hex_addr(s, &ok);
	if (!ok || v >= NADDR)
		die("%s '%s' is not an address (use hex, e.g. 0x8000)", what, s);
	return v;
}

/* A file name ending in "0xADDR" (e.g. firmware_0x8000.bin) gives its
 * base address. Requires the literal "0x" so a trailing checksum or date
 * in a name isn't mistaken for one. */
static bool base_from_filename(const char *path, unsigned long *out)
{
	const char *b = strrchr(path, '/');
	const char *dot;
	size_t stem, i;
	b = b ? b + 1 : path;
	dot = strrchr(b, '.');
	stem = dot ? (size_t)(dot - b) : strlen(b);
	if (stem < 6 || b[stem - 6] != '0' || (b[stem - 5] != 'x' && b[stem - 5] != 'X'))
		return false;
	for (i = stem - 4; i < stem; i++)
		if (!isxdigit((unsigned char)b[i]))
			return false;
	*out = strtoul(b + stem - 4, NULL, 16);
	return true;
}

/* =====================================================================
 * Symbols
 * ===================================================================== */

enum
{
	SK_LABEL = 1,  /* positional label: names the address of a line */
	SK_EQU,        /* .equ written in the listing */
	SK_INC,        /* from an .include file (e.g. SFR names) */
	SK_AUTO,       /* generated: sub_XXXX, loc_XXXX, ... */
	SK_CALLT       /* generated CALLT slot name (value: the slot, 0-1FH) */
};

/* CALLT: where the 32-entry table is in the loaded image (-1: none set),
 * the generated name of each slot, and the user's name for a slot (an
 * .equ written at every CALLT that uses it) */
static long g_calltBase = -1;
static int g_calltAuto[32];
static int g_slotName[32];

typedef struct Sym
{
	char *name;
	int kind;
	long long value;
	bool resolved, resolving;
	char *expr;          /* SK_EQU/SK_INC: the value as written */
	char *comment;       /* SK_LABEL/SK_EQU: end-of-line comment, lines split by '\n' */
	bool global;         /* SK_EQU: printed wherever its value is an address */
	bool alias;          /* SK_EQU whose value is just another name */
	char *src;           /* where it was defined, for messages */
	int line;
	bool used;           /* printed in the current output */
	bool lineDefined;    /* defined by a label line or .equ line in the current output */
} Sym;

static Sym *g_sym = NULL;
static int g_nsym = 0, g_capsym = 0;
static int *g_htab = NULL;
static int g_hcap = 0;

static int g_labelAt[NADDR];    /* SK_LABEL, index + 1 */
static int g_autoAt[NADDR];     /* SK_AUTO */
static int g_globalAt[NADDR];   /* global SK_EQU */
static int g_incAt[NADDR];      /* SK_INC with an SFR-range value */
static int g_extAt[NADDR];      /* SK_INC for an address outside every loaded image */

static unsigned long hash_str(const char *s)
{
	unsigned long h = 5381;
	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

static void hash_rebuild(void)
{
	int i;
	g_hcap = g_hcap ? g_hcap * 2 : 1024;
	free(g_htab);
	g_htab = (int *)calloc((size_t)g_hcap, sizeof(int));
	for (i = 0; i < g_nsym; i++)
	{
		unsigned long h = hash_str(g_sym[i].name) & (unsigned long)(g_hcap - 1);
		while (g_htab[h])
			h = (h + 1) & (unsigned long)(g_hcap - 1);
		g_htab[h] = i + 1;
	}
}

static int sym_find(const char *name)
{
	unsigned long h;
	if (!g_hcap)
		return -1;
	h = hash_str(name) & (unsigned long)(g_hcap - 1);
	while (g_htab[h])
	{
		if (strcmp(g_sym[g_htab[h] - 1].name, name) == 0)
			return g_htab[h] - 1;
		h = (h + 1) & (unsigned long)(g_hcap - 1);
	}
	return -1;
}

static int sym_add(const char *name, int kind, const char *src, int line)
{
	Sym *s;
	unsigned long h;
	if ((g_nsym + 1) * 2 >= g_hcap)
	{
		GROW(g_sym, g_nsym, g_capsym);
		hash_rebuild();
	}
	GROW(g_sym, g_nsym, g_capsym);
	s = &g_sym[g_nsym];
	memset(s, 0, sizeof(*s));
	s->name = xstrdup(name);
	s->kind = kind;
	s->src = src ? xstrdup(src) : NULL;
	s->line = line;
	h = hash_str(name) & (unsigned long)(g_hcap - 1);
	while (g_htab[h])
		h = (h + 1) & (unsigned long)(g_hcap - 1);
	g_htab[h] = g_nsym + 1;
	return g_nsym++;
}

static const char *const AUTO_PREFIX[] = { "", "dat", "ram", "ext", "loc", "sub" };
enum { AP_NONE, AP_DAT, AP_RAM, AP_EXT, AP_LOC, AP_SUB };   /* AP_FILL uses dat/code */

/* Recognizes generated names: sub_XXXX, loc_XXXX, dat_XXXX, ram_XXXX,
 * ext_XXXX, code_XXXX (fill anchors), with uppercase hex. */
static bool auto_name_addr(const char *name, unsigned long *addr)
{
	static const char *const pre[] = { "sub_", "loc_", "dat_", "ram_", "ext_", "code_" };
	size_t i, k;
	for (i = 0; i < sizeof(pre) / sizeof(pre[0]); i++)
	{
		size_t n = strlen(pre[i]);
		if (strncmp(name, pre[i], n) != 0 || strlen(name) != n + 4)
			continue;
		for (k = n; k < n + 4; k++)
			if (!(isdigit((unsigned char)name[k]) || (name[k] >= 'A' && name[k] <= 'F')))
				return false;
		*addr = strtoul(name + n, NULL, 16);
		return true;
	}
	return false;
}

static bool sym_value(int idx, long long *v);
static int callt_slot_of_name(const char *name);

static bool lookup_cb(const char *name, long long *v, void *ctx)
{
	int i = sym_find(name);
	unsigned long a;
	(void)ctx;
	if (i >= 0)
		return sym_value(i, v);
	if ((i = callt_slot_of_name(name)) >= 0)
	{
		*v = i;
		return true;
	}
	if (auto_name_addr(name, &a))
	{
		*v = (long long)a;
		return true;
	}
	if (strncmp(name, "image_end_", 10) == 0 && strlen(name) == 14)
	{
		*v = (long long)strtoul(name + 10, NULL, 16);
		return true;
	}
	return false;
}

static bool sym_value(int idx, long long *v)
{
	Sym *s = &g_sym[idx];
	char err[200];
	if (s->resolved)
	{
		*v = s->value;
		return true;
	}
	if (!s->expr || s->resolving)
		return false;
	s->resolving = true;
	if (!expr_eval(s->expr, lookup_cb, NULL, &s->value, err, sizeof(err), NULL, 0))
	{
		s->resolving = false;
		return false;
	}
	s->resolving = false;
	s->resolved = true;
	*v = s->value;
	return true;
}

static void set_label(int idx, unsigned long addr)
{
	g_sym[idx].kind = SK_LABEL;
	g_sym[idx].value = (long long)addr;
	g_sym[idx].resolved = true;
	g_labelAt[addr] = idx + 1;
}

static void comment_append(char **c, const char *line)
{
	if (!*c)
		*c = xstrdup(line);
	else
	{
		char *n = xasprintf("%s\n%s", *c, line);
		free(*c);
		*c = n;
	}
}

/* =====================================================================
 * Annotations
 * ===================================================================== */

enum { IT_TEXT = 1, IT_LABEL, IT_EQU };

typedef struct Item
{
	int kind;
	char *s;
	int next;   /* index + 1 */
} Item;

static Item *g_item = NULL;
static int g_nitem = 0, g_capitem = 0;
static int g_itemHead[NADDR + 1], g_itemTail[NADDR + 1];
static char *g_eol[NADDR];

typedef struct Bind
{
	int slot;       /* 1-based operand; 1 for a data item */
	char *expr;
	int next;
} Bind;

static Bind *g_bind = NULL;
static int g_nbind = 0, g_capbind = 0;
static int g_bindHead[NADDR];

static void item_add(unsigned long a, int kind, const char *s)
{
	Item *it;
	GROW(g_item, g_nitem, g_capitem);
	it = &g_item[g_nitem];
	it->kind = kind;
	it->s = xstrdup(s);
	it->next = 0;
	if (g_itemTail[a])
		g_item[g_itemTail[a] - 1].next = g_nitem + 1;
	else
		g_itemHead[a] = g_nitem + 1;
	g_itemTail[a] = g_nitem + 1;
	g_nitem++;
}

static void eol_add(unsigned long a, const char *text)
{
	comment_append(&g_eol[a], text);
}

static const Bind *bind_find(unsigned long a, int slot)
{
	int i;
	for (i = g_bindHead[a]; i; i = g_bind[i - 1].next)
		if (g_bind[i - 1].slot == slot)
			return &g_bind[i - 1];
	return NULL;
}

static void bind_add(unsigned long a, int slot, const char *expr)
{
	Bind *b;
	GROW(g_bind, g_nbind, g_capbind);
	b = &g_bind[g_nbind];
	b->slot = slot;
	b->expr = xstrdup(expr);
	b->next = g_bindHead[a];
	g_bindHead[a] = g_nbind + 1;
	g_nbind++;
}

/* =====================================================================
 * Layout
 * ===================================================================== */

enum { RT_NONE = 0, RT_CODE, RT_BYTE, RT_WORD, RT_STRING, RT_PTR };
static const char *const RT_NAME[] = { "", "code", "byte", "word", "string", "ptr" };

static byte g_rt[NADDR];        /* range type of each byte */
static unsigned short g_rper[NADDR];  /* items per line (0 = default) */
static bool g_rbeg[NADDR];      /* a range record starts here */
static bool g_brk[NADDR];       /* forced data line break */

enum { LC_NONE = 0, LC_CODE, LC_BODY, LC_BAD };
static byte g_lc[NADDR];        /* instruction starts inside code ranges */
static byte g_ilen[NADDR];

static bool g_userData[NADDR];  /* declared with -k (fresh-mode pointer guess) */

static int default_per(int t)
{
	switch (t)
	{
	case RT_BYTE: return 8;
	case RT_WORD: case RT_PTR: return 4;
	case RT_STRING: return 64;
	default: return 1;
	}
}

static int rt_per(unsigned long a)
{
	return g_rper[a] ? g_rper[a] : default_per(g_rt[a]);
}

/* Decodes every code range linearly from its start. Bytes in a code
 * range that don't decode, or whose instruction would run into another
 * range or unloaded memory, are marked LC_BAD and print as .byte. */
static void decode_layout(void)
{
	unsigned long a = 0;
	memset(g_lc, 0, sizeof(g_lc));
	memset(g_ilen, 0, sizeof(g_ilen));
	while (a < NADDR)
	{
		Insn in;
		int k;
		bool fits = true;
		if (!g_pop[a] || g_rt[a] != RT_CODE)
		{
			a++;
			continue;
		}
		if (!decode_insn(a, &in))
		{
			g_lc[a++] = LC_BAD;
			continue;
		}
		for (k = 1; k < in.len; k++)
			if (g_rt[a + k] != RT_CODE || g_rbeg[a + k])
				fits = false;
		if (!fits)
		{
			g_lc[a++] = LC_BAD;
			continue;
		}
		g_lc[a] = LC_CODE;
		g_ilen[a] = (byte)in.len;
		for (k = 1; k < in.len; k++)
			g_lc[a + k] = LC_BODY;
		a += in.len;
	}
}

/* =====================================================================
 * Macros: a .macro defined in an include file stands for a sequence of
 * instructions. Turned on by name (--macro, ;@macro), a macro prints as
 * one line wherever the code matches its body, except where that would
 * hide something (see find_macros()). A named parameter (\name in the
 * body) stands for any value at its place; the values found become the
 * macro line's arguments.
 * ===================================================================== */

static char *nospace(const char *s);

#define MAC_MAXP 8      /* parameters per macro */
#define ARG_SLOT 100    /* binding slot of macro argument n (1-based): ARG_SLOT + n */

typedef struct Macro
{
	char *name;
	char *param[MAC_MAXP];
	int np;
	char **body;    /* instruction lines, as written */
	char **canon;   /* the same, compared form (made by mac_prepare()) */
	int n;
	bool on;
	char *src;      /* where it was defined, for messages */
	int line;
} Macro;

static Macro *g_mac = NULL;
static int g_nmac = 0, g_capmac = 0;
static int g_macAt[NADDR];              /* a macro line starts here: index + 1 */
static unsigned short g_macLen[NADDR];  /* bytes it covers */
static bool g_noMac[NADDR];             /* written out in full here */

static int mac_find(const char *name)
{
	int i;
	for (i = 0; i < g_nmac; i++)
		if (strcmp(g_mac[i].name, name) == 0)
			return i;
	return -1;
}

static int mac_on(const char *name, const char *what)
{
	int k = mac_find(name);
	if (k < 0)
		die("%s: no macro named '%s' is defined (macros come from include files, between\n"
			"  .macro %s and .endm)", what, name, name);
	g_mac[k].on = true;
	return k;
}

/* =====================================================================
 * Include files (.include "x.inc": names only)
 * ===================================================================== */

static char g_exeDir[PATH_MAX];

static char **g_incl = NULL;    /* top-level includes, as written (printed in listings) */
static int g_nincl = 0, g_capincl = 0;
static char **g_loaded = NULL;  /* every include loaded, nested ones too */
static int g_nloaded = 0, g_caploaded = 0;
static char **g_macOnly = NULL; /* includes that hold only macros */
static int g_nmacOnly = 0, g_capmacOnly = 0;

static char *dir_of(const char *path)
{
	const char *s = strrchr(path, '/');
	return s ? xstrndup(path, (size_t)(s - path)) : xstrdup(".");
}

static char *find_include(const char *name, const char *fromDir)
{
	const char *dirs[4];
	char *sfrDir = xasprintf("%s/sfr", g_exeDir);
	int i, n = 0;
	if (name[0] == '/')
		return xstrdup(name);
	if (fromDir)
		dirs[n++] = fromDir;
	dirs[n++] = sfrDir;
	dirs[n++] = g_exeDir;
	dirs[n++] = ".";
	for (i = 0; i < n; i++)
	{
		char *p = xasprintf("%s/%s", dirs[i], name);
		if (access(p, R_OK) == 0)
		{
			free(sfrDir);
			return p;
		}
		free(p);
	}
	free(sfrDir);
	return NULL;
}

/* Splits "body ; comment": terminates body, returns the comment text
 * (one leading blank removed) or NULL. Quotes protect ';'. */
static char *split_comment(char *line)
{
	char *p;
	char q = 0;
	for (p = line; *p; p++)
	{
		if (q)
		{
			if (*p == '\\' && p[1])
				p++;
			else if (*p == q)
				q = 0;
		}
		else if (*p == '"' || *p == '\'')
		{
			/* a quote right after a letter is a primed register (b') */
			if (*p == '\'' && p > line && isalpha((unsigned char)p[-1]))
				continue;
			q = *p;
		}
		else if (*p == ';')
		{
			*p = '\0';
			p++;
			if (*p == ' ')
				p++;
			return p;
		}
	}
	return NULL;
}

/* Parses ".equ NAME, EXPR" (leading blanks allowed). */
static bool parse_equ(const char *line, char *name, size_t namesz, char **expr)
{
	const char *p = skipws(line);
	const char *n0;
	if (strncmp(p, ".equ", 4) != 0 || !isspace((unsigned char)p[4]))
		return false;
	p = skipws(p + 4);
	n0 = p;
	if (!is_ident_start((unsigned char)*p))
		return false;
	while (is_ident_char((unsigned char)*p))
		p++;
	if ((size_t)(p - n0) >= namesz)
		return false;
	memcpy(name, n0, (size_t)(p - n0));
	name[p - n0] = '\0';
	p = skipws(p);
	if (*p != ',')
		return false;
	p = skipws(p + 1);
	if (!*p)
		return false;
	*expr = xstrdup(p);
	rstrip(*expr);
	return true;
}

static bool parse_include(const char *line, char *name, size_t namesz)
{
	const char *p = skipws(line);
	const char *e;
	if (strncmp(p, ".include", 8) != 0 || !isspace((unsigned char)p[8]))
		return false;
	p = skipws(p + 8);
	if (*p != '"')
		return false;
	e = strchr(p + 1, '"');
	if (!e || (size_t)(e - p - 1) >= namesz)
		return false;
	memcpy(name, p + 1, (size_t)(e - p - 1));
	name[e - p - 1] = '\0';
	return true;
}

/* Parses ".macro NAME [param[, param...]]" into mc's name and parameters. */
static bool parse_macro(const char *line, Macro *mc, int lineNo)
{
	const char *p = skipws(line);
	const char *n0;
	if (strncmp(p, ".macro", 6) != 0 || !isspace((unsigned char)p[6]))
		return false;
	p = skipws(p + 6);
	n0 = p;
	if (!is_ident_start((unsigned char)*p))
		die_at(lineNo, "expected a name after .macro");
	while (is_ident_char((unsigned char)*p))
		p++;
	mc->name = xstrndup(n0, (size_t)(p - n0));
	for (;;)
	{
		int j;
		p = skipws(p);
		if (*p == ',')
			p = skipws(p + 1);
		if (!*p)
			break;
		n0 = p;
		if (!is_ident_start((unsigned char)*p))
			die_at(lineNo, "a macro parameter must be a name: '%s'", line);
		while (is_ident_char((unsigned char)*p))
			p++;
		if (mc->np == MAC_MAXP)
			die_at(lineNo, "a macro can have at most %d parameters", MAC_MAXP);
		mc->param[mc->np] = xstrndup(n0, (size_t)(p - n0));
		for (j = 0; j < mc->np; j++)
			if (strcmp(mc->param[j], mc->param[mc->np]) == 0)
				die_at(lineNo, "the parameter '%s' is named twice", mc->param[j]);
		mc->np++;
	}
	return true;
}

/* The parameter of mc named at p (just after a backslash), or -1. */
static int mac_param_at(const Macro *mc, const char *p, size_t *len)
{
	const char *q = p;
	int j;
	while (is_ident_char((unsigned char)*q))   /* as vasm reads it: a dot doesn't end it */
		q++;
	*len = (size_t)(q - p);
	for (j = 0; mc && j < mc->np; j++)
		if (strlen(mc->param[j]) == *len && strncmp(mc->param[j], p, *len) == 0)
			return j;
	return -1;
}

/* Loads the .equ names of an include file. A name for FF00H-FFFFH is the
 * register's name wherever that address is data; a name for an address
 * outside every loaded image (resident RAM, another ROM's entry points) is
 * used wherever that address appears; other names only where the listing
 * writes them. top: named in the listing or database itself (and printed
 * there), not included by another include file. */
static void load_include(const char *name, const char *fromDir, bool required, bool top)
{
	char *path;
	char *text, **lines;
	int n, i;
	int names = 0, macros = 0;   /* .equ lines (or nested includes), .macro blocks */
	const char *saved = g_errFile;

	for (i = 0; i < g_nloaded; i++)
		if (strcmp(g_loaded[i], name) == 0)
		{
			/* already loaded: as a nested include, it no longer needs to be
			 * printed on its own (vasm would define its names twice) */
			if (!top)
				for (n = 0; n < g_nincl; n++)
					if (strcmp(g_incl[n], name) == 0)
					{
						memmove(&g_incl[n], &g_incl[n + 1], (size_t)(g_nincl - n - 1) * sizeof(char *));
						g_nincl--;
						break;
					}
			if (top)
			{
				bool listed = false;
				for (n = 0; n < g_nincl; n++)
					if (strcmp(g_incl[n], name) == 0)
						listed = true;
				if (!listed)
				{
					GROW(g_incl, g_nincl, g_capincl);
					g_incl[g_nincl++] = xstrdup(name);
				}
			}
			return;
		}
	path = find_include(name, fromDir);
	if (!path)
	{
		if (required)
			die("can't find the include file '%s' (looked next to the database or listing, in the\n"
				"  current directory and in %s/sfr)", name, g_exeDir);
		warn("can't find '%s'; register names won't be shown", name);
		return;
	}
	GROW(g_loaded, g_nloaded, g_caploaded);
	g_loaded[g_nloaded++] = xstrdup(name);
	if (top)
	{
		GROW(g_incl, g_nincl, g_capincl);
		g_incl[g_nincl++] = xstrdup(name);
	}

	text = read_file(path, NULL);
	if (!text)
		die("can't read '%s'", path);
	n = split_lines(text, &lines);
	g_errFile = path;
	for (i = 0; i < n; i++)
	{
		char nm[64];
		char *expr;
		char *ln = xstrdup(lines[i]);
		split_comment(ln);
		rstrip(ln);
		if (!*skipws(ln))
		{
			free(ln);
			continue;
		}
		if (parse_equ(ln, nm, sizeof(nm), &expr))
		{
			int k = sym_find(nm);
			if (k >= 0)
				die_at(i + 1, "'%s' is already defined (%s:%d)", nm, g_sym[k].src ? g_sym[k].src : "?", g_sym[k].line);
			k = sym_add(nm, SK_INC, path, i + 1);
			g_sym[k].expr = expr;
			names++;
		}
		else if (strncmp(skipws(ln), ".macro", 6) == 0)
		{
			Macro mcv, *mc;
			int j, m;
			memset(&mcv, 0, sizeof(mcv));
			if (!parse_macro(ln, &mcv, i + 1))
				die_at(i + 1, "expected '.macro NAME', optionally with parameters: '%s'", lines[i]);
			macros++;
			if (mac_find(mcv.name) >= 0)
				die_at(i + 1, "the macro '%s' is defined twice", mcv.name);
			for (m = 0; m < (int)(sizeof(mnemonic_names) / sizeof(mnemonic_names[0])); m++)
				if (strcasecmp(mcv.name, mnemonic_names[m]) == 0)
					die_at(i + 1, "'%s' is an instruction; give the macro another name", mcv.name);
			mcv.src = xstrdup(path);
			mcv.line = i + 1;
			for (j = i + 1; j < n; j++)
			{
				char *b = xstrdup(lines[j]);
				const char *t, *q;
				split_comment(b);
				rstrip(b);
				t = skipws(b);
				if (!*t)
				{
					free(b);
					continue;
				}
				if (strncmp(t, ".endm", 5) == 0 && (!t[5] || isspace((unsigned char)t[5])))
				{
					free(b);
					break;
				}
				if (*t == '.' || strchr(t, ':'))
					die_at(j + 1, "a macro body may only hold instructions (no labels or directives): '%s'",
						lines[j]);
				for (q = t; (q = strchr(q, '\\')) != NULL; q++)
				{
					size_t len;
					const char *dot;
					if (q[1] == '(' && q[2] == ')')
					{
						q += 2;   /* \() ends a parameter name, as in \byte\().3 */
						continue;
					}
					if (mac_param_at(&mcv, q + 1, &len) >= 0)
						continue;
					dot = memchr(q + 1, '.', len);
					if (dot)
					{
						size_t l2 = (size_t)(dot - (q + 1));
						int pj;
						for (pj = 0; pj < mcv.np; pj++)
							if (strlen(mcv.param[pj]) == l2 && strncmp(mcv.param[pj], q + 1, l2) == 0)
								die_at(j + 1, "vasm reads '\\%.*s' as one name; write '\\%s\\()%.*s' for the parameter\n"
									"  followed by a bit number", (int)len, q + 1, mcv.param[pj],
									(int)(len - l2), dot);
					}
					die_at(j + 1, "'\\%.*s' isn't a parameter of '%s'", (int)len, q + 1, mcv.name);
				}
				mcv.body = (char **)realloc(mcv.body, (size_t)(mcv.n + 1) * sizeof(char *));
				if (!mcv.body)
					die("out of memory");
				mcv.body[mcv.n++] = xstrdup(t);
				free(b);
			}
			if (j >= n)
				die_at(i + 1, "'.macro %s' has no .endm", mcv.name);
			if (mcv.n == 0)
				die_at(i + 1, "the macro '%s' is empty", mcv.name);
			for (m = 0; m < mcv.np; m++)
			{
				int b;
				bool used = false;
				for (b = 0; b < mcv.n; b++)
				{
					const char *q;
					for (q = mcv.body[b]; (q = strchr(q, '\\')) != NULL; q++)
					{
						size_t len;
						if (q[1] != '(' && mac_param_at(&mcv, q + 1, &len) == m)
							used = true;
					}
				}
				if (!used)
					die_at(i + 1, "the parameter '%s' of '%s' isn't used in its body", mcv.param[m], mcv.name);
			}
			GROW(g_mac, g_nmac, g_capmac);
			mc = &g_mac[g_nmac++];
			*mc = mcv;
			i = j;
		}
		else
		{
			char inc[256];
			if (parse_include(ln, inc, sizeof(inc)))
			{
				char *d = dir_of(path);
				load_include(inc, d, true, false);
				free(d);
				names++;
			}
			else
				die_at(i + 1, "an include file may only hold .equ lines, .macro blocks and comments: '%s'", lines[i]);
		}
		free(ln);
	}
	if (macros && !names)
	{
		GROW(g_macOnly, g_nmacOnly, g_capmacOnly);
		g_macOnly[g_nmacOnly++] = xstrdup(name);
	}
	g_errFile = saved;
	free(lines);
	free(text);
	free(path);
}

static void index_includes(void)
{
	int i;
	for (i = 0; i < g_nsym; i++)
	{
		long long v;
		if (g_sym[i].kind != SK_INC)
			continue;
		if (!sym_value(i, &v))
			die("%s:%d: can't evaluate '%s'", g_sym[i].src, g_sym[i].line, g_sym[i].expr);
		if (v >= 0xFF00 && v <= 0xFFFF && !g_incAt[v])
			g_incAt[v] = i + 1;
		else if (v >= 0 && v < 0xFF00 && !g_pop[v] && !g_extAt[v])
			g_extAt[v] = i + 1;
	}
}

/* =====================================================================
 * Names for addresses
 * ===================================================================== */

enum { CTX_DATA, CTX_CODE, CTX_WORD };

static bool g_noAuto = false;
static bool g_fresh = true;     /* no database: guess like a first look */

static int name_for(long v, int ctx)
{
	if (v < 0 || v >= NADDR)
		return -1;
	if (g_labelAt[v])
		return g_labelAt[v] - 1;
	if (g_globalAt[v])
		return g_globalAt[v] - 1;
	if (g_extAt[v])
		return g_extAt[v] - 1;
	if (ctx == CTX_DATA && g_incAt[v])
		return g_incAt[v] - 1;
	if (g_autoAt[v] && !g_noAuto)
		return g_autoAt[v] - 1;
	return -1;
}

static const char *use_name(int idx)
{
	g_sym[idx].used = true;
	return g_sym[idx].name;
}

/* Marks every name in an expression as used. */
static void mark_expr_names(const char *e)
{
	const char *p = e;
	while (*p)
	{
		if (*p == '\'' && p[1] && p[2] == '\'')
		{
			p += 3;
			continue;
		}
		if (isdigit((unsigned char)*p))
		{
			while (is_ident_char((unsigned char)*p))
				p++;
			continue;
		}
		if (is_ident_start((unsigned char)*p))
		{
			char nm[64];
			const char *q = p;
			int k;
			while (is_ident_char((unsigned char)*q))
				q++;
			if ((size_t)(q - p) < sizeof(nm))
			{
				memcpy(nm, p, (size_t)(q - p));
				nm[q - p] = '\0';
				k = sym_find(nm);
				if (k >= 0)
					g_sym[k].used = true;
			}
			p = q;
			continue;
		}
		p++;
	}
}

/* =====================================================================
 * Auto labels
 * ===================================================================== */

static byte g_autoPrio[NADDR];

static void want_auto(long t, int prio)
{
	if (t < 0 || t >= NADDR)
		return;
	if (g_autoPrio[t] < prio)
		g_autoPrio[t] = (byte)prio;
}

static void refs_of_insn(const Insn *in)
{
	int i;
	for (i = 0; i < in->n; i++)
	{
		const DOp *d = &in->ops[i];
		long v = d->value;
		switch (d->kind)
		{
		case K_CODE:
			/* a call, or an absolute BR that is itself the whole of a
			 * labeled entry (a trampoline), leads to a subroutine */
			want_auto(v, insn_is_call(in) || (d->form == DF_ABS && g_labelAt[in->addr]) ? AP_SUB : AP_LOC);
			break;
		case K_DATA:
		case K_INDEX:
			if (d->kind == K_DATA && d->form == DF_SFR)
				break;
			if (v >= 0xFF00)
				break;
			if (v >= 0xFE00)
				want_auto(v, AP_RAM);
			else if (g_pop[v])
				want_auto(v, AP_DAT);
			else
				want_auto(v, AP_EXT);
			break;
		case K_IMM:
			if (g_fresh && d->width == 16 && insn_imm_is_pointer(in) && v < NADDR && g_pop[v] && g_userData[v])
				want_auto(v, AP_DAT);
			break;
		default:
			break;
		}
	}
}

static void auto_from_expr(const char *e)
{
	const char *p = e;
	while (*p)
	{
		if (is_ident_start((unsigned char)*p))
		{
			char nm[64];
			const char *q = p;
			unsigned long a;
			while (is_ident_char((unsigned char)*q))
				q++;
			if ((size_t)(q - p) < sizeof(nm))
			{
				memcpy(nm, p, (size_t)(q - p));
				nm[q - p] = '\0';
				if (sym_find(nm) < 0 && auto_name_addr(nm, &a))
				{
					int pr = strncmp(nm, "sub_", 4) == 0 ? AP_SUB :
						strncmp(nm, "loc_", 4) == 0 ? AP_LOC :
						strncmp(nm, "ram_", 4) == 0 ? AP_RAM :
						strncmp(nm, "ext_", 4) == 0 ? AP_EXT : AP_DAT;
					want_auto((long)a, pr);
				}
			}
			p = q;
			continue;
		}
		if (isdigit((unsigned char)*p))
		{
			while (is_ident_char((unsigned char)*p))
				p++;
			continue;
		}
		p++;
	}
}

/* Collects references from code and pointer tables, then creates the
 * generated names. A user label or global name at an address wins. */
static void make_auto_labels(void)
{
	unsigned long a;
	int i;
	memset(g_autoPrio, 0, sizeof(g_autoPrio));
	memset(g_autoAt, 0, sizeof(g_autoAt));
	if (g_noAuto)
		return;
	for (a = 0; a < NADDR; a++)
	{
		if (g_lc[a] == LC_CODE)
		{
			Insn in;
			if (decode_insn(a, &in))
				refs_of_insn(&in);
		}
		else if (g_pop[a] && g_rt[a] == RT_PTR && g_pop[a + 1] && g_rt[a + 1] == RT_PTR &&
			((a - 0) & 0) == 0)
		{
			/* handled below, word by word from each ptr range start */
		}
	}
	/* pointer tables: every word is a code address */
	for (a = 0; a < NADDR; a++)
	{
		if (g_pop[a] && g_rt[a] == RT_PTR && (g_rbeg[a] || a == 0 || g_rt[a - 1] != RT_PTR || !g_pop[a - 1]))
		{
			unsigned long p = a;
			while (p + 1 < NADDR && g_pop[p] && g_pop[p + 1] && g_rt[p] == RT_PTR && g_rt[p + 1] == RT_PTR)
			{
				unsigned long w = g_mem[p] | (g_mem[p + 1] << 8);
				if (w != 0xFFFF)
					want_auto((long)w, AP_SUB);
				p += 2;
				if (g_rbeg[p])
					break;
			}
		}
	}
	for (i = 0; i < g_nbind; i++)
		auto_from_expr(g_bind[i].expr);

	for (a = 0; a < NADDR; a++)
	{
		char nm[16];
		int k;
		if (!g_autoPrio[a] || g_labelAt[a] || g_globalAt[a])
			continue;
		snprintf(nm, sizeof(nm), "%s_%04lX", AUTO_PREFIX[g_autoPrio[a]], a);
		k = sym_find(nm);
		if (k >= 0)
		{
			long long v;
			if (g_sym[k].kind == SK_AUTO)
				continue;
			if (sym_value(k, &v) && v == (long long)a)
				continue;   /* the user defined this name for this address */
			die("'%s' is defined (%s:%d) with a different value than its generated meaning (0x%04lX); rename it",
				nm, g_sym[k].src ? g_sym[k].src : "?", g_sym[k].line, a);
		}
		k = sym_add(nm, SK_AUTO, NULL, 0);
		g_sym[k].value = (long long)a;
		g_sym[k].resolved = true;
		g_autoAt[a] = k + 1;
	}
}

static int name_for(long v, int ctx);
static bool g_noAuto;

static unsigned long callt_word(int slot)
{
	unsigned long a = (unsigned long)g_calltBase + 2 * (unsigned long)slot;
	if (g_calltBase < 0 || a + 1 >= NADDR || !g_pop[a] || !g_pop[a + 1])
		return 0xFFFF;
	return g_mem[a] | (g_mem[a + 1] << 8);
}

/* The slot a generated-form CALLT name stands for, even one from an older
 * listing: callt_13, callt_ followed by a name of the routine in that
 * slot (callt_dac_write, callt_sub_8F22), or such a name with the slot
 * added (callt_dac_write_14). -1 if it is none of these. */
static int callt_slot_of_name(const char *name)
{
	unsigned long a;
	int k;
	if (g_calltBase < 0 || strncmp(name, "callt_", 6) != 0)
		return -1;
	name += 6;
	if (strlen(name) == 2 && isxdigit((unsigned char)name[0]) && isxdigit((unsigned char)name[1]) &&
		!islower((unsigned char)name[0]) && !islower((unsigned char)name[1]))
	{
		k = (int)strtol(name, NULL, 16);
		return k < 32 ? k : -1;
	}
	/* callt_NAME: the routine's own name comes first, so a routine whose
	 * name ends in _ and two hex digits (beep_0A) is not read as the
	 * callt_NAME_13 form below */
	k = sym_find(name);
	if (k >= 0 && (g_sym[k].kind == SK_LABEL || g_sym[k].kind == SK_AUTO ||
			((g_sym[k].kind == SK_INC || g_sym[k].kind == SK_EQU) && g_sym[k].resolved)))
		a = (unsigned long)g_sym[k].value;
	else if (k >= 0 || !auto_name_addr(name, &a))
		a = NADDR;
	if (a < NADDR)
		for (k = 0; k < 32; k++)
			if (callt_word(k) == a)
				return k;
	{
		/* callt_NAME_13: a second slot with the same routine */
		size_t n = strlen(name);
		if (n > 3 && name[n - 3] == '_' && isxdigit((unsigned char)name[n - 2]) &&
			isxdigit((unsigned char)name[n - 1]) && !islower((unsigned char)name[n - 2]) &&
			!islower((unsigned char)name[n - 1]))
		{
			k = (int)strtol(name + n - 2, NULL, 16);
			if (k < 32)
				return k;
		}
	}
	return -1;
}

/* Names each CALLT slot after the routine in it: callt_dac_write, or
 * callt_13 for an empty slot; a second slot with the same routine gets
 * the slot number added. Done after the address names exist. */
static void make_callt_names(void)
{
	int k;
	memset(g_calltAuto, 0, sizeof(g_calltAuto));
	if (g_calltBase < 0 || g_noAuto)
		return;
	for (k = 0; k < 32; k++)
	{
		unsigned long w = callt_word(k);
		int t = (w != 0xFFFF) ? name_for((long)w, CTX_CODE) : -1;
		char nm[96];
		int i;
		if (t >= 0)
			snprintf(nm, sizeof(nm), "callt_%s", g_sym[t].name);
		else
			snprintf(nm, sizeof(nm), "callt_%02X", k);
		i = sym_find(nm);
		if (i >= 0 && !(g_sym[i].kind == SK_CALLT && g_sym[i].value == k))
		{
			if (g_sym[i].kind != SK_CALLT)
				die("'%s' is defined (%s:%d) but is also the generated name of CALLT slot %02XH; rename it",
					nm, g_sym[i].src ? g_sym[i].src : "?", g_sym[i].line, k);
			snprintf(nm + strlen(nm), sizeof(nm) - strlen(nm), "_%02X", k);
			i = sym_find(nm);
		}
		if (i < 0)
		{
			i = sym_add(nm, SK_CALLT, NULL, 0);
			g_sym[i].value = k;
			g_sym[i].resolved = true;
		}
		g_calltAuto[k] = i + 1;
	}
}

static void add_anchor(unsigned long a)
{
	char nm[16];
	int k;
	if (name_for((long)a, CTX_WORD) >= 0)
		return;
	snprintf(nm, sizeof(nm), "%s_%04lX", g_lc[a] == LC_CODE ? "code" : "dat", a);
	k = sym_find(nm);
	if (k < 0)
	{
		k = sym_add(nm, SK_AUTO, NULL, 0);
		g_sym[k].value = (long long)a;
		g_sym[k].resolved = true;
	}
	g_autoAt[a] = k + 1;
}

/* =====================================================================
 * Printing
 * ===================================================================== */

enum { LK_GEN = 1, LK_ORG, LK_TEXT, LK_LABEL, LK_ALABEL, LK_EQU, LK_CONT, LK_CODE, LK_DATA, LK_MBYTES };

typedef struct Line
{
	long addr;          /* -1: no address column */
	char bytes[16];
	char *src;
	int kind;
	long at;            /* address the line belongs to (for messages) */
} Line;

typedef struct Out
{
	Line *v;
	int n, cap;
} Out;

static void out_line(Out *o, long addr, const char *bytes, const char *src, int kind, long at)
{
	Line *l;
	GROW(o->v, o->n, o->cap);
	l = &o->v[o->n++];
	l->addr = addr;
	snprintf(l->bytes, sizeof(l->bytes), "%s", bytes ? bytes : "");
	l->src = xstrdup(src);
	rstrip(l->src);
	l->kind = kind;
	l->at = at;
}

static void out_free(Out *o)
{
	int i;
	for (i = 0; i < o->n; i++)
		free(o->v[i].src);
	free(o->v);
	memset(o, 0, sizeof(*o));
}

/* Adds a line plus its end-of-line comment lines. */
static void out_with_eol(Out *o, long addr, const char *bytes, const char *src, int kind, long at, const char *eol)
{
	Str s = { 0 };
	const char *p;
	str_add(&s, src);
	if (!eol)
	{
		out_line(o, addr, bytes, str_c(&s), kind, at);
		free(s.s);
		return;
	}
	p = eol;
	for (;;)
	{
		const char *e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		str_pad(&s, COL_CMT);
		str_add(&s, n ? "; " : ";");
		str_addn(&s, p, n);
		out_line(o, addr, bytes, str_c(&s), kind, at);
		addr = -1;
		bytes = "";
		kind = LK_CONT;
		str_clear(&s);
		if (!e)
			break;
		p = e + 1;
	}
	free(s.s);
}

static void hex_bytes(char *buf, size_t sz, unsigned long a, unsigned long n)
{
	unsigned long i, show = n <= 5 ? n : 4;
	size_t len = 0;
	buf[0] = '\0';
	for (i = 0; i < show; i++)
		len += (size_t)snprintf(buf + len, sz - len, i ? " %02X" : "%02X", g_mem[a + i]);
	if (n > 5)
		snprintf(buf + len, sz - len, " ..");
}

/* Value text of a numeric operand with no binding. */
static void default_num(Str *o, const Insn *in, const DOp *d)
{
	int k;
	switch (d->kind)
	{
	case K_IMM:
		if (g_fresh && d->width == 16 && insn_imm_is_pointer(in) && d->value < NADDR &&
			g_userData[d->value] && (k = name_for(d->value, CTX_WORD)) >= 0)
			str_add(o, use_name(k));
		else
			str_addf(o, d->width == 8 ? "0x%02lX" : "0x%04lX", d->value);
		break;
	case K_DATA:
		if ((k = name_for(d->value, CTX_DATA)) >= 0)
			str_add(o, use_name(k));
		else if (d->form == DF_SFR)
			str_addf(o, "0x%02lX", d->value & 0xFF);
		else
			str_addf(o, "0x%04lX", d->value);
		break;
	case K_CODE:
		if ((k = name_for(d->value, CTX_CODE)) >= 0)
			str_add(o, use_name(k));
		else
			str_addf(o, "0x%04lX", d->value & 0xFFFF);
		break;
	case K_INDEX:
		if ((k = name_for(d->value, CTX_DATA)) >= 0)
			str_add(o, use_name(k));
		else
			str_addf(o, "0x%04lX", d->value);
		break;
	case K_DISP:
		str_addf(o, "0x%02lX", d->value);
		break;
	case K_CALLT:
		if (g_slotName[d->value & 31])
			str_add(o, use_name(g_slotName[d->value & 31] - 1));
		else if (g_calltAuto[d->value & 31])
			str_add(o, use_name(g_calltAuto[d->value & 31] - 1));
		else
			str_addf(o, "0x%02lX", d->value);
		break;
	default:
		str_addf(o, "%ld", d->value);
		break;
	}
}

static void op_text(Str *o, const Insn *in, int i, bool binds)
{
	const DOp *d = &in->ops[i];
	const Bind *b;
	if (d->kind == K_TEXT)
	{
		str_add(o, d->text);
		return;
	}
	str_add(o, d->pre);
	b = binds ? bind_find(in->addr, i + 1) : NULL;
	if (b)
	{
		str_add(o, b->expr);
		mark_expr_names(b->expr);
	}
	else
		default_num(o, in, d);
	str_add(o, d->post);
}

/* How an instruction is written: NEC's bit syntax puts the bit number
 * after the byte it belongs to ("set1 flags.3", "bt flags.3, label",
 * "mov1 cy, flags.3", "and1 cy, /flags.3"), while the decoded operands
 * keep it in a slot of its own (or attached to CY). The vasm backend
 * rewrites the NEC form into its own when it reads it. Bindings and the
 * database always use the decoded slot numbers. */
typedef struct View
{
	const char *mn;
	int n;          /* written operands */
	int slot[3];    /* decoded operand each written one shows */
	int bit[3];     /* decoded bit operand written after it as ".N", or -1 */
	bool cy[3];     /* written as plain "cy" */
	bool inv[3];    /* written with a leading '/' */
} View;

static void insn_view(const Insn *in, View *v)
{
	int i, bitIdx = -1, cyIdx = -1;
	bool inverted = in->op->mnemonic == ANDI1 || in->op->mnemonic == ORI1;
	v->mn = in->op->mnemonic == ANDI1 ? "and1" : in->op->mnemonic == ORI1 ? "or1" : insn_mnemonic(in);
	v->n = 0;
	for (i = 0; i < in->n; i++)
	{
		if (in->ops[i].kind == K_BIT)
			bitIdx = i;
		if (in->ops[i].kind == K_CYBIT)
			cyIdx = i;
	}
	for (i = 0; i < in->n; i++)
	{
		int n = v->n;
		if (i == bitIdx)
			continue;
		v->slot[n] = i;
		v->bit[n] = -1;
		v->cy[n] = v->inv[n] = false;
		if (i == cyIdx)
			v->cy[n] = true;
		else if (cyIdx >= 0)
		{
			v->bit[n] = cyIdx;
			v->inv[n] = inverted;
		}
		else if (bitIdx >= 0 && n == 0)
			v->bit[n] = bitIdx;
		v->n++;
	}
}

/* A bit number: its binding, or the number */
static void bit_text(Str *o, const Insn *in, int i, bool binds)
{
	const Bind *b = binds ? bind_find(in->addr, i + 1) : NULL;
	if (b)
	{
		str_add(o, b->expr);
		mark_expr_names(b->expr);
	}
	else
		str_addf(o, "%ld", in->ops[i].value);
}

static void wop_text(Str *o, const Insn *in, const View *v, int j, bool binds)
{
	if (v->cy[j])
	{
		str_add(o, "cy");
		return;
	}
	if (v->inv[j])
		str_add(o, "/");
	op_text(o, in, v->slot[j], binds);
	if (v->bit[j] >= 0)
	{
		str_add(o, ".");
		bit_text(o, in, v->bit[j], binds);
	}
}

/* "        mnemonic operands" */
static void insn_src(Str *o, const Insn *in, bool binds)
{
	View v;
	int j;
	insn_view(in, &v);
	str_add(o, INDENT);
	str_add(o, v.mn);
	if (v.n == 0)
		return;
	str_pad(o, COL_OPND);
	for (j = 0; j < v.n; j++)
	{
		if (j)
			str_add(o, ", ");
		wop_text(o, in, &v, j, binds);
	}
}

/* The instruction as text, without bindings and with single spaces:
 * the comment on a .byte line that stands in for it. */
static char *insn_plain(const Insn *in)
{
	Str s = { 0 };
	View v;
	int j;
	insn_view(in, &v);
	str_add(&s, v.mn);
	for (j = 0; j < v.n; j++)
	{
		str_add(&s, j ? ", " : " ");
		wop_text(&s, in, &v, j, false);
	}
	return s.s;
}

/* The value tokens of an instruction's compared form, in order: each
 * value, the text it was written as, and whether it is an sfr[] operand
 * (compared by its low byte). */
#define CT_MAX 24
typedef struct CTok
{
	int n;
	long long val[CT_MAX];
	char *src[CT_MAX];
	bool masked[CT_MAX];
} CTok;

static void ctok_free(CTok *ct)
{
	int i;
	for (i = 0; i < ct->n; i++)
		free(ct->src[i]);
	ct->n = 0;
}

/* A name or number character for mac_canon(): as is_ident_char(), but
 * a dot ends the word, so "ram_FEDA.2" is a byte and a bit number. */
static bool mac_word_char(int c)
{
	return isalnum(c) || c == '_';
}

/* Compared form of one instruction's text, for matching a macro body:
 * names and numbers become "$" and their value in hex, the rest is
 * lowercased and blanks are dropped. So "mov wdm, #0x00" and
 * "MOV WDM,#0" compare equal, and so do a name and the address it stands
 * for. In a body (mc set), \param becomes "\1" plus the parameter number.
 * ct (if given) collects the value tokens. */
static char *mac_canon(const char *text, const Macro *mc, CTok *ct)
{
	Str o = { 0 };
	char *t = xstrdup(text);
	const char *p;
	if (ct)
		ct->n = 0;
	split_comment(t);
	p = t;
	while (*p)
	{
		long long v = 0;
		bool val = false;
		const char *tok = p;
		if (isspace((unsigned char)*p))
		{
			p++;
			continue;
		}
		if (*p == '\\' && mc && p[1] == '(' && p[2] == ')')
		{
			p += 3;
			continue;
		}
		if (*p == '\\' && mc)
		{
			size_t len;
			int j = mac_param_at(mc, p + 1, &len);
			if (j >= 0)
			{
				str_addf(&o, "\x01%c", '1' + j);
				p += 1 + len;
				continue;
			}
		}
		if (isdigit((unsigned char)*p))
		{
			const char *q = p;
			char nb[32];
			size_t n;
			char *e;
			while (mac_word_char((unsigned char)*q))
				q++;
			n = (size_t)(q - p);
			if (n < sizeof(nb))
			{
				memcpy(nb, p, n);
				nb[n] = '\0';
				if (n > 2 && nb[0] == '0' && (nb[1] == 'x' || nb[1] == 'X'))
					v = strtoll(nb + 2, &e, 16);
				else if (nb[n - 1] == 'h' || nb[n - 1] == 'H')
				{
					nb[n - 1] = '\0';
					v = strtoll(nb, &e, 16);
				}
				else
					v = strtoll(nb, &e, 10);
				val = (*e == '\0');
			}
			if (!val)
			{
				for (; p < q; p++)
					str_addf(&o, "%c", tolower((unsigned char)*p));
				continue;
			}
			p = q;
		}
		else if (is_ident_start((unsigned char)*p) && *p != '.' && !(p > t && p[-1] == '\''))
		{
			const char *q = p;
			char nm[64];
			while (mac_word_char((unsigned char)*q))
				q++;
			if ((size_t)(q - p) < sizeof(nm))
			{
				memcpy(nm, p, (size_t)(q - p));
				nm[q - p] = '\0';
				val = lookup_cb(nm, &v, NULL);
			}
			if (!val)
			{
				for (; p < q; p++)
					str_addf(&o, "%c", tolower((unsigned char)*p));
				continue;
			}
			p = q;
		}
		else
		{
			str_addf(&o, "%c", tolower((unsigned char)*p));
			p++;
			continue;
		}
		{
			/* sfr[x] takes the register's low byte or its full address */
			bool masked = o.len >= 4 && strcmp(o.s + o.len - 4, "sfr[") == 0;
			if (masked)
				v &= 0xFF;
			str_addf(&o, "$%llX", (unsigned long long)v);
			if (ct && ct->n < CT_MAX)
			{
				ct->val[ct->n] = v;
				ct->src[ct->n] = xstrndup(tok, (size_t)(p - tok));
				ct->masked[ct->n] = masked;
				ct->n++;
			}
		}
	}
	free(t);
	return o.s ? o.s : xstrdup("");
}

/* What a macro's parameters were found to be at one site. */
typedef struct MacArgs
{
	bool have[MAC_MAXP];
	long long val[MAC_MAXP];
	bool masked[MAC_MAXP];
	char *src[MAC_MAXP];   /* the text the disassembler writes for it */
} MacArgs;

static void macargs_free(MacArgs *g)
{
	int i;
	for (i = 0; i < MAC_MAXP; i++)
		free(g->src[i]);
	memset(g, 0, sizeof(*g));
}

/* Compares a compared-form body line with an instruction's, filling in
 * parameters as they turn up (a parameter seen again must have the same
 * value). */
static bool mac_cmp(const char *body, const char *ins, const CTok *ct, MacArgs *g)
{
	int t = 0;   /* value tokens of ins passed */
	while (*body || *ins)
	{
		if (*body == '\x01')
		{
			int j = body[1] - '1';
			char *e;
			long long v;
			if (*ins != '$' || t >= ct->n)
				return false;
			v = strtoll(ins + 1, &e, 16);
			if (g->have[j] && g->val[j] != v)
				return false;
			if (!g->have[j])
			{
				g->have[j] = true;
				g->val[j] = v;
				g->masked[j] = ct->masked[t];
				g->src[j] = xstrdup(ct->src[t]);
			}
			ins = e;
			body += 2;
			t++;
			continue;
		}
		if (*body != *ins)
			return false;
		if (*ins == '$')
			t++;
		body++;
		ins++;
	}
	return true;
}

/* Whether something at address x (inside a would-be macro, after its
 * first byte) needs a line of its own: a label, comment or layout record. */
static bool mac_blocked(unsigned long x)
{
	return g_itemHead[x] || g_eol[x] || g_labelAt[x] || (g_autoAt[x] && !g_noAuto) ||
		g_rbeg[x] || g_brk[x];
}

/* Whether the code at a matches macro k; *len gets the bytes it covers
 * and g the parameter values. strict: also refuse where a line of its
 * own, or an operand written as a name that doesn't become an argument,
 * would be lost. On failure, *why (if given) says which instruction
 * differs. */
static bool mac_match(unsigned long a, int k, bool strict, unsigned long *len, char **why, MacArgs *g)
{
	Macro *mc = &g_mac[k];
	unsigned long x = a;
	int i;
	const Bind *bound[3 * 32];
	int nbound = 0;
	memset(g, 0, sizeof(*g));
	for (i = 0; i < mc->n; i++)
	{
		Insn in;
		Str s = { 0 };
		char *c;
		CTok ct;
		bool same;
		int slot;
		if (x >= NADDR || g_lc[x] != LC_CODE || !decode_insn(x, &in) || !insn_asm_ok(&in))
		{
			if (why)
				*why = xasprintf("instruction %d of the macro ('%s') has no matching instruction at 0x%04lX",
					i + 1, mc->body[i], x);
			macargs_free(g);
			return false;
		}
		if (strict && i > 0 && mac_blocked(x))
		{
			macargs_free(g);
			return false;
		}
		/* as printed, names and operand bindings included, so a name
		 * written in an operand becomes the argument */
		insn_src(&s, &in, true);
		c = mac_canon(str_c(&s), NULL, &ct);
		same = mac_cmp(mc->canon[i], c, &ct, g);
		if (!same && why)
		{
			char *plain = insn_plain(&in);
			*why = xasprintf("instruction %d is '%s' here, but the macro has '%s'", i + 1, plain, mc->body[i]);
			free(plain);
		}
		free(c);
		free(s.s);
		ctok_free(&ct);
		if (!same)
		{
			macargs_free(g);
			return false;
		}
		for (slot = 1; slot <= 3; slot++)
		{
			const Bind *b = bind_find(x, slot);
			if (b && nbound < (int)(sizeof(bound) / sizeof(bound[0])))
				bound[nbound++] = b;
		}
		if (strict)
		{
			unsigned long b;
			for (b = x + 1; b < x + (unsigned long)in.len; b++)
				if (mac_blocked(b))
				{
					macargs_free(g);
					return false;
				}
		}
		x += (unsigned long)in.len;
	}
	if (strict)
	{
		/* an operand written as a name must survive as an argument */
		int j, p;
		for (j = 0; j < nbound; j++)
		{
			bool kept = false;
			char *e = nospace(bound[j]->expr);
			for (p = 0; p < mc->np; p++)
			{
				char *q = g->src[p] ? nospace(g->src[p]) : NULL;
				if (q && strcmp(q, e) == 0)
					kept = true;
				free(q);
			}
			free(e);
			if (!kept)
			{
				macargs_free(g);
				return false;
			}
		}
	}
	*len = x - a;
	return true;
}

/* Makes the compared form of macro k's body. */
static void mac_prepare(int k)
{
	int i;
	if (!g_mac[k].canon)
	{
		g_mac[k].canon = (char **)calloc((size_t)g_mac[k].n, sizeof(char *));
		if (!g_mac[k].canon)
			die("out of memory");
	}
	for (i = 0; i < g_mac[k].n; i++)
	{
		free(g_mac[k].canon[i]);
		g_mac[k].canon[i] = mac_canon(g_mac[k].body[i], &g_mac[k], NULL);
	}
}

/* Marks where each macro that is on stands in for its instructions.
 * Left out: sites marked nomacro, and sites where a label, comment,
 * layout record or named operand inside the sequence needs a line of its
 * own. */
static void find_macros(void)
{
	unsigned long a;
	int k;
	bool any = false;
	memset(g_macAt, 0, sizeof(g_macAt));
	memset(g_macLen, 0, sizeof(g_macLen));
	for (k = 0; k < g_nmac; k++)
		if (g_mac[k].on)
		{
			any = true;
			mac_prepare(k);
		}
	if (!any)
		return;
	/* at each address the longest match wins; between equally long ones,
	 * the macro defined first */
	for (a = 0; a < NADDR; a++)
	{
		unsigned long len, best = 0;
		int bestK = -1;
		if (g_lc[a] != LC_CODE || g_noMac[a])
			continue;
		for (k = 0; k < g_nmac; k++)
		{
			MacArgs g;
			if (g_mac[k].on && mac_match(a, k, true, &len, NULL, &g))
			{
				macargs_free(&g);
				if (len > best)
				{
					best = len;
					bestK = k;
				}
			}
		}
		if (bestK >= 0)
		{
			g_macAt[a] = bestK + 1;
			g_macLen[a] = (unsigned short)best;
			a += best - 1;
		}
	}
}

static bool is_line_start(unsigned long a)
{
	return g_pop[a] && g_lc[a] != LC_BODY;
}

static bool has_annot(unsigned long a)
{
	return g_itemHead[a] || g_eol[a] || name_for((long)a, CTX_WORD) >= 0;
}

/* Whether a data line that started earlier must end before a. */
static bool data_breaks_at(unsigned long a, int type, int per)
{
	if (a >= NADDR || !g_pop[a] || g_rbeg[a] || g_brk[a] || g_rt[a] != type || rt_per(a) != per)
		return true;
	if (g_lc[a] != LC_NONE)
		return true;
	return has_annot(a);
}

/* Length of the 0xFF run at a that would print as one fill. */
static unsigned long fill_run(unsigned long a)
{
	unsigned long e = a;
	int type = g_rt[a], per = rt_per(a);
	if (type != RT_BYTE && type != RT_WORD)
		return 0;
	while (e < NADDR && g_pop[e] && g_mem[e] == 0xFF && g_lc[e] == LC_NONE)
	{
		if (e != a && data_breaks_at(e, type, per))
			break;
		e++;
	}
	return e - a;
}

/* Number of bytes the data line at a covers (not counting fills). */
static unsigned long data_line_len(unsigned long a)
{
	int type = g_rt[a], per = rt_per(a);
	unsigned long n = 0;
	int items = 0;
	int unit = (type == RT_WORD || type == RT_PTR) ? 2 : 1;

	if (unit == 2 && data_breaks_at(a + 1, type, per))
		return 1;   /* a lone byte where a word can't fit */
	while (items < per)
	{
		unsigned long x = a + n;
		if (n > 0 && (data_breaks_at(x, type, per) || (unit == 1 && type != RT_STRING && fill_run(x) >= FILL_MIN)))
			break;
		if (unit == 2 && (x + 1 >= NADDR || data_breaks_at(x + 1, type, per)))
			break;
		if (unit == 2 && n > 0 && fill_run(x) >= FILL_MIN)
			break;
		n += (unsigned long)unit;
		items++;
	}
	return n ? n : 1;
}

static void emit_org(Out *o, unsigned long a)
{
	char *s = xasprintf(INDENT ".org" "    0x%04lX", a);
	out_line(o, -1, "", s, LK_ORG, (long)a);
	free(s);
}

/* Lines before address a: comments, labels, .equ lines, then the
 * generated label. The .org that starts a block goes right before the
 * first label (a label before it would get the wrong value). */
static void emit_items(Out *o, unsigned long a, bool *needOrg, bool lineStart)
{
	int i;
	for (i = (a <= NADDR ? g_itemHead[a] : 0); i; i = g_item[i - 1].next)
	{
		Item *it = &g_item[i - 1];
		int k;
		if (it->kind == IT_TEXT)
		{
			out_line(o, -1, "", it->s, LK_TEXT, (long)a);
			continue;
		}
		k = sym_find(it->s);
		if (k < 0)
			continue;
		if (it->kind == IT_LABEL && lineStart)
		{
			char *s;
			if (*needOrg)
			{
				emit_org(o, a);
				*needOrg = false;
			}
			s = xasprintf("%s:", g_sym[k].name);
			out_with_eol(o, -1, "", s, LK_LABEL, (long)a, g_sym[k].comment);
			free(s);
			g_sym[k].lineDefined = true;
		}
		else
		{
			/* .equ, or a label whose address isn't the start of a line */
			char *s;
			if (it->kind == IT_LABEL)
				s = xasprintf(INDENT ".equ    %s, 0x%04lX", g_sym[k].name, a);
			else
				s = xasprintf(INDENT ".equ    %s, %s", g_sym[k].name, g_sym[k].expr);
			if (it->kind == IT_EQU)
				mark_expr_names(g_sym[k].expr);
			out_with_eol(o, -1, "", s, LK_EQU, (long)a, g_sym[k].comment);
			free(s);
			g_sym[k].lineDefined = true;
		}
	}
	if (a < NADDR && lineStart && g_autoAt[a] && !g_labelAt[a] && !g_noAuto)
	{
		int k = g_autoAt[a] - 1;
		char *s = xasprintf("%s:", g_sym[k].name);
		if (*needOrg)
		{
			emit_org(o, a);
			*needOrg = false;
		}
		out_line(o, -1, "", s, LK_ALABEL, (long)a);
		free(s);
		g_sym[k].lineDefined = true;
	}
	if (a < NADDR && lineStart && *needOrg)
	{
		emit_org(o, a);
		*needOrg = false;
	}
}

static char *join_eol(const char *first, const char *rest)
{
	if (!first)
		return rest ? xstrdup(rest) : NULL;
	if (!rest)
		return xstrdup(first);
	return xasprintf("%s\n%s", first, rest);
}

static void emit_code(Out *o, unsigned long a)
{
	Insn in;
	char bytes[16];
	Str s = { 0 };
	char *eol = NULL;
	unsigned long k;

	decode_insn(a, &in);
	hex_bytes(bytes, sizeof(bytes), a, (unsigned long)in.len);

	if (!insn_asm_ok(&in))
	{
		/* The assembler can't reproduce this one from text: write its
		 * bytes, with the instruction as the first comment line. */
		char *plain = insn_plain(&in);
		int j;
		str_add(&s, INDENT ".byte");
		str_pad(&s, COL_OPND);
		for (j = 0; j < in.len; j++)
			str_addf(&s, j ? ", 0x%02X" : "0x%02X", in.b[j]);
		eol = join_eol(plain, g_eol[a]);
		free(plain);
	}
	else
	{
		insn_src(&s, &in, true);
		eol = g_eol[a] ? xstrdup(g_eol[a]) : NULL;
	}
	/* comments filed at addresses inside the instruction */
	for (k = 1; k < (unsigned long)in.len; k++)
		if (g_eol[a + k])
		{
			char *n = join_eol(eol, g_eol[a + k]);
			free(eol);
			eol = n;
		}
	out_with_eol(o, (long)a, bytes, str_c(&s), LK_CODE, (long)a, eol);
	free(eol);
	free(s.s);
}

/* Marks the names a macro body uses (not its parameters) as used, so the
 * header defines them for the assembler. */
static void mark_body_names(const char *line)
{
	char *t = xstrdup(line);
	char *p;
	for (p = t; *p; p++)
		if (*p == '\\')
		{
			*p = ' ';
			while (is_ident_char((unsigned char)p[1]))
				*++p = ' ';
		}
	mark_expr_names(t);
	free(t);
}

/* A macro line: the macro's name and arguments with the first
 * instruction's bytes, then a line with just the address and bytes of
 * each instruction after it (the listing keeps every byte; --source
 * leaves those lines out). */
static void emit_macro(Out *o, unsigned long a)
{
	int k = g_macAt[a] - 1;
	Macro *mc = &g_mac[k];
	unsigned long x, end = a + g_macLen[a], len;
	char bytes[16];
	Str s = { 0 };
	MacArgs g;
	int i;
	mac_match(a, k, false, &len, NULL, &g);
	for (i = 0; i < mc->n; i++)
		mark_body_names(mc->body[i]);   /* vasm needs the names the body uses */
	str_add(&s, INDENT);
	str_add(&s, mc->name);
	for (i = 0; i < mc->np; i++)
	{
		const Bind *b = bind_find(a, ARG_SLOT + i + 1);
		const char *arg = b ? b->expr : (g.src[i] ? g.src[i] : "?");
		if (i == 0)
			str_pad(&s, COL_OPND);
		else
			str_add(&s, ", ");
		str_add(&s, arg);
		mark_expr_names(arg);
	}
	macargs_free(&g);
	hex_bytes(bytes, sizeof(bytes), a, g_ilen[a]);
	out_with_eol(o, (long)a, bytes, str_c(&s), LK_CODE, (long)a, g_eol[a]);
	free(s.s);
	for (x = a + g_ilen[a]; x < end; x += g_ilen[x])
	{
		hex_bytes(bytes, sizeof(bytes), x, g_ilen[x]);
		out_line(o, (long)x, bytes, "", LK_MBYTES, (long)x);
	}
}

/* A .word item with no binding: pointer tables name their targets,
 * plain words are numbers. */
static void word_default(Str *o, unsigned long w, int type, bool mark)
{
	int k;
	if (type == RT_PTR && (k = name_for((long)w, CTX_WORD)) >= 0)
		str_add(o, mark ? use_name(k) : g_sym[k].name);
	else
		str_addf(o, "0x%04lX", w);
}

/* Returns the number of bytes printed. *fillTo is set when the line is
 * a fill (so the caller starts a new block with .org). */
static unsigned long emit_data(Out *o, unsigned long a, unsigned long *fillTo)
{
	int type = g_rt[a];
	char bytes[16];
	Str s = { 0 };
	unsigned long n, i;
	unsigned long run;

	*fillTo = 0;
	if (g_lc[a] == LC_BAD)
		type = RT_BYTE;

	if (g_lc[a] == LC_NONE && (run = fill_run(a)) >= FILL_MIN)
	{
		unsigned long e = a + run;
		const char *target;
		char endname[16];
		bool atEnd = !(e < NADDR && g_pop[e]);
		if (atEnd)
		{
			int k;
			snprintf(endname, sizeof(endname), "image_end_%04lX", e);
			k = sym_find(endname);
			if (k < 0)
			{
				k = sym_add(endname, SK_AUTO, NULL, 0);
				g_sym[k].value = (long long)e;
				g_sym[k].resolved = true;
			}
			target = use_name(k);
		}
		else
		{
			int k = name_for((long)e, CTX_WORD);
			target = k >= 0 ? use_name(k) : "?";
		}
		hex_bytes(bytes, sizeof(bytes), a, run);
		str_add(&s, INDENT ".space");
		str_pad(&s, COL_OPND);
		str_addf(&s, "%s-$, 0xFF", target);
		out_with_eol(o, (long)a, bytes, str_c(&s), LK_DATA, (long)a, g_eol[a]);
		free(s.s);
		if (!atEnd)
			*fillTo = e;
		return run;
	}

	n = (g_lc[a] == LC_BAD) ? 1 : data_line_len(a);
	hex_bytes(bytes, sizeof(bytes), a, n);

	if (type == RT_STRING && g_lc[a] == LC_NONE)
	{
		str_add(&s, INDENT ".ascii");
		str_pad(&s, COL_OPND);
		str_add(&s, "\"");
		for (i = 0; i < n; i++)
		{
			byte c = g_mem[a + i];
			if (c == '"' || c == '\\')
				str_addf(&s, "\\%c", c);
			else if (c >= 32 && c < 127)
				str_addf(&s, "%c", c);
			else
				str_addf(&s, "\\x%02X", c);
		}
		str_add(&s, "\"");
	}
	else if ((type == RT_WORD || type == RT_PTR) && n >= 2 && g_lc[a] == LC_NONE)
	{
		str_add(&s, INDENT ".word");
		str_pad(&s, COL_OPND);
		for (i = 0; i < n; i += 2)
		{
			unsigned long w = g_mem[a + i] | (g_mem[a + i + 1] << 8);
			const Bind *b = bind_find(a + i, 1);
			if (i)
				str_add(&s, ", ");
			if (b)
			{
				str_add(&s, b->expr);
				mark_expr_names(b->expr);
			}
			else
				word_default(&s, w, type, true);
		}
	}
	else
	{
		str_add(&s, INDENT ".byte");
		str_pad(&s, COL_OPND);
		for (i = 0; i < n; i++)
		{
			const Bind *b = bind_find(a + i, 1);
			if (i)
				str_add(&s, ", ");
			if (b)
			{
				str_add(&s, b->expr);
				mark_expr_names(b->expr);
			}
			else
				str_addf(&s, "0x%02X", g_mem[a + i]);
		}
	}
	out_with_eol(o, (long)a, bytes, str_c(&s), LK_DATA, (long)a, g_eol[a]);
	free(s.s);
	return n;
}

/* Creates fill anchors: a fill runs up to a label, so the address it
 * stops at needs a name. Done before printing so the names exist. */
static void make_fill_anchors(void)
{
	unsigned long a = 0;
	while (a < NADDR)
	{
		unsigned long run;
		if (!g_pop[a] || g_lc[a] != LC_NONE)
		{
			a++;
			continue;
		}
		run = fill_run(a);
		if (run >= FILL_MIN)
		{
			unsigned long e = a + run;
			if (e < NADDR && g_pop[e] && !g_noAuto)
				add_anchor(e);
			a = e;
		}
		else
			a += (g_rt[a] == RT_CODE) ? 1 : data_line_len(a);
	}
}

static void emit_body(Out *o)
{
	unsigned long a = 0;
	bool needOrg = true;
	int i;

	for (i = 0; i < g_nsym; i++)
	{
		g_sym[i].used = false;
		g_sym[i].lineDefined = false;
	}
	while (a < NADDR)
	{
		unsigned long n, fillTo;
		if (!g_pop[a])
		{
			if (!needOrg)
				needOrg = true;
			a++;
			continue;
		}
		emit_items(o, a, &needOrg, true);
		if (g_lc[a] == LC_CODE && g_macAt[a])
		{
			emit_macro(o, a);
			n = g_macLen[a];
		}
		else if (g_lc[a] == LC_CODE)
		{
			unsigned long k;
			for (k = 1; k < g_ilen[a]; k++)
			{
				bool dummy = false;
				emit_items(o, a + k, &dummy, false);
			}
			emit_code(o, a);
			n = g_ilen[a];
		}
		else
		{
			n = emit_data(o, a, &fillTo);
			if (fillTo)
				needOrg = true;
		}
		a += n;
	}
	{
		bool dummy = false;
		emit_items(o, END_SLOT, &dummy, false);
	}
}

static int cmp_sym_value(const void *x, const void *y)
{
	const Sym *a = &g_sym[*(const int *)x], *b = &g_sym[*(const int *)y];
	if ((a->kind == SK_CALLT) != (b->kind == SK_CALLT))
		return a->kind == SK_CALLT ? 1 : -1;
	if (a->value != b->value)
		return a->value < b->value ? -1 : 1;
	return strcmp(a->name, b->name);
}

static const char *const HDR_END = "; ---- End of generated header ----";

/* The whole listing: generated header (ROMs, includes, names used but
 * defined outside the listing), then the body. */
static void emit_listing(Out *o)
{
	Out body = { 0 };
	int *gen = NULL, ngen = 0, capgen = 0;
	int i;

	find_macros();
	make_fill_anchors();
	emit_body(&body);

	out_line(o, -1, "", "; Generated by 78k_disasm. After editing, rebuild the database with:", LK_GEN, -1);
	out_line(o, -1, "", ";   78k_disasm --extract <this listing> --db <database.inc>", LK_GEN, -1);
	for (i = 0; i < g_nimg; i++)
	{
		char *s = xasprintf("; ROM 0x%04lX-0x%04lX, %lu bytes, CRC-32 0x%08lX",
			g_img[i].base, g_img[i].base + g_img[i].size - 1, g_img[i].size, g_img[i].crc);
		out_line(o, -1, "", s, LK_GEN, -1);
		free(s);
	}
	if (g_calltBase >= 0)
	{
		char *s = xasprintf("; CALLT table at 0x%04lX", (unsigned long)g_calltBase);
		out_line(o, -1, "", s, LK_GEN, -1);
		free(s);
	}
	for (i = 0; i < g_nincl; i++)
	{
		char *s = xasprintf(INDENT ".include \"%s\"", g_incl[i]);
		out_line(o, -1, "", s, LK_GEN, -1);
		free(s);
	}
	for (i = 0; i < g_nsym; i++)
	{
		Sym *s = &g_sym[i];
		if (!s->used || s->lineDefined || s->kind == SK_INC)
			continue;
		if (s->kind == SK_EQU)
			continue;   /* defined by its own .equ line in the body */
		GROW(gen, ngen, capgen);
		gen[ngen++] = i;
	}
	if (ngen > 1)
		qsort(gen, (size_t)ngen, sizeof(int), cmp_sym_value);
	for (i = 0; i < ngen; i++)
	{
		char *s = xasprintf(g_sym[gen[i]].kind == SK_CALLT ? INDENT ".equ    %s, 0x%02llX" : INDENT ".equ    %s, 0x%04llX",
			g_sym[gen[i]].name, g_sym[gen[i]].value);
		out_line(o, -1, "", s, LK_GEN, -1);
		free(s);
	}
	free(gen);
	out_line(o, -1, "", HDR_END, LK_GEN, -1);

	for (i = 0; i < body.n; i++)
	{
		GROW(o->v, o->n, o->cap);
		o->v[o->n++] = body.v[i];
	}
	free(body.v);
}

/* =====================================================================
 * Output files
 * ===================================================================== */

static const char *g_outs[4];
static int g_nouts = 0;

static void refuse_existing(const char *path)
{
	die("refusing to overwrite '%s': it already exists. Delete it first, or choose another name.", path);
}

/* The file a path names, for comparing two paths that don't exist yet:
 * its directory resolved, plus the last part. */
static char *out_identity(const char *path)
{
	char buf[PATH_MAX];
	char *d = dir_of(path);
	const char *b = strrchr(path, '/');
	char *r;
	b = b ? b + 1 : path;
	r = realpath(d, buf) ? xasprintf("%s/%s", buf, b) : xstrdup(path);
	free(d);
	return r;
}

/* Every output file must be new and named only once. Checked before any
 * work is done, so a mistake costs nothing and no output is left half
 * written. */
static void check_outputs(void)
{
	int i, j;
	struct stat st;
	for (i = 0; i < g_nouts; i++)
	{
		char *a;
		if (stat(g_outs[i], &st) == 0)
			refuse_existing(g_outs[i]);
		a = out_identity(g_outs[i]);
		for (j = 0; j < i; j++)
		{
			char *b = out_identity(g_outs[j]);
			if (strcmp(a, b) == 0)
				die("'%s' is named for two outputs; give each output its own file.", g_outs[i]);
			free(b);
		}
		free(a);
	}
}

static void add_output(const char *path)
{
	if (path && g_nouts < 4)
		g_outs[g_nouts++] = path;
}

static FILE *open_out(const char *path)
{
	int fd;
	FILE *fp;
	if (!path)
		return stdout;
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0)
	{
		if (errno == EEXIST)
			refuse_existing(path);
		die("can't write '%s': %s", path, strerror(errno));
	}
	fp = fdopen(fd, "w");
	if (!fp)
		die("can't write '%s': %s", path, strerror(errno));
	return fp;
}

static void close_out(FILE *fp, const char *path)
{
	if (fp == stdout)
	{
		fflush(fp);
		return;
	}
	if (fclose(fp) != 0)
		die("error writing '%s': %s", path, strerror(errno));
}

static void render_line(Str *s, const Line *l, bool columns)
{
	str_clear(s);
	if (columns)
	{
		if (l->addr >= 0)
			str_addf(s, "%04lX  %-14s  ", (unsigned long)l->addr, l->bytes);
		else
			str_add(s, "                      ");
	}
	str_add(s, l->src);
	while (s->len && s->s[s->len - 1] == ' ')
		s->s[--s->len] = '\0';
}

static void write_listing(const Out *o, const char *path, bool columns)
{
	FILE *fp = open_out(path);
	Str s = { 0 };
	int i;
	for (i = 0; i < o->n; i++)
	{
		if (!columns && o->v[i].kind == LK_MBYTES)
			continue;   /* a macro's further bytes: only the listing shows them */
		render_line(&s, &o->v[i], columns);
		fputs(str_c(&s), fp);
		fputc('\n', fp);
	}
	free(s.s);
	close_out(fp, path);
}

/* =====================================================================
 * Fresh analysis (no database): walk code from entry points
 * ===================================================================== */

typedef struct Vec
{
	const char *name;
	unsigned long slot;
} Vec;

/* Fixed 78K/III vector table (NEC datasheet Table 2), 0000H-003FH. */
static const Vec VECTORS[] = {
	{ "RESET", 0x00 }, { "NMI", 0x02 }, { "EXIF0", 0x04 }, { "EXIF1", 0x06 },
	{ "EXIF2", 0x08 }, { "WDT", 0x0A }, { "TBF", 0x0C }, { "TMF0", 0x0E },
	{ "TMF1", 0x10 }, { "TMF2", 0x12 }, { "CRF00", 0x1A }, { "CRF01", 0x1C },
	{ "CRF10", 0x1E }, { "CRF11", 0x20 }, { "SEF", 0x22 }, { "SRF", 0x24 },
	{ "STF", 0x26 }, { "ADF", 0x28 }, { "BRK", 0x3E },
};
#define NVEC (sizeof(VECTORS) / sizeof(VECTORS[0]))

static unsigned long *g_entries = NULL;
static int g_nentries = 0, g_capentries = 0;
static bool g_isEntry[NADDR];
static bool g_entrySep[NADDR];   /* starts a routine: gets a separator */
static bool g_follow = false;

static void add_entry(unsigned long a, bool sep)
{
	if (a >= NADDR)
		return;
	if (sep)
		g_entrySep[a] = true;
	if (g_isEntry[a])
		return;
	g_isEntry[a] = true;
	GROW(g_entries, g_nentries, g_capentries);
	g_entries[g_nentries++] = a;
}

static unsigned long erased_run(unsigned long a)
{
	unsigned long e = a;
	while (e < NADDR && g_pop[e] && g_mem[e] == 0xFF)
		e++;
	return e - a;
}

/* Walks code from start: stops at a return (or, with -f, an
 * unconditional BR), at another entry point, at data, at erased ROM, or
 * where memory ends. Returns the call/branch targets it found. */
static void walk(unsigned long start)
{
	unsigned long a = start;
	while (a < NADDR && g_pop[a])
	{
		Insn in;
		int k;
		bool clash = false;
		if (a != start && g_isEntry[a])
			break;
		if (g_rt[a] != RT_NONE && g_rt[a] != RT_CODE)
			break;
		if (g_lc[a] == LC_CODE || g_lc[a] == LC_BODY)
			break;
		if (erased_run(a) >= FILL_MIN)
			break;
		if (!decode_insn(a, &in))
			break;
		for (k = 1; k < in.len; k++)
			if (g_isEntry[a + k] || g_lc[a + k] != LC_NONE || (g_rt[a + k] != RT_NONE && g_rt[a + k] != RT_CODE))
				clash = true;
		if (clash)
			break;
		g_lc[a] = LC_CODE;
		g_ilen[a] = (byte)in.len;
		for (k = 1; k < in.len; k++)
			g_lc[a + k] = LC_BODY;
		if (g_follow)
		{
			for (k = 0; k < in.n; k++)
				if (in.ops[k].kind == K_CODE && in.ops[k].value >= 0 && in.ops[k].value < NADDR)
					add_entry((unsigned long)in.ops[k].value,
						insn_is_call(&in) || (in.ops[k].absolute && g_entrySep[a] && insn_is_jump(&in)));
		}
		a += in.len;
		if (insn_is_return(&in) || (g_follow && insn_is_jump(&in) && in.ops[0].kind == K_CODE))
			break;
	}
}

static const char *const RULE = "; ------------------------------------------------------------";

/* Builds layout and first-look annotations without a database. */
static void fresh_analysis(void)
{
	unsigned long a;
	int i;

	/* no entry points at all: decode everything as code, start to end,
	 * except erased runs and -k ranges */
	if (g_nentries == 0)
	{
		a = 0;
		while (a < NADDR)
		{
			unsigned long run;
			if (!g_pop[a] || g_rt[a] != RT_NONE)
			{
				a++;
				continue;
			}
			run = erased_run(a);
			if (run >= FILL_MIN)
			{
				unsigned long e = a + run;
				for (; a < e; a++)
				{
					if (g_rt[a] != RT_NONE)
						break;
					g_rt[a] = RT_BYTE;
					g_rbeg[a] = (a == e - run) || (a > 0 && g_rt[a - 1] != RT_BYTE);
				}
				continue;
			}
			g_rbeg[a] = (a == 0 || !g_pop[a - 1] || g_rt[a - 1] != RT_CODE);
			g_rt[a++] = RT_CODE;
		}
		for (a = 0; a < NADDR; a++)
			if (g_pop[a] && g_rt[a] != RT_NONE && a + 1 < NADDR && g_pop[a + 1] && g_rt[a + 1] != g_rt[a])
				g_rbeg[a + 1] = true;
		decode_layout();
		return;
	}

	/* walk every entry; with -f the list grows as targets are found */
	for (i = 0; i < g_nentries; i++)
		walk(g_entries[i]);

	/* separators and labels at entry points */
	for (i = 0; i < g_nentries; i++)
	{
		unsigned long e = g_entries[i];
		if (!g_pop[e] || g_lc[e] != LC_CODE || !g_entrySep[e])
			continue;
		bool hasRule = false;
		int it;
		for (it = g_itemHead[e]; it; it = g_item[it - 1].next)
			if (g_item[it - 1].kind == IT_TEXT && strcmp(g_item[it - 1].s, RULE) == 0)
				hasRule = true;
		if (!hasRule)
		{
			/* insert a separator before any existing items */
			int old = g_itemHead[e], oldTail = g_itemTail[e];
			g_itemHead[e] = g_itemTail[e] = 0;
			item_add(e, IT_TEXT, "");
			item_add(e, IT_TEXT, RULE);
			if (old)
			{
				g_item[g_itemTail[e] - 1].next = old;
				g_itemTail[e] = oldTail;
			}
		}
	}

	/* everything walked is code; the rest is data (byte unless -k said) */
	for (a = 0; a < NADDR; a++)
	{
		if (!g_pop[a])
			continue;
		if (g_lc[a] == LC_CODE || g_lc[a] == LC_BODY)
		{
			if (g_rt[a] != RT_CODE)
			{
				g_rt[a] = RT_CODE;
				g_rper[a] = 0;
			}
			if (g_lc[a] == LC_CODE && (a == 0 || !(g_lc[a - 1] == LC_CODE || g_lc[a - 1] == LC_BODY) || !g_pop[a - 1]))
				g_rbeg[a] = true;
		}
		else if (g_rt[a] == RT_NONE || g_rt[a] == RT_CODE)
		{
			g_rt[a] = RT_BYTE;
			g_rper[a] = 0;
			if (a == 0 || !g_pop[a - 1] || g_rt[a - 1] != RT_BYTE)
				g_rbeg[a] = true;
		}
	}
	decode_layout();
}

/* -k START-END[:format] */
static void apply_data_option(const char *spec)
{
	char *s = xstrdup(spec);
	char *colon = strchr(s, ':');
	char *dash;
	unsigned long st, en, a;
	int type = RT_BYTE, per = 0;
	bool ok1, ok2;

	if (colon)
	{
		char *slash = strchr(colon + 1, '/');
		*colon = '\0';
		if (slash)
		{
			*slash = '\0';
			per = atoi(slash + 1);
			if (per <= 0 || per > 255)
				die("-k %s: the count after '/' must be 1-255", spec);
		}
		if (!strcmp(colon + 1, "byte")) type = RT_BYTE;
		else if (!strcmp(colon + 1, "word")) type = RT_WORD;
		else if (!strcmp(colon + 1, "string")) type = RT_STRING;
		else if (!strcmp(colon + 1, "ptr")) type = RT_PTR;
		else
			die("-k %s: unknown format '%s' (use byte, word, string or ptr, e.g. -k 0x1E00-0x1EA4:string/8)",
				spec, colon + 1);
	}
	dash = strchr(s, '-');
	if (!dash)
		die("-k %s: expected START-END, e.g. -k 0x08D7-0x08F5:string", spec);
	*dash = '\0';
	st = parse_hex_addr(s, &ok1);
	en = parse_hex_addr(dash + 1, &ok2);
	if (!ok1 || !ok2 || st >= en || en > NADDR)
		die("-k %s: expected START-END with START below END, e.g. -k 0x08D7-0x08F5", spec);
	for (a = st; a < en; a++)
	{
		g_rt[a] = (byte)type;
		g_rper[a] = (unsigned short)per;
		g_userData[a] = true;
		g_rbeg[a] = (a == st);
	}
	if (en < NADDR)
		g_rbeg[en] = true;
	free(s);
}

/* =====================================================================
 * Database: read
 * ===================================================================== */

typedef struct RomRec
{
	unsigned long base, size, crc;
} RomRec;

static RomRec *g_dbRom = NULL;
static int g_ndbRom = 0, g_capdbRom = 0;
static bool g_ignoreCrc = false;

static int parse_rt(const char *s, int *per)
{
	const char *slash = strchr(s, '/');
	size_t n = slash ? (size_t)(slash - s) : strlen(s);
	int t;
	*per = slash ? atoi(slash + 1) : 0;
	for (t = RT_CODE; t <= RT_PTR; t++)
		if (strlen(RT_NAME[t]) == n && strncmp(s, RT_NAME[t], n) == 0)
			return t;
	return RT_NONE;
}

static void load_db(const char *path)
{
	char *text = read_file(path, NULL);
	char **lines;
	int n, i;
	int lastEqu = -1;
	char *dbDir;
	unsigned long a;
	unsigned long curStart = 0;
	bool haveRange = false;

	if (!text)
		die("can't read the database '%s': %s", path, strerror(errno));
	dbDir = dir_of(path);
	g_errFile = path;
	n = split_lines(text, &lines);

	for (i = 0; i < n; i++)
	{
		char *ln = lines[i];
		const char *p = skipws(ln);

		if (strncmp(p, ";@", 2) == 0)
		{
			char kw[16];
			lastEqu = -1;
			p += 2;
			if (strncmp(p, "rom ", 4) == 0)
			{
				RomRec r;
				if (sscanf(p + 4, "%lx %lu %lx", &r.base, &r.size, &r.crc) != 3 || r.size == 0 ||
					r.base >= NADDR || r.size > NADDR - r.base)
					die_at(i + 1, "bad ;@rom line");
				GROW(g_dbRom, g_ndbRom, g_capdbRom);
				g_dbRom[g_ndbRom++] = r;
				continue;
			}
			if (strncmp(p, "callt ", 6) == 0)
			{
				char *e;
				g_calltBase = (long)strtoul(skipws(p + 6), &e, 16);
				if (g_calltBase < 0 || g_calltBase + 0x3F >= NADDR)
					die_at(i + 1, "bad ;@callt address");
				continue;
			}
			if (strncmp(p, "slot ", 5) == 0)
			{
				int k = sym_find(skipws(p + 5));
				long long v;
				if (k < 0 || g_sym[k].kind != SK_EQU || !sym_value(k, &v) || v < 0 || v > 31)
					die_at(i + 1, "';@slot %s' names no .equ with a CALLT slot value (0-1FH)", skipws(p + 5));
				g_slotName[v] = k + 1;
				continue;
			}
			if (strncmp(p, "macro ", 6) == 0)
			{
				mac_on(skipws(p + 6), ";@macro");
				continue;
			}
			if (strncmp(p, "global ", 7) == 0)
			{
				int k = sym_find(skipws(p + 7));
				if (k < 0 || g_sym[k].kind != SK_EQU)
					die_at(i + 1, "';@global %s' names no .equ", skipws(p + 7));
				g_sym[k].global = true;
				continue;
			}
			if (*p != ' ')
				die_at(i + 1, "unknown database line '%s'", ln);
			p = skipws(p);
			if (strncmp(p, "end", 3) == 0 && p[3] == ' ')
			{
				a = END_SLOT;
				p += 4;
			}
			else
			{
				char *e;
				if (strncmp(p, "0x", 2) != 0)
					die_at(i + 1, "expected an address in '%s'", ln);
				a = strtoul(p + 2, &e, 16);
				if (e == p + 2 || *e != ' ' || a >= NADDR)
					die_at(i + 1, "bad address in '%s'", ln);
				p = e + 1;
			}
			if (*p == ';')
			{
				const char *t = p + 1;
				if (*t == ' ')
					t++;
				if (a == END_SLOT)
					die_at(i + 1, "a comment needs a line address");
				eol_add(a, t);
				continue;
			}
			if (sscanf(p, "%15s", kw) != 1)
				die_at(i + 1, "empty database record");
			p += strlen(kw);
			if (*p == ' ')
				p++;
			if (!strcmp(kw, "text"))
				item_add(a, IT_TEXT, p);
			else if (!strcmp(kw, "label") || !strcmp(kw, "equ"))
			{
				int k = sym_find(p);
				if (k < 0)
					die_at(i + 1, "'%s' has no .equ line in the symbols part", p);
				if (!strcmp(kw, "label"))
				{
					long long v;
					if (a == END_SLOT || !sym_value(k, &v) || v != (long long)a)
						die_at(i + 1, "label '%s' doesn't match its .equ value", p);
					if (g_labelAt[a])
						die_at(i + 1, "two labels at 0x%04lX ('%s' and '%s')", a, g_sym[g_labelAt[a] - 1].name, p);
					set_label(k, a);
					item_add(a, IT_LABEL, p);
				}
				else
					item_add(a, IT_EQU, p);
			}
			else if (!strcmp(kw, "op"))
			{
				int slot;
				char *e;
				slot = (int)strtol(p, &e, 10);
				if (e == p || *e != ' ' || slot < 1 || slot > 3 || a == END_SLOT)
					die_at(i + 1, "bad 'op' record");
				bind_add(a, slot, skipws(e));
			}
			else if (!strcmp(kw, "code") || !strcmp(kw, "data"))
			{
				int per = 0, t = RT_CODE;
				if (a == END_SLOT)
					die_at(i + 1, "a range needs an address");
				if (!strcmp(kw, "data"))
				{
					t = parse_rt(p, &per);
					if (t == RT_NONE || t == RT_CODE)
						die_at(i + 1, "unknown data format '%s'", p);
				}
				if (haveRange && a < curStart)
					die_at(i + 1, "ranges must be in address order");
				g_rbeg[a] = true;
				g_rt[a] = (byte)t;
				g_rper[a] = (unsigned short)per;
				curStart = a;
				haveRange = true;
			}
			else if (!strcmp(kw, "break"))
			{
				if (a == END_SLOT)
					die_at(i + 1, "a break needs an address");
				g_brk[a] = true;
			}
			else if (!strcmp(kw, "arg"))
			{
				int n;
				char *e;
				n = (int)strtol(p, &e, 10);
				if (e == p || *e != ' ' || n < 1 || n > MAC_MAXP || a == END_SLOT)
					die_at(i + 1, "bad 'arg' record");
				bind_add(a, ARG_SLOT + n, skipws(e));
			}
			else if (!strcmp(kw, "nomacro"))
			{
				if (a == END_SLOT)
					die_at(i + 1, "'nomacro' needs an address");
				g_noMac[a] = true;
			}
			else
				die_at(i + 1, "unknown database record '%s'", kw);
			continue;
		}

		/* symbols part (plain vasm source) */
		{
			char *ln2 = xstrdup(ln);
			char *cm = split_comment(ln2);
			char nm[64], inc[256];
			char *expr;
			rstrip(ln2);
			if (!*skipws(ln2))
			{
				/* comment-only line: continues the previous .equ's comment
				 * when indented */
				if (cm && lastEqu >= 0 && (ln[0] == ' ' || ln[0] == '\t'))
				{
					char *c = xstrdup(cm);
					rstrip(c);
					comment_append(&g_sym[lastEqu].comment, c);
					free(c);
				}
				else
					lastEqu = -1;
				free(ln2);
				continue;
			}
			if (parse_include(ln2, inc, sizeof(inc)))
			{
				load_include(inc, dbDir, true, true);
				lastEqu = -1;
			}
			else if (parse_equ(ln2, nm, sizeof(nm), &expr))
			{
				int k = sym_find(nm);
				if (k >= 0)
					die_at(i + 1, "'%s' is defined twice", nm);
				k = sym_add(nm, SK_EQU, path, i + 1);
				g_sym[k].expr = expr;
				g_sym[k].alias = expr_is_name(expr);
				if (cm)
				{
					char *c = xstrdup(cm);
					rstrip(c);
					g_sym[k].comment = c;
				}
				lastEqu = k;
			}
			else
				die_at(i + 1, "can't read this line: '%s'", ln);
			free(ln2);
		}
	}

	/* ranges: a record's type and count apply until the next record */
	{
		int t = RT_NONE, per = 0;
		for (a = 0; a < NADDR; a++)
		{
			if (g_rbeg[a])
			{
				t = g_rt[a];
				per = g_rper[a];
			}
			else if (g_pop[a] || t != RT_NONE)
			{
				g_rt[a] = (byte)t;
				g_rper[a] = (unsigned short)per;
			}
		}
	}

	/* resolve every .equ now, so errors point at the database */
	for (i = 0; i < g_nsym; i++)
	{
		long long v;
		char err[200];
		if (g_sym[i].kind != SK_EQU)
			continue;
		if (!sym_value(i, &v))
		{
			long long dummy;
			expr_eval(g_sym[i].expr, lookup_cb, NULL, &dummy, err, sizeof(err), NULL, 0);
			die("%s:%d: '%s': %s", path, g_sym[i].line, g_sym[i].name, err);
		}
		if (g_sym[i].global)
		{
			if (v < 0 || v >= NADDR)
				die("%s: ';@global %s': value 0x%llX isn't an address", path, g_sym[i].name, v);
			if (g_globalAt[v])
				die("%s: '%s' and '%s' are both global names for 0x%04llX", path,
					g_sym[g_globalAt[v] - 1].name, g_sym[i].name, v);
			g_globalAt[v] = i + 1;
		}
	}
	/* an .equ with no place in the listing goes before the first line */
	for (i = 0; i < g_nsym; i++)
	{
		int j;
		bool placed = false;
		if (g_sym[i].kind != SK_EQU)
			continue;
		for (j = 0; j < g_nitem; j++)
			if (g_item[j].kind != IT_TEXT && strcmp(g_item[j].s, g_sym[i].name) == 0)
				placed = true;
		if (!placed)
		{
			for (a = 0; a < NADDR && !g_pop[a]; a++)
				;
			item_add(a < NADDR ? a : END_SLOT, IT_EQU, g_sym[i].name);
		}
	}

	free(lines);
	free(text);
	free(dbDir);
	g_errFile = NULL;
}

static void check_db_roms(void)
{
	int i;
	for (i = 0; i < g_ndbRom; i++)
	{
		RomRec *r = &g_dbRom[i];
		unsigned long a;
		bool full = r->base + r->size <= NADDR;
		unsigned long crc;
		for (a = r->base; full && a < r->base + r->size; a++)
			if (!g_pop[a])
				full = false;
		if (!full)
		{
			if (g_ignoreCrc)
			{
				warn("the database describes a ROM at 0x%04lX-0x%04lX that isn't fully loaded", r->base,
					r->base + r->size - 1);
				continue;
			}
			die("the database is for a ROM of %lu bytes at 0x%04lX-0x%04lX, but that isn't fully loaded.\n"
				"  Load it there (use -b ADDR before the file name if it doesn't start at 0),\n"
				"  or add --ignore-crc to use the database anyway.", r->size, r->base, r->base + r->size - 1);
		}
		crc = crc32_buf(&g_mem[r->base], r->size);
		if (crc != r->crc)
		{
			if (g_ignoreCrc)
				warn("ROM at 0x%04lX has CRC-32 0x%08lX, the database expects 0x%08lX", r->base, crc, r->crc);
			else
				die("the database expects the ROM at 0x%04lX-0x%04lX to have CRC-32 0x%08lX,\n"
					"  but the loaded bytes have CRC-32 0x%08lX. Wrong file, or a bad dump?\n"
					"  To use the database anyway, add --ignore-crc.",
					r->base, r->base + r->size - 1, r->crc, crc);
		}
	}
	for (i = 0; i < g_nimg; i++)
	{
		int j;
		bool described = false;
		for (j = 0; j < g_ndbRom; j++)
			if (g_dbRom[j].base <= g_img[i].base && g_img[i].base < g_dbRom[j].base + g_dbRom[j].size)
				described = true;
		if (!described)
			warn("'%s' (0x%04lX) isn't described by the database; it gets no annotations",
				g_img[i].path, g_img[i].base);
	}
}

/* Bytes the database has no range for are printed as plain data. */
static void fill_layout_gaps(void)
{
	unsigned long a;
	for (a = 0; a < NADDR; a++)
	{
		if (!g_pop[a] || g_rt[a] != RT_NONE)
			continue;
		g_rt[a] = RT_BYTE;
		if (a == 0 || !g_pop[a - 1] || g_rt[a - 1] != RT_BYTE)
			g_rbeg[a] = true;
	}
}

/* =====================================================================
 * Database: write
 * ===================================================================== */

/* forCode: the --symbols file. A name for an address in neither the ROM,
 * RAM nor SFR space (something the ROM expects another program to put
 * there, like a firmware callback) is written commented out, since the
 * code that includes the file defines it itself. */
static void write_symbols_part(FILE *fp, bool forCode)
{
	int i;
	unsigned long a;
	Str s = { 0 };
	bool noted = false;
	for (i = 0; i < g_nincl; i++)
	{
		int j;
		bool macOnly = false;
		/* a symbols file is names for your own code: a file of macros
		 * alone is the listing's business, not its */
		for (j = 0; forCode && j < g_nmacOnly; j++)
			if (strcmp(g_macOnly[j], g_incl[i]) == 0)
				macOnly = true;
		if (!macOnly)
			fprintf(fp, INDENT ".include \"%s\"\n", g_incl[i]);
	}
	fprintf(fp, "\n; ---- Symbols ----\n");
	for (a = 0; a <= NADDR; a++)
	{
		int it;
		for (it = g_itemHead[a]; it; it = g_item[it - 1].next)
		{
			Item *x = &g_item[it - 1];
			int k;
			const char *c;
			if (x->kind == IT_TEXT)
				continue;
			k = sym_find(x->s);
			str_clear(&s);
			if (forCode && g_sym[k].kind == SK_EQU && !g_sym[k].alias && g_sym[k].value >= 0 &&
				g_sym[k].value < 0xFE00 && !g_pop[g_sym[k].value])
			{
				if (!noted)
					fprintf(fp, "; Commented out: addresses outside the ROM that it expects your code to\n"
						"; provide. Define them in your code, at these addresses.\n");
				noted = true;
				str_add(&s, ";");
			}
			if (g_sym[k].kind == SK_LABEL)
				str_addf(&s, INDENT ".equ    %s, 0x%04llX", g_sym[k].name, g_sym[k].value);
			else
				str_addf(&s, INDENT ".equ    %s, %s", g_sym[k].name, g_sym[k].expr);
			c = g_sym[k].comment;
			if (!c)
			{
				fprintf(fp, "%s\n", str_c(&s));
				continue;
			}
			for (;;)
			{
				const char *e = strchr(c, '\n');
				size_t n = e ? (size_t)(e - c) : strlen(c);
				str_pad(&s, COL_CMT);
				str_add(&s, n ? "; " : ";");
				str_addn(&s, c, n);
				fprintf(fp, "%s\n", str_c(&s));
				str_clear(&s);
				if (!e)
					break;
				c = e + 1;
			}
		}
	}
	free(s.s);
}

static void write_db(const char *path, const char *listing)
{
	FILE *fp = open_out(path);
	unsigned long a;
	int i;
	int prevT = RT_NONE, prevPer = -1;
	bool prevPop = false;

	fprintf(fp, "; 78k_disasm database, extracted from %s.\n", listing);
	fprintf(fp, "; Edit the listing, not this file, then run --extract again. The\n");
	fprintf(fp, "; part before the database records is plain assembler source and can be\n");
	fprintf(fp, "; .include'd; the ;@ records are comments to the assembler.\n");
	write_symbols_part(fp, false);
	fprintf(fp, "\n; ---- Database ----\n");
	for (i = 0; i < g_nimg; i++)
		fprintf(fp, ";@rom 0x%04lX %lu 0x%08lX\n", g_img[i].base, g_img[i].size, g_img[i].crc);
	if (g_calltBase >= 0)
		fprintf(fp, ";@callt 0x%04lX\n", (unsigned long)g_calltBase);
	for (i = 0; i < g_nsym; i++)
		if (g_sym[i].kind == SK_EQU && g_sym[i].global)
			fprintf(fp, ";@global %s\n", g_sym[i].name);
	for (i = 0; i < 32; i++)
		if (g_slotName[i])
			fprintf(fp, ";@slot %s\n", g_sym[g_slotName[i] - 1].name);
	for (i = 0; i < g_nmac; i++)
		if (g_mac[i].on)
			fprintf(fp, ";@macro %s\n", g_mac[i].name);

	for (a = 0; a <= NADDR; a++)
	{
		int it;
		const char *at = a == END_SLOT ? "end" : NULL;
		char abuf[16];
		const char *c;
		if (!at)
		{
			snprintf(abuf, sizeof(abuf), "0x%04lX", a);
			at = abuf;
		}
		if (a < NADDR && g_pop[a])
		{
			int per = g_rper[a];
			if (g_rbeg[a] || !prevPop || g_rt[a] != prevT || per != prevPer)
			{
				if (g_rt[a] == RT_CODE)
					fprintf(fp, ";@ %s code\n", at);
				else if (per)
					fprintf(fp, ";@ %s data %s/%d\n", at, RT_NAME[g_rt[a]], per);
				else
					fprintf(fp, ";@ %s data %s\n", at, RT_NAME[g_rt[a]]);
			}
			prevT = g_rt[a];
			prevPer = per;
			if (g_brk[a])
				fprintf(fp, ";@ %s break\n", at);
			if (g_noMac[a])
				fprintf(fp, ";@ %s nomacro\n", at);
		}
		prevPop = a < NADDR && g_pop[a];
		for (it = g_itemHead[a]; it; it = g_item[it - 1].next)
		{
			Item *x = &g_item[it - 1];
			fprintf(fp, ";@ %s %s %s\n", at, x->kind == IT_TEXT ? "text" : x->kind == IT_LABEL ? "label" : "equ", x->s);
		}
		if (a < NADDR && (c = g_eol[a]) != NULL)
		{
			for (;;)
			{
				const char *e = strchr(c, '\n');
				size_t n = e ? (size_t)(e - c) : strlen(c);
				fprintf(fp, ";@ %s ;%s%.*s\n", at, n ? " " : "", (int)n, c);
				if (!e)
					break;
				c = e + 1;
			}
		}
		if (a < NADDR && g_bindHead[a])
		{
			int slot;
			for (slot = 1; slot <= 3; slot++)
			{
				const Bind *b = bind_find(a, slot);
				if (b)
					fprintf(fp, ";@ %s op %d %s\n", at, slot, b->expr);
			}
			for (slot = 1; slot <= MAC_MAXP; slot++)
			{
				const Bind *b = bind_find(a, ARG_SLOT + slot);
				if (b)
					fprintf(fp, ";@ %s arg %d %s\n", at, slot, b->expr);
			}
		}
	}
	close_out(fp, path);
}

static void write_symbols_file(const char *path, const char *listing)
{
	FILE *fp = open_out(path);
	fprintf(fp, "; Names from %s, for use in your own source:\n", listing);
	fprintf(fp, ";   .include \"%s\"\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	fprintf(fp, "; Generated by 78k_disasm --extract; edit the listing, not this file.\n");
	write_symbols_part(fp, true);
	close_out(fp, path);
}

/* =====================================================================
 * Extract: listing -> database
 * ===================================================================== */

typedef struct MLine
{
	int no;            /* 1-based line number in the listing */
	long addr;         /* address column, or -1 */
	char *bytes;       /* bytes column text */
	char *src;         /* source part */
	char *code;        /* source without the comment */
	char *cmt;         /* end-of-line comment, or NULL */
	int cmtCol;        /* source column of ';' */
	int kind;          /* LK_* as recognized */
	int mac;           /* a macro line: the macro's index + 1 */
} MLine;

static int g_stale = 0;   /* lines that read differently when regenerated */
static MLine *g_ml = NULL;
static int g_nml = 0, g_capml = 0;

static bool is_hex4_addr(const char *s)
{
	int i;
	for (i = 0; i < 4; i++)
		if (!isxdigit((unsigned char)s[i]))
			return false;
	return s[4] == ' ' && s[5] == ' ';
}

/* "        .byte   0x01, 0x02" -> directive name and operand text */
static const char *first_word(const char *code, char *w, size_t wsz)
{
	const char *p = skipws(code);
	size_t n = 0;
	while (*p && !isspace((unsigned char)*p) && n + 1 < wsz)
		w[n++] = *p++;
	w[n] = '\0';
	return skipws(p);
}

/* An operand field split at top-level commas (not inside brackets,
 * braces, parentheses or quotes), each part trimmed. */
typedef struct Ops
{
	int n;
	char **v;
} Ops;

static void ops_split(const char *s, Ops *o, int line)
{
	int depth = 0, cap = 0;
	char q = 0;
	const char *st = s, *p;
	o->n = 0;
	o->v = NULL;
	if (!*skipws(s))
		return;
	for (p = s;; p++)
	{
		if (q && *p)
		{
			if (*p == '\\' && p[1])
				p++;
			else if (*p == q)
				q = 0;
			continue;
		}
		if (q)
			die_at(line, "a quote is never closed");
		if (*p == '"' || (*p == '\'' && !(p > s && isalpha((unsigned char)p[-1]))))
			q = *p;
		else if (*p == '(' || *p == '[' || *p == '{')
			depth++;
		else if (*p == ')' || *p == ']' || *p == '}')
			depth--;
		if ((*p == ',' && depth == 0) || *p == '\0')
		{
			char *t = xstrndup(st, (size_t)(p - st));
			rstrip(t);
			memmove(t, skipws(t), strlen(skipws(t)) + 1);
			GROW(o->v, o->n, cap);
			o->v[o->n++] = t;
			if (!*p)
				break;
			st = p + 1;
		}
	}
}

static void ops_free(Ops *o)
{
	int i;
	for (i = 0; i < o->n; i++)
		free(o->v[i]);
	free(o->v);
	o->n = 0;
	o->v = NULL;
}

static char *nospace(const char *s)
{
	char *d = (char *)xrealloc(NULL, strlen(s) + 1);
	char *o = d;
	char q = 0;
	for (; *s; s++)
	{
		if (q)
		{
			*o++ = *s;
			if (*s == q)
				q = 0;
			continue;
		}
		if (*s == '"')
			q = *s;
		if (!isspace((unsigned char)*s))
			*o++ = *s;
	}
	*o = '\0';
	return d;
}

/* Matches pre ... post around an operand (blanks ignored); returns the
 * middle (trimmed, user's spacing kept) or NULL. */
static char *strip_pre_post(const char *t, const char *pre, const char *post)
{
	const char *p = t;
	const char *q;
	const char *e;
	size_t i;
	for (i = 0; pre[i]; i++)
	{
		p = skipws(p);
		if (tolower((unsigned char)*p) != tolower((unsigned char)pre[i]))
			return NULL;
		p++;
	}
	e = t + strlen(t);
	for (i = strlen(post); i > 0; i--)
	{
		while (e > p && isspace((unsigned char)e[-1]))
			e--;
		if (e <= p || tolower((unsigned char)e[-1]) != tolower((unsigned char)post[i - 1]))
			return NULL;
		e--;
	}
	q = skipws(p);
	while (e > q && isspace((unsigned char)e[-1]))
		e--;
	if (e <= q)
		return NULL;
	return xstrndup(q, (size_t)(e - q));
}

static bool eval_master(const MLine *m, const char *expr, long long *v, const char *what)
{
	char err[200], missing[64];
	if (expr_eval(expr, lookup_cb, NULL, v, err, sizeof(err), missing, sizeof(missing)))
		return true;
	if (missing[0])
		return false;
	die_at(m->no, "%s '%s': %s", what, expr, err);
	return false;
}

static bool value_fits(const DOp *d, long long v)
{
	switch (d->kind)
	{
	case K_IMM:
	case K_DISP:
	{
		long long mask = d->width == 8 ? 0xFF : 0xFFFF;
		long long lo = d->width == 8 ? -128 : -32768;
		if (v < lo || v > mask)
			return false;
		return (v & mask) == d->value;
	}
	case K_DATA:
		if (d->form == DF_SFR && v >= 0 && v < 0x100)
			return (v | 0xFF00) == d->value;
		return v == d->value;
	case K_CODE:
		return v == (d->value & 0xFFFF);
	default:
		return v == d->value;
	}
}

static const char *kind_word(const DOp *d)
{
	switch (d->kind)
	{
	case K_IMM: return "immediate";
	case K_DATA: case K_INDEX: return "address";
	case K_CODE: return "target";
	case K_BIT: case K_CYBIT: return "bit number";
	case K_COUNT: return "count";
	case K_DISP: return "displacement";
	case K_CALLT: return "CALLT slot";
	default: return "operand";
	}
}

static void hint_value(const MLine *m, const char *expr, const DOp *d)
{
	char missing[64], err[200];
	long long v;
	char valtxt[32];
	if (d->kind == K_IMM || d->kind == K_DISP)
		snprintf(valtxt, sizeof(valtxt), d->width == 8 ? "0x%02lX" : "0x%04lX", d->value);
	else if (d->kind == K_BIT || d->kind == K_CYBIT || d->kind == K_COUNT)
		snprintf(valtxt, sizeof(valtxt), "%ld", d->value);
	else
		snprintf(valtxt, sizeof(valtxt), "0x%04lX", d->value & 0xFFFF);
	if (!expr_eval(expr, lookup_cb, NULL, &v, err, sizeof(err), missing, sizeof(missing)))
	{
		if (missing[0] && strcmp(missing, expr) == 0)
			die_at(m->no, "'%s' is not defined. This %s is %s; to name it, add this line above:\n"
				INDENT ".equ    %s, %s", expr, kind_word(d), valtxt, expr, valtxt);
		die_at(m->no, "'%s': %s", expr, err);
	}
	{
		const char *q = skipws(expr);
		bool hasName = false;
		for (; *q; q++)
			if (is_ident_start((unsigned char)*q) && !(q > expr && isalnum((unsigned char)q[-1])))
				hasName = true;
		if (!hasName)
			die_at(m->no, "this %s is %s in the ROM, not %s. Instructions come from the ROM, so the\n"
				"  number can't change; put %s back.", kind_word(d), valtxt, expr, valtxt);
	}
	die_at(m->no, "'%s' is 0x%llX, but this %s is %s in the ROM. The instruction comes from the\n"
		"  ROM, so use a name whose value is %s (or fix the .equ).", expr, v, kind_word(d), valtxt, valtxt);
}

/* A site where the listing writes a number or name. */
typedef struct Site
{
	unsigned long addr;
	int slot;
	char *expr;          /* as written */
	long long value;
	bool addressCtx;     /* where global names apply */
	bool callt;          /* a CALLT slot operand */
	const MLine *m;
} Site;

static Site *g_site = NULL;
static int g_nsite = 0, g_capsite = 0;

static void site_add(unsigned long a, int slot, const char *expr, long long v, bool actx, const MLine *m)
{
	Site *s;
	GROW(g_site, g_nsite, g_capsite);
	s = &g_site[g_nsite++];
	s->addr = a;
	s->slot = slot;
	s->expr = xstrdup(expr);
	s->value = v;
	s->addressCtx = actx;
	s->callt = false;
	s->m = m;
}

/* Value of a name that stands for an address: a label, a generated
 * name, or a name in the generated form for an address that has a real
 * label now. -1 for anything else (constants, registers, ...). */
static long addr_name_value(const char *nm)
{
	int k = sym_find(nm);
	unsigned long a;
	if (k >= 0 && (g_sym[k].kind == SK_LABEL || g_sym[k].kind == SK_AUTO))
		return (long)g_sym[k].value;
	if (k < 0 && auto_name_addr(nm, &a))
		return (long)a;
	if (k < 0 && strncmp(nm, "image_end_", 10) == 0 && strlen(nm) == 14)
		return (long)strtoul(nm + 10, NULL, 16);
	return -1;
}

/* Copies code text, replacing names of addresses by "@XXXX" (so a
 * renamed label compares equal to its old generated name), or, with
 * rename set, replacing generated-form names by the address's current
 * name. Text in quotes is copied unchanged. */
static char *map_names(const char *code, bool rename)
{
	Str o = { 0 };
	const char *p = code;
	while (*p)
	{
		if (*p == '"')
		{
			const char *q = p + 1;
			while (*q && *q != '"')
				q += (*q == '\\' && q[1]) ? 2 : 1;
			if (*q)
				q++;
			str_addn(&o, p, (size_t)(q - p));
			p = q;
			continue;
		}
		if (isdigit((unsigned char)*p))
		{
			const char *q = p;
			while (is_ident_char((unsigned char)*q))
				q++;
			str_addn(&o, p, (size_t)(q - p));
			p = q;
			continue;
		}
		if (is_ident_start((unsigned char)*p) && !(p > code && p[-1] == '\''))
		{
			const char *q = p;
			char nm[64];
			long v;
			while (is_ident_char((unsigned char)*q))
				q++;
			if ((size_t)(q - p) < sizeof(nm))
			{
				memcpy(nm, p, (size_t)(q - p));
				nm[q - p] = '\0';
				{
					int k2 = sym_find(nm);
					int slot = (k2 >= 0 && g_sym[k2].kind == SK_CALLT) ? (int)g_sym[k2].value :
						(k2 < 0 ? callt_slot_of_name(nm) : -1);
					if (slot >= 0 && !rename)
					{
						str_addf(&o, "%%C%02X", slot);
						p = q;
						continue;
					}
					if (slot >= 0 && rename && k2 < 0 && g_calltAuto[slot])
					{
						str_add(&o, g_sym[g_calltAuto[slot] - 1].name);
						p = q;
						continue;
					}
				}
				v = addr_name_value(nm);
				if (v >= 0 && !rename)
				{
					str_addf(&o, "@%04lX", (unsigned long)v);
					p = q;
					continue;
				}
				if (v >= 0 && rename && sym_find(nm) < 0)
				{
					int k = name_for(v, CTX_WORD);
					if (k >= 0)
					{
						str_add(&o, g_sym[k].name);
						p = q;
						continue;
					}
				}
			}
			str_addn(&o, p, (size_t)(q - p));
			p = q;
			continue;
		}
		str_addn(&o, p, 1);
		p++;
	}
	return o.s ? o.s : xstrdup("");
}

/* Canonical form for comparing a listing line with its regeneration:
 * whitespace in the code part is ignored (alignment, "a,b" vs "a, b"),
 * and so is which name an address goes by; comments must match
 * exactly. */
static char *canon(const char *line)
{
	char *l = xstrdup(line);
	char *src;
	char *cm;
	char *code, *mapped;
	char *r;
	char head[32] = "";
	if (is_hex4_addr(l))
	{
		/* address and bytes column */
		size_t n = strlen(l) < MARGIN ? strlen(l) : MARGIN;
		char *h = xstrndup(l, n);
		char *hs = nospace(h);
		snprintf(head, sizeof(head), "%s|", hs);
		free(h);
		free(hs);
		src = l + n;
	}
	else
	{
		size_t k = 0;
		while (l[k] == ' ' && k < MARGIN)
			k++;
		src = l + k;
	}
	cm = split_comment(src);
	code = map_names(src, false);
	mapped = nospace(code);
	if (cm)
	{
		char *c = xstrdup(cm);
		rstrip(c);
		r = xasprintf("%s%s;%s", head, mapped, c);
		free(c);
	}
	else
		r = xasprintf("%s%s", head, mapped);
	free(code);
	free(mapped);
	free(l);
	return r;
}

/* Parses the escapes of a .ascii string. */
static int parse_ascii(const char *s, byte *out, int max, bool *ok)
{
	const char *p = skipws(s);
	int n = 0;
	*ok = false;
	if (*p != '"')
		return 0;
	p++;
	while (*p && *p != '"')
	{
		int c;
		if (*p == '\\')
		{
			p++;
			if (*p == 'x' || *p == 'X')
			{
				char h[3] = { 0 };
				if (!isxdigit((unsigned char)p[1]) || !isxdigit((unsigned char)p[2]))
					return n;
				h[0] = p[1];
				h[1] = p[2];
				c = (int)strtol(h, NULL, 16);
				p += 3;
			}
			else if (*p == '\\' || *p == '"' || *p == '\'')
				c = *p++;
			else
				return n;
		}
		else
			c = (unsigned char)*p++;
		if (n < max)
			out[n] = (byte)c;
		n++;
	}
	if (*p != '"' || *skipws(p + 1))
		return n;
	*ok = true;
	return n;
}

/* Stores one rebuilt ROM byte, refusing addresses past FFFFH. */
static void put_byte(const MLine *m, unsigned long *a, byte b)
{
	if (*a >= NADDR)
		die_at(m->no, "this line's bytes run past FFFFH, the end of the address space");
	g_mem[*a] = b;
	g_pop[*a] = true;
	(*a)++;
}

static void extract_listing(const char *path, const char *dbOut, const char *symOut)
{
	char *text = read_file(path, NULL);
	char **lines;
	int nlines, i, hdrEnd = -1;
	char *dir;
	unsigned long a;

	if (!text)
		die("can't read '%s': %s", path, strerror(errno));
	g_errFile = path;
	g_fresh = false;
	dir = dir_of(path);
	nlines = split_lines(text, &lines);

	/* ---- header ---- */
	for (i = 0; i < nlines; i++)
		if (strcmp(lines[i] + (strncmp(lines[i], "                      ", MARGIN) == 0 ? MARGIN : 0), HDR_END) == 0)
		{
			hdrEnd = i;
			break;
		}
	if (hdrEnd < 0)
		die_at(0, "this doesn't look like a 78k_disasm listing: the line\n  %s\nwasn't found. "
			"Extract from a listing made by 78k_disasm (with its address/byte columns).", HDR_END);
	for (i = 0; i < hdrEnd; i++)
	{
		const char *p = lines[i];
		char inc[256];
		unsigned long b, e, n, c;
		if (strncmp(p, "                      ", MARGIN) == 0)
			p += MARGIN;
		if (sscanf(p, "; CALLT table at 0x%lx", &b) == 1)
		{
			if (b + 0x3F >= NADDR)
				die_at(i + 1, "the CALLT table address 0x%lX leaves no room for its 32 entries", b);
			g_calltBase = (long)b;
		}
		else if (sscanf(p, "; ROM 0x%lx-0x%lx, %lu bytes, CRC-32 0x%lx", &b, &e, &n, &c) == 4)
		{
			RomRec r = { b, n, c };
			if (n == 0 || b >= NADDR || n > NADDR - b || e != b + n - 1)
				die_at(i + 1, "this ROM line doesn't describe a ROM inside 0000H-FFFFH:\n  %s", lines[i]);
			GROW(g_dbRom, g_ndbRom, g_capdbRom);
			g_dbRom[g_ndbRom++] = r;
		}
		else if (parse_include(p, inc, sizeof(inc)))
			load_include(inc, dir, true, true);
		else
		{
			char nm[64];
			char *expr = NULL;
			if (!(*skipws(p) == '\0' || strncmp(p, "; Generated by 78k_disasm", 25) == 0 ||
				strncmp(p, ";   78k_disasm --extract", 24) == 0 || parse_equ(p, nm, sizeof(nm), &expr)))
				die_at(i + 1, "the generated header is rebuilt every time, so this line would be lost:\n  %s\n"
					"Move it below the line '%s'.", lines[i], HDR_END);
			free(expr);
		}
	}
	g_errFile = path;
	if (g_ndbRom == 0)
		die_at(0, "the header has no '; ROM ...' line, so the ROM it was made from is unknown");

	/* ---- classify body lines ---- */
	for (i = hdrEnd + 1; i < nlines; i++)
	{
		MLine m;
		char *l = lines[i];
		char w[32];
		memset(&m, 0, sizeof(m));
		m.no = i + 1;
		m.addr = -1;
		if (is_hex4_addr(l))
		{
			size_t len = strlen(l);
			m.addr = (long)strtoul(l, NULL, 16);
			m.bytes = xstrndup(len > 6 ? l + 6 : "", len > 6 ? (len < 20 ? len - 6 : 14) : 0);
			rstrip(m.bytes);
			m.src = xstrdup(len > MARGIN ? l + MARGIN : "");
		}
		else
		{
			size_t k = 0;
			while (l[k] == ' ' && k < MARGIN)
				k++;
			if (l[k] == '\t')
				die_at(m.no, "tabs aren't supported in a listing; use spaces");
			m.bytes = xstrdup("");
			m.src = xstrdup(l + k);
		}
		rstrip(m.src);
		{
			char *c = xstrdup(m.src);
			char *cm = split_comment(c);
			m.cmtCol = cm ? (int)(cm - c) - (cm[-1] == ' ' && cm - 2 >= c && cm[-2] == '\0' ? 2 : 1) : -1;
			if (cm)
			{
				/* column of the ';' itself */
				const char *semi = m.src;
				char qq = 0;
				for (; *semi; semi++)
				{
					if (qq) { if (*semi == '\\' && semi[1]) semi++; else if (*semi == qq) qq = 0; continue; }
					if (*semi == '"' || (*semi == '\'' && !(semi > m.src && isalpha((unsigned char)semi[-1])))) qq = *semi;
					else if (*semi == ';') break;
				}
				m.cmtCol = (int)(semi - m.src);
				m.cmt = xstrdup(cm);
				rstrip(m.cmt);
			}
			rstrip(c);
			m.code = c;
		}
		first_word(m.code, w, sizeof(w));
		if (m.addr >= 0 && !*skipws(m.code))
		{
			/* a macro's further bytes: address and bytes only */
			if (m.cmt)
				die_at(m.no, "a macro's byte line can't carry a comment; put it on the macro line");
			if (!*m.bytes)
				die_at(m.no, "this line has an address but no bytes and no source");
			if (g_nml == 0 || !(g_ml[g_nml - 1].mac || g_ml[g_nml - 1].kind == LK_MBYTES))
			{
				int b = g_nml - 1;
				while (b >= 0 && g_ml[b].addr < 0)
					b--;
				if (b >= 0 && (g_ml[b].mac || g_ml[b].kind == LK_MBYTES))
					die_at(m.no, "nothing can go between a macro line and its byte lines; to give an\n"
						"  instruction inside the macro a label or comment, write the instructions out");
				die_at(m.no, "a line with only an address and bytes must follow a macro line");
			}
			m.kind = LK_MBYTES;
		}
		else if (m.addr >= 0)
		{
			m.kind = (w[0] == '.') ? LK_DATA : LK_CODE;
			if (m.kind == LK_CODE && mac_find(w) >= 0)
				m.mac = mac_find(w) + 1;
		}
		else if (!*skipws(m.code))
		{
			m.kind = LK_TEXT;
			if (m.cmt && g_nml > 0 && m.cmtCol >= COL_CMT - 4)
			{
				MLine *pv = &g_ml[g_nml - 1];
				if (pv->cmt && (pv->kind == LK_CODE || pv->kind == LK_DATA || pv->kind == LK_LABEL ||
					pv->kind == LK_EQU || pv->kind == LK_CONT))
					m.kind = LK_CONT;
			}
		}
		else if (!strcmp(w, ".org"))
			m.kind = LK_ORG;
		else if (!strcmp(w, ".equ"))
			m.kind = LK_EQU;
		else
		{
			const char *p = skipws(m.code);
			const char *q = p;
			while (is_ident_char((unsigned char)*q))
				q++;
			if (q > p && is_ident_start((unsigned char)*p) && *q == ':' && !*skipws(q + 1))
				m.kind = LK_LABEL;
			else
				die_at(m.no, "can't tell what this line is:\n  %s\n"
					"Comments start with ';', labels end with ':', and constants are written\n"
					"  " INDENT ".equ    NAME, VALUE", lines[i]);
		}
		GROW(g_ml, g_nml, g_capml);
		g_ml[g_nml++] = m;
	}

	/* ---- names: labels (positional) and .equ lines ---- */
	{
		int k;
		for (i = 0; i < g_nml; i++)
		{
			MLine *m = &g_ml[i];
			if (m->kind == LK_LABEL)
			{
				char nm[64];
				const char *p = skipws(m->code);
				size_t n = (size_t)(strchr(p, ':') - p);
				unsigned long at = 0, aa;
				int j;
				bool found = false;
				if (n >= sizeof(nm))
					die_at(m->no, "name too long");
				memcpy(nm, p, n);
				nm[n] = '\0';
				for (j = i + 1; j < g_nml; j++)
					if (g_ml[j].addr >= 0)
					{
						at = (unsigned long)g_ml[j].addr;
						found = true;
						break;
					}
				if (!found)
					die_at(m->no, "label '%s' isn't followed by any code or data", nm);
				if (auto_name_addr(nm, &aa) && aa == at)
				{
					m->kind = LK_ALABEL;
					continue;
				}
				k = sym_find(nm);
				if (k >= 0)
					die_at(m->no, "'%s' is already defined at line %d. Each name can only be defined once;\n"
						"  rename one of them.", nm, g_sym[k].line);
				k = sym_add(nm, SK_LABEL, path, m->no);
				if (g_labelAt[at])
				{
					Sym *o = &g_sym[g_labelAt[at] - 1];
					die_at(m->no, "'%s' and '%s' (line %d) both label 0x%04lX. To keep both names, make this one\n"
						"  an alias of the other by replacing this line with:\n"
						INDENT ".equ    %s, %s", nm, o->name, o->line, at, nm, o->name);
				}
				set_label(k, at);
			}
			else if (m->kind == LK_EQU)
			{
				char nm[64];
				char *expr;
				if (!parse_equ(m->code, nm, sizeof(nm), &expr))
					die_at(m->no, "expected " INDENT ".equ    NAME, VALUE");
				k = sym_find(nm);
				if (k >= 0)
					die_at(m->no, "'%s' is already defined at line %d; rename one of them", nm, g_sym[k].line);
				k = sym_add(nm, SK_EQU, path, m->no);
				g_sym[k].expr = expr;
				g_sym[k].alias = expr_is_name(expr);
			}
		}
		/* all .equ values must resolve */
		for (k = 0; k < g_nsym; k++)
		{
			long long v;
			char err[200], missing[64];
			if (g_sym[k].kind != SK_EQU)
				continue;
			if (!sym_value(k, &v))
			{
				long long d;
				expr_eval(g_sym[k].expr, lookup_cb, NULL, &d, err, sizeof(err), missing, sizeof(missing));
				die_at(g_sym[k].line, "'%s': %s", g_sym[k].name, err);
			}
		}
	}

	/* ---- rebuild the ROM from the listing ---- */
	memset(g_pop, 0, sizeof(g_pop));
	for (i = 0; i < g_nml; i++)
	{
		MLine *m = &g_ml[i];
		char w[32];
		const char *ops;
		if (m->addr < 0)
			continue;
		ops = first_word(m->code, w, sizeof(w));
		a = (unsigned long)m->addr;
		if (m->kind == LK_CODE || m->kind == LK_MBYTES)
		{
			/* bytes column */
			const char *p = m->bytes;
			int n = 0;
			while (*(p = skipws(p)))
			{
				char *e;
				long b = strtol(p, &e, 16);
				if (e - p != 2 || b < 0 || b > 255 || n >= 5)
					die_at(m->no, "the byte column '%s' was changed", m->bytes);
				{
					unsigned long x = a + (unsigned long)n;
					put_byte(m, &x, (byte)b);
				}
				n++;
				p = e;
			}
			if (n == 0)
				die_at(m->no, "this instruction has no bytes in its byte column");
		}
		else if (!strcmp(w, ".byte") || !strcmp(w, ".word"))
		{
			Ops o;
			int j;
			int unit = w[1] == 'b' ? 1 : 2;
			ops_split(ops, &o, m->no);
			if (o.n <= 0)
				die_at(m->no, "%s needs values", w);
			for (j = 0; j < o.n; j++)
			{
				long long v;
				if (!eval_master(m, o.v[j], &v, w))
					die_at(m->no, "'%s' is not defined", o.v[j]);
				if (unit == 1 ? (v < -128 || v > 255) : (v < -32768 || v > 65535))
					die_at(m->no, "%s value '%s' doesn't fit", w, o.v[j]);
				put_byte(m, &a, (byte)(v & 0xFF));
				if (unit == 2)
					put_byte(m, &a, (byte)((v >> 8) & 0xFF));
			}
			ops_free(&o);
		}
		else if (!strcmp(w, ".ascii"))
		{
			byte buf[4096];
			bool ok;
			int n = parse_ascii(ops, buf, (int)sizeof(buf), &ok), j;
			if (!ok || n == 0 || n > (int)sizeof(buf))
				die_at(m->no, "can't read this string. Write it in double quotes, with \\\" for a quote,\n"
					"  \\\\ for a backslash and \\xNN for any other byte.");
			for (j = 0; j < n; j++)
				put_byte(m, &a, buf[j]);
		}
		else if (!strcmp(w, ".space"))
		{
			Ops o;
			int j;
			long long v;
			unsigned long end = 0;
			bool found = false;
			ops_split(ops, &o, m->no);
			if (o.n != 2 || !eval_master(m, o.v[1], &v, ".space fill value"))
				die_at(m->no, "expected .space TARGET-$, VALUE");
			ops_free(&o);
			for (j = i + 1; j < g_nml; j++)
				if (g_ml[j].addr >= 0)
				{
					end = (unsigned long)g_ml[j].addr;
					found = true;
					break;
				}
			if (!found)
			{
				int r;
				for (r = 0; r < g_ndbRom; r++)
					if (a >= g_dbRom[r].base && a < g_dbRom[r].base + g_dbRom[r].size)
					{
						end = g_dbRom[r].base + g_dbRom[r].size;
						found = true;
					}
			}
			if (!found || end <= a || end - a > NADDR)
				die_at(m->no, "can't tell where this fill ends");
			/* a fill ends at the next line, unless the ROM ends first */
			{
				int r;
				for (r = 0; r < g_ndbRom; r++)
					if (a >= g_dbRom[r].base && a < g_dbRom[r].base + g_dbRom[r].size &&
						end > g_dbRom[r].base + g_dbRom[r].size)
						end = g_dbRom[r].base + g_dbRom[r].size;
			}
			while (a < end)
				put_byte(m, &a, (byte)(v & 0xFF));
		}
		else
			die_at(m->no, "unknown directive '%s' on a line with an address", w);
	}
	{
		int r;
		for (r = 0; r < g_ndbRom; r++)
		{
			RomRec *rr = &g_dbRom[r];
			unsigned long crc;
			for (a = rr->base; a < rr->base + rr->size; a++)
				if (!g_pop[a])
					die_at(0, "the listing has no bytes for 0x%04lX, but its ROM covers 0x%04lX-0x%04lX.\n"
						"  Lines with addresses can't be removed.", a, rr->base, rr->base + rr->size - 1);
			crc = crc32_buf(&g_mem[rr->base], rr->size);
			if (crc != rr->crc)
				die_at(0, "the bytes in this listing don't match its ROM (CRC-32 0x%08lX, the header says\n"
					"  0x%08lX). An address or byte column, or a data line, was changed. Compare with a\n"
					"  freshly generated listing to find it.", crc, rr->crc);
			add_image(path, rr->base, rr->size);
		}
		for (a = 0; a < NADDR; a++)
		{
			bool in = false;
			for (r = 0; r < g_ndbRom; r++)
				if (a >= g_dbRom[r].base && a < g_dbRom[r].base + g_dbRom[r].size)
					in = true;
			if (g_pop[a] && !in)
				die_at(0, "the listing has bytes at 0x%04lX, outside the ROM its header describes", a);
		}
	}

	/* ---- layout ---- */
	{
		int prevType = RT_NONE;
		long prevEnd = -1;
		for (i = 0; i < g_nml; i++)
		{
			MLine *m = &g_ml[i];
			char w[32];
			int type;
			unsigned long len;
			if (m->addr < 0)
				continue;
			a = (unsigned long)m->addr;
			first_word(m->code, w, sizeof(w));
			if (m->kind == LK_CODE || m->kind == LK_MBYTES)
				type = RT_CODE;
			else if (!strcmp(w, ".byte"))
			{
				/* a .byte line standing in for an instruction? */
				Insn in;
				int nb = 0;
				Ops o;
				ops_split(first_word(m->code, w, sizeof(w)), &o, m->no);
				nb = o.n;
				ops_free(&o);
				type = RT_BYTE;
				/* recognized by its generated comment, which starts with
				 * the mnemonic (the operands may show names) */
				if (decode_insn(a, &in) && in.len == nb && !insn_asm_ok(&in) && m->cmt)
				{
					View v;
					const char *mn;
					size_t ml;
					insn_view(&in, &v);
					mn = v.mn;
					ml = strlen(mn);
					if (strncasecmp(m->cmt, mn, ml) == 0 && (m->cmt[ml] == ' ' || m->cmt[ml] == '\0'))
					{
						type = RT_CODE;
						m->kind = LK_CODE;
					}
				}
			}
			else if (!strcmp(w, ".word"))
				type = RT_WORD;
			else if (!strcmp(w, ".ascii"))
				type = RT_STRING;
			else
				type = RT_BYTE;   /* .space */
			/* length of this line */
			{
				int j;
				unsigned long next = NADDR;
				for (j = i + 1; j < g_nml; j++)
					if (g_ml[j].addr >= 0)
					{
						next = (unsigned long)g_ml[j].addr;
						break;
					}
				len = 0;
				while (a + len < NADDR && g_pop[a + len] && a + len < next)
					len++;
			}
			if (type != prevType || prevEnd != (long)a)
				g_rbeg[a] = true;
			{
				unsigned long k;
				for (k = 0; k < len; k++)
					g_rt[a + k] = (byte)type;
			}
			prevType = type;
			prevEnd = (long)(a + len);
		}
		/* items per line: the longest line in each data range */
		for (i = 0; i < g_nml; i++)
		{
			MLine *m = &g_ml[i];
			char w[32];
			const char *ops;
			unsigned long st;
			int items = 0;
			if (m->addr < 0 || m->kind != LK_DATA)
				continue;
			ops = first_word(m->code, w, sizeof(w));
			if (!strcmp(w, ".space"))
				continue;
			if (!strcmp(w, ".ascii"))
			{
				byte buf[4096];
				bool ok;
				items = parse_ascii(ops, buf, (int)sizeof(buf), &ok);
			}
			else
			{
				Ops o;
				ops_split(ops, &o, m->no);
				items = o.n;
				ops_free(&o);
			}
			st = (unsigned long)m->addr;
			while (st > 0 && !g_rbeg[st] && g_pop[st - 1])
				st--;
			if (items > g_rper[st])
				g_rper[st] = (unsigned short)items;
		}
		/* spread the per-line count and mark pointer tables later */
		{
			unsigned short per = 0;
			for (a = 0; a < NADDR; a++)
			{
				if (!g_pop[a])
					continue;
				if (g_rbeg[a])
					per = g_rper[a];
				g_rper[a] = (g_rt[a] == RT_CODE) ? 0 : per;
			}
			/* a count equal to the default needn't be stored */
			for (a = 0; a < NADDR; a++)
				if (g_pop[a] && g_rt[a] != RT_CODE && g_rper[a] == default_per(g_rt[a]))
					g_rper[a] = 0;
		}
	}
	decode_layout();
	for (i = 0; i < g_nml; i++)
	{
		MLine *m = &g_ml[i];
		if ((m->kind == LK_CODE || m->kind == LK_MBYTES) && m->addr >= 0 && g_lc[m->addr] != LC_CODE)
			die_at(m->no, "this instruction doesn't decode from its bytes (or overlaps another line)");
	}

	/* ---- items, comments ---- */
	{
		/* pending items attach to the next line with an address */
		int pend0 = 0;
		for (i = 0; i <= g_nml; i++)
		{
			MLine *m = i < g_nml ? &g_ml[i] : NULL;
			unsigned long at;
			int j;
			if (m && m->addr < 0)
				continue;
			at = m ? (unsigned long)m->addr : END_SLOT;
			for (j = pend0; j < i; j++)
			{
				MLine *x = &g_ml[j];
				if (m && m->kind == LK_MBYTES && (x->kind == LK_TEXT || x->kind == LK_LABEL || x->kind == LK_EQU))
					die_at(x->no, "nothing can go between a macro line and its byte lines; to give an\n"
						"  instruction inside the macro a label or comment, write the instructions out");
				switch (x->kind)
				{
				case LK_TEXT:
					item_add(at, IT_TEXT, x->src);
					break;
				case LK_LABEL:
				{
					const char *p = skipws(x->code);
					char nm[64];
					size_t n = (size_t)(strchr(p, ':') - p);
					memcpy(nm, p, n);
					nm[n] = '\0';
					item_add(at, IT_LABEL, nm);
					if (x->cmt)
						comment_append(&g_sym[sym_find(nm)].comment, x->cmt);
					break;
				}
				case LK_EQU:
				{
					char nm[64];
					char *expr;
					parse_equ(x->code, nm, sizeof(nm), &expr);
					free(expr);
					item_add(at, IT_EQU, nm);
					if (x->cmt)
						comment_append(&g_sym[sym_find(nm)].comment, x->cmt);
					break;
				}
				case LK_CONT:
				{
					/* continues the comment of the line before it */
					int b = j - 1;
					while (b >= 0 && g_ml[b].kind == LK_CONT)
						b--;
					if (b >= 0 && (g_ml[b].kind == LK_LABEL || g_ml[b].kind == LK_EQU))
					{
						char nm[64];
						const char *p = skipws(g_ml[b].code);
						if (g_ml[b].kind == LK_LABEL)
						{
							size_t n = (size_t)(strchr(p, ':') - p);
							memcpy(nm, p, n);
							nm[n] = '\0';
						}
						else
						{
							char *expr;
							parse_equ(g_ml[b].code, nm, sizeof(nm), &expr);
							free(expr);
						}
						comment_append(&g_sym[sym_find(nm)].comment, x->cmt ? x->cmt : "");
					}
					else if (b >= 0 && g_ml[b].addr >= 0)
						eol_add((unsigned long)g_ml[b].addr, x->cmt ? x->cmt : "");
					else
						item_add(at, IT_TEXT, x->src);
					break;
				}
				default:
					break;   /* .org, generated labels */
				}
			}
			if (m)
			{
				if (m->cmt)
				{
					/* the first comment of a stand-in .byte line is generated */
					bool generated = false;
					if (m->kind == LK_CODE)
					{
						char w[32];
						first_word(m->code, w, sizeof(w));
						generated = !strcmp(w, ".byte");
					}
					if (!generated)
						eol_add(at, m->cmt);
				}
				/* continuation lines that follow belong to this line; they
				 * are handled as pending items when the next address comes */
			}
			pend0 = i + 1;
		}
	}

	/* ---- operands ---- */
	for (i = 0; i < g_nml; i++)
	{
		MLine *m = &g_ml[i];
		char w[32];
		const char *ops;
		if (m->addr < 0)
			continue;
		a = (unsigned long)m->addr;
		ops = first_word(m->code, w, sizeof(w));
		if (m->kind == LK_CODE && !m->mac && strcmp(w, ".byte") != 0)
		{
			Insn in;
			View v;
			Ops so;
			char **slot;
			int n, j;
			decode_insn(a, &in);
			insn_view(&in, &v);
			if (strcasecmp(w, v.mn) != 0)
			{
				char *plain = insn_plain(&in);
				die_at(m->no, "the instruction was changed: the ROM has '%s' here. Instructions come\n"
					"  from the ROM; only names in operands and comments can be edited.", plain);
			}
			ops_split(ops, &so, m->no);
			n = so.n;
			slot = so.v;
			if (n != v.n)
			{
				char *plain = insn_plain(&in);
				die_at(m->no, "this instruction should have %d operand(s): the ROM has '%s' here", v.n, plain);
			}
			for (j = 0; j < n; j++)
			{
				const DOp *d = &in.ops[v.slot[j]];
				char *text = slot[j];
				if (v.cy[j])
				{
					char *x = nospace(text);
					if (strcasecmp(x, "cy") != 0)
					{
						char *plain = insn_plain(&in);
						die_at(m->no, "operand %d was changed: the ROM has '%s' here", j + 1, plain);
					}
					free(x);
					continue;
				}
				if (v.bit[j] >= 0)
				{
					/* "byte.bit": the bit number follows the last dot */
					const DOp *bd = &in.ops[v.bit[j]];
					char *dot = NULL, *q;
					char *bt;
					long long bv;
					int depth = 0;
					for (q = text; *q; q++)
					{
						if (*q == '(' || *q == '[' || *q == '{')
							depth++;
						else if (*q == ')' || *q == ']' || *q == '}')
							depth--;
						else if (*q == '.' && depth == 0 && q > text)
							dot = q;
					}
					if (!dot || !*skipws(dot + 1))
					{
						char *plain = insn_plain(&in);
						die_at(m->no, "operand %d needs its bit number after a dot, as in '%s'", j + 1, plain);
					}
					*dot = '\0';
					bt = xstrdup(skipws(dot + 1));
					rstrip(bt);
					if (!eval_master(m, bt, &bv, "bit number") || bv != bd->value)
						hint_value(m, bt, bd);
					site_add(a, v.bit[j] + 1, bt, bv, false, m);
					free(bt);
					rstrip(text);
				}
				if (v.inv[j])
				{
					char *t = (char *)skipws(text);
					if (*t != '/')
					{
						char *plain = insn_plain(&in);
						die_at(m->no, "operand %d must start with '/' (an inverted bit): the ROM has '%s' here",
							j + 1, plain);
					}
					text = t + 1;
				}
				if (d->kind == K_TEXT)
				{
					char *x = nospace(text), *y = nospace(d->text);
					if (strcasecmp(x, y) != 0)
					{
						char *plain = insn_plain(&in);
						die_at(m->no, "operand %d was changed: the ROM has '%s' here", j + 1, plain);
					}
					free(x);
					free(y);
				}
				else
				{
					char *mid = strip_pre_post(text, d->pre, d->post);
					long long val;
					if (!mid)
					{
						char *plain = insn_plain(&in);
						die_at(m->no, "operand %d must be written %s...%s (the form decides the encoding);\n"
							"  the ROM has '%s' here", j + 1, d->pre[0] ? d->pre : "", d->post, plain);
					}
					if (!eval_master(m, mid, &val, "operand") || !value_fits(d, val))
						hint_value(m, mid, d);
					site_add(a, v.slot[j] + 1, mid, val,
						d->kind == K_CODE || d->kind == K_INDEX || (d->kind == K_DATA), m);
					g_site[g_nsite - 1].callt = d->kind == K_CALLT;
					free(mid);
				}
			}
			ops_free(&so);
		}
		else if (m->kind == LK_DATA && (!strcmp(w, ".byte") || !strcmp(w, ".word")))
		{
			Ops o;
			int j;
			int unit = w[1] == 'b' ? 1 : 2;
			ops_split(ops, &o, m->no);
			for (j = 0; j < o.n; j++)
			{
				long long v;
				eval_master(m, o.v[j], &v, w);
				site_add(a + (unsigned long)(j * unit), 1, o.v[j], v, unit == 2, m);
			}
			ops_free(&o);
		}
	}

	/* ---- slot names: an .equ written at every CALLT of its slot ---- */
	{
		int k;
		for (k = 0; k < g_nsym; k++)
		{
			Sym *sy = &g_sym[k];
			int j, uses = 0;
			bool all = true;
			if (sy->kind != SK_EQU || sy->alias || sy->value < 0 || sy->value > 31 || g_slotName[sy->value])
				continue;
			for (j = 0; j < g_nsite; j++)
			{
				if (!g_site[j].callt || g_site[j].value != sy->value)
					continue;
				if (strcmp(g_site[j].expr, sy->name) == 0)
					uses++;
				else
					all = false;
			}
			if (uses > 0 && all)
				g_slotName[sy->value] = k + 1;
		}
	}

	/* ---- global names: an .equ written at every use of its address ---- */
	{
		int k;
		for (k = 0; k < g_nsym; k++)
		{
			Sym *s = &g_sym[k];
			int j, uses = 0;
			bool all = true;
			if (s->kind != SK_EQU || s->alias || s->value < 0 || s->value >= NADDR || g_labelAt[s->value])
				continue;
			for (j = 0; j < g_nsite; j++)
			{
				if (!g_site[j].addressCtx || g_site[j].value != s->value)
					continue;
				if (strcmp(g_site[j].expr, s->name) == 0)
					uses++;
				else
					all = false;
			}
			if (uses > 0 && all && !g_globalAt[s->value])
			{
				s->global = true;
				g_globalAt[s->value] = k + 1;
			}
		}
	}

	/* ---- generated names ---- */
	for (i = 0; i < g_nsite; i++)
		auto_from_expr(g_site[i].expr);
	index_includes();
	{
		/* pointer tables: .word ranges whose items name generated sub_ labels */
		unsigned long st = 0;
		bool inWord = false;
		for (a = 0; a <= NADDR; a++)
		{
			if (a == NADDR || !g_pop[a] || g_rbeg[a] || g_rt[a] != RT_WORD)
			{
				if (inWord)
				{
					/* a word written as the name of its own address (a label or
					 * generated name) makes the range a pointer table */
					int j;
					bool ptr = false;
					for (j = 0; j < g_nsite; j++)
						if (g_site[j].addr >= st && g_site[j].addr < a && expr_is_name(g_site[j].expr) &&
							addr_name_value(g_site[j].expr) == g_site[j].value)
							ptr = true;
					if (ptr)
					{
						unsigned long x;
						for (x = st; x < a; x++)
							g_rt[x] = RT_PTR;
					}
					inWord = false;
				}
				if (a < NADDR && g_pop[a] && g_rt[a] == RT_WORD)
				{
					inWord = true;
					st = a;
				}
			}
		}
	}
	make_auto_labels();
	make_callt_names();
	/* re-add binding-derived generated names (make_auto_labels cleared them) */
	for (i = 0; i < g_nsite; i++)
		auto_from_expr(g_site[i].expr);

	/* ---- macro lines: the bytes must be the macro's instructions, with a
	 * byte line for each instruction after the first ---- */
	for (i = 0; i < g_nml; i++)
	{
		MLine *m = &g_ml[i];
		int k = m->mac - 1, j;
		unsigned long len, x;
		char *why = NULL;
		if (!m->mac)
			continue;
		MacArgs g;
		Ops args;
		char w[32];
		g_mac[k].on = true;
		mac_prepare(k);
		a = (unsigned long)m->addr;
		if (!mac_match(a, k, false, &len, &why, &g))
			die_at(m->no, "the bytes here aren't the macro %s: %s", g_mac[k].name, why);
		/* the arguments: each must have the value found in the code; one
		 * written differently from how it would print is kept as written */
		ops_split(first_word(m->code, w, sizeof(w)), &args, m->no);
		if (args.n != g_mac[k].np)
			die_at(m->no, "the macro %s takes %d argument(s), not %d", g_mac[k].name, g_mac[k].np, args.n);
		for (j = 0; j < args.n; j++)
		{
			long long v;
			char *x1, *y1;
			if (!eval_master(m, args.v[j], &v, "macro argument") ||
				(g.masked[j] ? (v & 0xFF) : v) != g.val[j])
				die_at(m->no, "argument %d of %s must have the value 0x%llX (the code here has '%s')",
					j + 1, g_mac[k].name, (unsigned long long)g.val[j], g.src[j]);
			auto_from_expr(args.v[j]);
			x1 = nospace(args.v[j]);
			y1 = nospace(g.src[j]);
			/* numbers compare without regard to case (0x2c is 0x2C) */
			if (isdigit((unsigned char)x1[0]) ? strcasecmp(x1, y1) != 0 : strcmp(x1, y1) != 0)
			{
				char *e = map_names(args.v[j], true);
				bind_add(a, ARG_SLOT + j + 1, e);
				free(e);
			}
			free(x1);
			free(y1);
		}
		ops_free(&args);
		macargs_free(&g);
		x = a + g_ilen[a];
		for (j = i + 1; j < g_nml && x < a + len; j++)
		{
			if (g_ml[j].addr < 0)
				continue;
			if (g_ml[j].kind != LK_MBYTES || (unsigned long)g_ml[j].addr != x)
				die_at(g_ml[j].no, "the macro %s at line %d covers 0x%04lX-0x%04lX: each instruction after\n"
					"  the first needs a line with just its address and bytes, here 0x%04lX",
					g_mac[k].name, m->no, a, a + len - 1, x);
			x += g_ilen[x];
		}
		for (; j < g_nml; j++)
			if (g_ml[j].addr >= 0)
			{
				if (g_ml[j].kind == LK_MBYTES)
					die_at(g_ml[j].no, "this byte line is past the end of the macro %s at line %d",
						g_mac[k].name, m->no);
				break;
			}
	}

	/* ---- bindings: where the listing differs from what would be printed ---- */
	for (i = 0; i < g_nsite; i++)
	{
		Site *s = &g_site[i];
		Str def = { 0 };
		char *x, *y;
		if (s->m->kind == LK_CODE)
		{
			Insn in;
			const DOp *d;
			decode_insn(s->addr, &in);
			d = &in.ops[s->slot - 1];
			default_num(&def, &in, d);
		}
		else
		{
			/* .byte/.word item */
			if (g_rt[s->addr] == RT_WORD || g_rt[s->addr] == RT_PTR)
				word_default(&def, g_mem[s->addr] | (g_mem[s->addr + 1] << 8), g_rt[s->addr], false);
			else
				str_addf(&def, "0x%02X", g_mem[s->addr]);
		}
		x = canon(str_c(&def));
		y = canon(s->expr);
		if (strcmp(x, y) != 0)
		{
			char *e = map_names(s->expr, true);
			bind_add(s->addr, s->slot, e);
			free(e);
		}
		free(x);
		free(y);
		free(def.s);
	}

	/* ---- data line breaks: anywhere the listing starts a line the
	 * printer wouldn't ---- */
	{
		int round;
		for (round = 0; round < 4; round++)
		{
			bool changed = false;
			bool *starts = (bool *)calloc(NADDR, 1);
			unsigned long x = 0;
			make_fill_anchors();
			while (x < NADDR)
			{
				unsigned long n, run;
				if (!g_pop[x])
				{
					x++;
					continue;
				}
				starts[x] = true;
				if (g_lc[x] == LC_CODE)
					n = g_ilen[x];
				else if (g_lc[x] == LC_NONE && (run = fill_run(x)) >= FILL_MIN)
					n = run;
				else
					n = (g_lc[x] == LC_BAD) ? 1 : data_line_len(x);
				x += n;
			}
			for (i = 0; i < g_nml; i++)
				if (g_ml[i].addr >= 0 && !starts[g_ml[i].addr])
				{
					g_brk[g_ml[i].addr] = true;
					changed = true;
				}
			free(starts);
			if (!changed)
				break;
		}
	}

	/* ---- macros: where a macro would stand but the listing writes the
	 * instructions out, that site is marked nomacro ---- */
	{
		int *lineAt = (int *)calloc(NADDR, sizeof(int));
		int round;
		for (i = 0; i < g_nml; i++)
			if (g_ml[i].addr >= 0 && !lineAt[g_ml[i].addr])
				lineAt[g_ml[i].addr] = i + 1;
		for (round = 0; round < 16; round++)
		{
			bool changed = false;
			find_macros();
			for (a = 0; a < NADDR; a++)
				if (g_macAt[a] && !(lineAt[a] && g_ml[lineAt[a] - 1].mac == g_macAt[a]))
				{
					g_noMac[a] = true;
					changed = true;
				}
			if (!changed)
				break;
		}
		for (i = 0; i < g_nml; i++)
			if (g_ml[i].mac && g_macAt[g_ml[i].addr] != g_ml[i].mac)
				die_at(g_ml[i].no, "the macro %s can't stand here: a label, comment, operand name or data\n"
					"  break inside it needs a line of its own, or a longer macro matches here. Write\n"
					"  the instructions out instead.", g_mac[g_ml[i].mac - 1].name);
		free(lineAt);
	}

	/* ---- verify: regenerate and compare ----
	 * Generated lines (the header, .org lines, generated labels) are
	 * skipped on both sides, and names of addresses compare by value, so
	 * a renamed label needn't be renamed at every use. */
	{
		Out o = { 0 };
		Str r = { 0 };
		int mi = 0, oi = 0;
		int stale = 0;
		g_nimg = 0;
		{
			int r2;
			for (r2 = 0; r2 < g_ndbRom; r2++)
				add_image(path, g_dbRom[r2].base, g_dbRom[r2].size);
		}
		emit_listing(&o);
		for (;;)
		{
			char *ca, *cb;
			while (mi < g_nml && (g_ml[mi].kind == LK_ALABEL || g_ml[mi].kind == LK_ORG))
				mi++;
			while (oi < o.n && (o.v[oi].kind == LK_GEN || o.v[oi].kind == LK_ORG || o.v[oi].kind == LK_ALABEL))
				oi++;
			if (mi >= g_nml && oi >= o.n)
				break;
			if (oi >= o.n)
				die_at(g_ml[mi].no, "this line couldn't be represented in the database:\n  %s", lines[g_ml[mi].no - 1]);
			render_line(&r, &o.v[oi], true);
			if (mi >= g_nml)
				die_at(nlines, "the listing ends early; this line would come next:\n  %s", str_c(&r));
			ca = canon(str_c(&r));
			cb = canon(lines[g_ml[mi].no - 1]);
			if (strcmp(ca, cb) != 0)
				die_at(g_ml[mi].no, "this line couldn't be represented in the database:\n  %s\n"
					"Regenerating the listing would give, at this point:\n  %s\n"
					"Common causes: an edited instruction or byte column, a label moved away from\n"
					"  its code, or a comment moved onto a line where it can't stay.",
					lines[g_ml[mi].no - 1], str_c(&r));
			free(ca);
			free(cb);
			{
				char *x = nospace(str_c(&r)), *y = nospace(lines[g_ml[mi].no - 1]);
				if (strcmp(x, y) != 0)
					stale++;
				free(x);
				free(y);
			}
			mi++;
			oi++;
		}
		g_stale = stale;
		free(r.s);
		out_free(&o);
	}

	/* ---- write ---- */
	g_errFile = NULL;
	{
		const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
		if (dbOut)
			write_db(dbOut, base);
		if (symOut)
			write_symbols_file(symOut, base);
		if (dbOut || symOut)
			fprintf(stderr, "78k_disasm: %s checked.%s%s%s%s\n", path,
				dbOut ? " Wrote " : "", dbOut ? dbOut : "", symOut ? (dbOut ? " and " : " Wrote ") : "",
				symOut ? symOut : "");
		else
			fprintf(stderr, "78k_disasm: %s checked; it converts cleanly (no files written).\n", path);
		if (g_stale)
			fprintf(stderr, "78k_disasm: %d line(s) use an older name for an address, or other generated\n"
				"  text that has changed. Regenerate the listing from the database to update them.\n", g_stale);
	}
	free(dir);
	free(lines);
	free(text);
}

/* =====================================================================
 * Cross-reference report
 * ===================================================================== */

typedef struct XRef
{
	long target;
	unsigned long from;
	char kind;   /* C call, J jump/branch, R data access, I immediate */
	int width;   /* immediates: 8 or 16 */
} XRef;

static int cmp_xref(const void *x, const void *y)
{
	const XRef *a = (const XRef *)x, *b = (const XRef *)y;
	int ai = a->kind == 'I', bi = b->kind == 'I';
	if (ai != bi)
		return ai - bi;
	if (ai && a->width != b->width)
		return a->width - b->width;
	if (a->target != b->target)
		return a->target < b->target ? -1 : 1;
	return a->from < b->from ? -1 : a->from > b->from;
}

/* A bit reference: the byte (an address, or a register named in text),
 * the bit, how the instruction uses it, and where. */
typedef struct BitRef
{
	long addr;          /* -1 for a register */
	char reg[8];
	int bit;
	char kind;          /* S C N T R W, see write_xref() */
	unsigned long from;
	char *bitName;      /* the bit as written, if it is a name */
} BitRef;

static int cmp_bitref(const void *x, const void *y)
{
	const BitRef *a = (const BitRef *)x, *b = (const BitRef *)y;
	int c;
	if ((a->addr < 0) != (b->addr < 0))
		return a->addr < 0 ? 1 : -1;
	if (a->addr != b->addr)
		return a->addr < b->addr ? -1 : 1;
	if ((c = strcmp(a->reg, b->reg)) != 0)
		return c;
	if (a->bit != b->bit)
		return a->bit - b->bit;
	return a->from < b->from ? -1 : a->from > b->from;
}

static char bit_kind(const Insn *in, const View *v)
{
	switch (in->op->mnemonic)
	{
	case SET1: return 'S';
	case CLR1: return 'C';
	case NOT1: return 'N';
	case BT: case BF: case BTCLR: case BFSET: return 'T';
	case MOV1: return v->cy[0] ? 'R' : 'W';
	default: return 'R';   /* AND1, OR1, XOR1 and the inverted forms */
	}
}

static void write_bit_refs(FILE *fp)
{
	BitRef *r = NULL;
	int n = 0, cap = 0, i;
	unsigned long a;
	for (a = 0; a < NADDR; a++)
	{
		Insn in;
		View v;
		int j;
		if (g_lc[a] != LC_CODE || !decode_insn(a, &in))
			continue;
		insn_view(&in, &v);
		for (j = 0; j < v.n; j++)
		{
			const DOp *d;
			const Bind *b;
			BitRef x;
			if (v.bit[j] < 0)
				continue;
			d = &in.ops[v.slot[j]];
			memset(&x, 0, sizeof(x));
			if (d->kind == K_TEXT)
			{
				x.addr = -1;
				snprintf(x.reg, sizeof(x.reg), "%s", d->text);
			}
			else
				x.addr = d->value;
			x.bit = (int)in.ops[v.bit[j]].value;
			x.kind = bit_kind(&in, &v);
			x.from = a;
			b = bind_find(a, v.bit[j] + 1);
			x.bitName = (b && !isdigit((unsigned char)b->expr[0])) ? b->expr : NULL;
			GROW(r, n, cap);
			r[n++] = x;
		}
	}
	if (n > 1)
		qsort(r, (size_t)n, sizeof(BitRef), cmp_bitref);
	fprintf(fp, "\nBit references (S set1, C clr1, N not1, T tested by bt/bf/btclr/bfset,\n");
	fprintf(fp, "R read into CY, W written from CY):\n\n");
	for (i = 0; i < n; )
	{
		int j = i, cnt = 0, k;
		char where[40], name[96];
		const char *bn = NULL;
		if (r[i].addr >= 0)
		{
			snprintf(where, sizeof(where), "%04lX.%d", (unsigned long)r[i].addr, r[i].bit);
			k = name_for(r[i].addr, CTX_DATA);
		}
		else
		{
			snprintf(where, sizeof(where), "%s.%d", r[i].reg, r[i].bit);
			k = -1;
		}
		for (j = i; j < n && r[j].addr == r[i].addr && !strcmp(r[j].reg, r[i].reg) && r[j].bit == r[i].bit; j++)
			if (r[j].bitName && !bn)
				bn = r[j].bitName;
		if (k >= 0 || bn)
			snprintf(name, sizeof(name), "%s.%s", k >= 0 ? g_sym[k].name : (r[i].addr >= 0 ? "" : r[i].reg),
				bn ? bn : "");
		else
			name[0] = '\0';
		if (k >= 0 && !bn)
			snprintf(name, sizeof(name), "%s.%d", g_sym[k].name, r[i].bit);
		fprintf(fp, "%-9s %-27s", where, name);
		for (j = i; j < n && r[j].addr == r[i].addr && !strcmp(r[j].reg, r[i].reg) && r[j].bit == r[i].bit; j++)
		{
			if (cnt && cnt % 6 == 0)
				fprintf(fp, "\n%37s", "");
			fprintf(fp, " %c %04lX", r[j].kind, r[j].from);
			cnt++;
		}
		fprintf(fp, "\n");
		i = j;
	}
	free(r);
}

/* Every CALLT slot the code uses: its name, the routine in it (or why
 * there is none), and each CALLT of it. */
static void write_callt_refs(FILE *fp)
{
	unsigned long a;
	int slot;
	bool any = false;
	for (slot = 0; slot < 32; slot++)
	{
		int cnt = 0;
		for (a = 0; a < NADDR; a++)
		{
			Insn in;
			int j;
			if (g_lc[a] != LC_CODE || !decode_insn(a, &in))
				continue;
			for (j = 0; j < in.n; j++)
			{
				const DOp *d = &in.ops[j];
				if (d->kind != K_CALLT || (int)(d->value & 31) != slot)
					continue;
				if (cnt == 0)
				{
					char name[64], dest[64];
					unsigned long w = callt_word(slot);
					int k = g_slotName[slot] ? g_slotName[slot] - 1 : g_calltAuto[slot] ? g_calltAuto[slot] - 1 : -1;
					if (!any)
						fprintf(fp, "\nCALLT slots:\n\n");
					any = true;
					snprintf(name, sizeof(name), "%s", k >= 0 ? g_sym[k].name : "");
					if (g_calltBase < 0)
						snprintf(dest, sizeof(dest), "(no CALLT table set)");
					else if (w == 0xFFFF)
						snprintf(dest, sizeof(dest), "(empty)");
					else
					{
						int t = name_for((long)w, CTX_CODE);
						snprintf(dest, sizeof(dest), "-> %04lX %s", w, t >= 0 ? g_sym[t].name : "");
					}
					fprintf(fp, "%02XH  %-24s %-24s", slot, name, dest);
				}
				else if (cnt % 6 == 0)
					fprintf(fp, "\n%54s", "");
				fprintf(fp, " %04lX", a);
				cnt++;
			}
		}
		if (cnt)
			fprintf(fp, "\n");
	}
}

/* Cross-reference: each address the code uses, with its name and every
 * instruction that refers to it; then each immediate value. */
static void write_xref(const char *path)
{
	FILE *fp = open_out(path);
	XRef *v = NULL;
	int n = 0, cap = 0, i;
	unsigned long a;

	for (a = 0; a < NADDR; a++)
	{
		Insn in;
		int j;
		if (g_lc[a] != LC_CODE || !decode_insn(a, &in))
			continue;
		for (j = 0; j < in.n; j++)
		{
			const DOp *d = &in.ops[j];
			XRef x;
			x.from = a;
			x.target = d->value;
			x.width = d->width;
			if (d->kind == K_CODE)
				x.kind = insn_is_call(&in) ? 'C' : 'J';
			else if (d->kind == K_DATA || d->kind == K_INDEX)
				x.kind = 'R';
			else if (d->kind == K_IMM)
				x.kind = 'I';
			else if (d->kind == K_CALLT)
			{
				/* a call through the CALLT table: listed under the routine
				 * in that slot (and in the slot list below) */
				unsigned long w = callt_word((int)(d->value & 31));
				if (w == 0xFFFF)
					continue;
				x.kind = 'T';
				x.target = (long)w;
			}
			else
				continue;
			GROW(v, n, cap);
			v[n++] = x;
		}
	}
	if (n > 1)
		qsort(v, (size_t)n, sizeof(XRef), cmp_xref);

	fprintf(fp, "Cross-reference. Each address the code uses, with every instruction that\n");
	fprintf(fp, "refers to it (C = call, T = call through the CALLT table, J = jump or\n");
	fprintf(fp, "branch, R = data access), then each immediate value, each CALLT slot and\n");
	fprintf(fp, "each bit, with the instructions that use them.\n\n");
	for (i = 0; i < n && v[i].kind != 'I'; )
	{
		int j = i, cnt = 0;
		int k = name_for(v[i].target, v[i].kind == 'R' ? CTX_DATA : CTX_CODE);
		fprintf(fp, "%04lX  %-30s", (unsigned long)(v[i].target & 0xFFFF), k >= 0 ? g_sym[k].name : "");
		while (j < n && v[j].kind != 'I' && v[j].target == v[i].target)
		{
			if (cnt && cnt % 6 == 0)
				fprintf(fp, "\n%36s", "");
			fprintf(fp, " %c %04lX", v[j].kind, v[j].from);
			cnt++;
			j++;
		}
		fprintf(fp, "\n");
		i = j;
	}
	fprintf(fp, "\nImmediate values:\n\n");
	while (i < n)
	{
		int j = i, cnt = 0;
		char val[16];
		snprintf(val, sizeof(val), v[i].width == 8 ? "#0x%02lX" : "#0x%04lX", v[i].target & 0xFFFF);
		fprintf(fp, "%-8s", val);
		while (j < n && v[j].target == v[i].target && v[j].width == v[i].width)
		{
			if (cnt && cnt % 10 == 0)
				fprintf(fp, "\n%8s", "");
			fprintf(fp, " %04lX", v[j].from);
			cnt++;
			j++;
		}
		fprintf(fp, "\n");
		i = j;
	}
	write_callt_refs(fp);
	write_bit_refs(fp);
	free(v);
	close_out(fp, path);
}

/* -a / --vectors ADDR: the 78K/III vector table (32 words, the layout of
 * 0000H-003FH) is at ADDR in the loaded image: 0000H for the mask ROM, or
 * where a firmware image keeps its own. Its words become a pointer table,
 * one per line, each noted with its interrupt; handlers get names, and
 * on a first look they are walked as code. */
static void set_vector_table(unsigned long base, bool fresh)
{
	char spec[48];
	size_t v;
	if (base + 0x3F >= NADDR || !g_pop[base] || !g_pop[base + 0x3F])
	{
		warn("the vector table at 0x%04lX-0x%04lX isn't loaded; skipped", base, base + 0x3F);
		return;
	}
	snprintf(spec, sizeof(spec), "0x%04lX-0x%04lX:ptr/1", base, base + 0x40);
	apply_data_option(spec);
	if (!g_itemHead[base])
	{
		char *t = xasprintf("; Vector table (%04lXH-%04lXH)", base, base + 0x3F);
		if (base != 0)
			item_add(base, IT_TEXT, "");
		item_add(base, IT_TEXT, RULE);
		item_add(base, IT_TEXT, t);
		item_add(base, IT_TEXT, RULE);
		free(t);
	}
	for (v = 0; v < NVEC; v++)
	{
		unsigned long slot = base + VECTORS[v].slot;
		unsigned long t = g_mem[slot] | (g_mem[slot + 1] << 8);
		size_t w, nsh = 0;
		bool first = true;
		char nm[48];
		if (!g_eol[slot])
		{
			char *c = xasprintf("%s vector", VECTORS[v].name);
			eol_add(slot, c);
			free(c);
		}
		if (t == 0xFFFF || t >= NADDR || !g_pop[t])
			continue;   /* unused, or outside the loaded images */
		if (fresh)
			add_entry(t, true);
		for (w = 0; w < NVEC; w++)
		{
			unsigned long s2 = base + VECTORS[w].slot;
			if ((unsigned long)(g_mem[s2] | (g_mem[s2 + 1] << 8)) == t)
			{
				if (w < v)
					first = false;
				nsh++;
			}
		}
		if (!first || g_labelAt[t])
			continue;   /* named at its first vector, or already named */
		if (nsh == 1)
		{
			size_t j;
			for (j = 0; VECTORS[v].name[j]; j++)
				nm[j] = (char)tolower((unsigned char)VECTORS[v].name[j]);
			nm[j] = '\0';
			strcat(nm, "_handler");
		}
		else
		{
			Str lst = { 0 };
			str_add(&lst, "; Shared handler for:");
			for (w = 0; w < NVEC; w++)
			{
				unsigned long s2 = base + VECTORS[w].slot;
				if ((unsigned long)(g_mem[s2] | (g_mem[s2 + 1] << 8)) == t)
					str_addf(&lst, " %s", VECTORS[w].name);
			}
			item_add(t, IT_TEXT, str_c(&lst));
			free(lst.s);
			snprintf(nm, sizeof(nm), "int_handler_%04lX", t);
		}
		if (sym_find(nm) < 0)
		{
			int k = sym_add(nm, SK_LABEL, NULL, 0);
			set_label(k, t);
			item_add(t, IT_LABEL, nm);
		}
	}
}

/* --callt ADDR: the CALLT table is at ADDR in the loaded image. Its 32
 * entries become a pointer table, one per line, each noted with its slot;
 * on a first look the routines they point to are walked as code. */
static void set_callt_table(long base, bool fresh)
{
	char spec[48];
	int k;
	g_calltBase = base;
	snprintf(spec, sizeof(spec), "0x%04lX-0x%04lX:ptr/1", (unsigned long)base, (unsigned long)base + 0x40);
	apply_data_option(spec);
	if (!g_itemHead[base])
	{
		item_add((unsigned long)base, IT_TEXT, "");
		item_add((unsigned long)base, IT_TEXT, RULE);
		item_add((unsigned long)base, IT_TEXT, "; CALLT table: CALLT [n] calls the routine in entry n");
		item_add((unsigned long)base, IT_TEXT, RULE);
	}
	for (k = 0; k < 32; k++)
	{
		unsigned long a = (unsigned long)base + 2 * (unsigned long)k;
		unsigned long w = callt_word(k);
		if (!g_eol[a])
		{
			char c[24];
			snprintf(c, sizeof(c), "slot %02XH", k);
			eol_add(a, c);
		}
		if (fresh && w != 0xFFFF && w < NADDR && g_pop[w])
			add_entry(w, true);
	}
}

/* =====================================================================
 * Command line
 * ===================================================================== */

static void usage(FILE *fp)
{
	fprintf(fp,
		"NEC 78K/III disassembler v" DISASM_VERSION "\n"
		"\n"
		"Disassemble:\n"
		"  78k_disasm [-b ADDR] ROM [[-b ADDR] ROM ...] [options]\n"
		"    -d, --db FILE        database (.inc) with names, comments and layout\n"
		"    --listing FILE       write the listing (address/byte columns + source)\n"
		"    --source FILE        write the same without the columns (assembles with vasm)\n"
		"    --xref FILE          write a cross-reference of addresses, constants and bits\n"
		"                         (with no output option, the listing goes to the screen)\n"
		"    -b, --base ADDR      load the next ROM at ADDR (default 0, or from a file\n"
		"                         name ending in 0xADDR, e.g. firmware_0x8000.bin)\n"
		"    --ignore-crc         use the database even if the ROM doesn't match it\n"
		"  Code, data and names (a first look, or added to a database):\n"
		"    -a                   decode the vector table at 0000H and follow it\n"
		"    --vectors ADDR       the same for a vector table at ADDR (e.g. 0x8000 in a\n"
		"                         firmware image)\n"
		"    -t, --entry ADDR[:NAME]  code starts here (repeatable)\n"
		"    -k, --data START-END[:FORMAT]  these bytes are data (repeatable); FORMAT is\n"
		"                         byte, word, string or ptr (a table of code addresses),\n"
		"                         optionally /N items per line, e.g. :string/8\n"
		"    -f, --follow         also disassemble every call and branch target\n"
		"    --callt ADDR         the CALLT table is at ADDR in the loaded image\n"
		"    -i, --include FILE   names (.equ lines) and macros to use, e.g. another\n"
		"                         ROM's entry points; recorded in the database\n"
		"    -N, --no-auto-labels don't generate sub_/loc_/dat_/ram_/ext_ names\n"
		"    -m, --macro NAME     print the code as macro NAME (defined in an include file)\n"
		"                         wherever it matches; recorded in the database\n"
		"\n"
		"Extract (turn an edited listing into a database):\n"
		"  78k_disasm --extract LISTING [--db FILE] [--symbols FILE]\n"
		"    --db FILE            write the database\n"
		"    --symbols FILE       write just the names, as an .include file for your code\n"
		"    (with neither, the listing is only checked)\n"
		"\n"
		"Other options:\n"
		"    -h, --help           show this help\n"
		"    --version            print version, copyright and license information\n"
		"\n"
		"Addresses are hex: 0x8000, $8000 or 8000. Output files are never replaced: an\n"
		"output that already exists is refused.\n");
}

static const char *need_arg(int argc, const char **argv, int *i)
{
	if (*i + 1 >= argc)
		die("%s needs a value (see --help)", argv[*i]);
	return argv[++*i];
}

int main(int argc, const char **argv)
{
	const char *dbPath = NULL, *listOut = NULL, *srcOut = NULL, *xrefOut = NULL;
	const char *extractIn = NULL, *symOut = NULL;
	unsigned long pendingBase = 0;
	bool havePendingBase = false;
	long calltOpt = -1, vectorsOpt = -1;
	const char **includes = NULL;
	int nincludes = 0, capincludes = 0;
	const char **macros = NULL;
	int nmacros = 0, capmacros = 0;
	bool freshOpts = false;
	const char **roms = NULL;
	unsigned long *bases = NULL;
	int nroms = 0, caproms = 0, capb = 0;
	int i;

	/* where the program lives (for sfr/uPD78312A.inc) */
	{
		ssize_t n = readlink("/proc/self/exe", g_exeDir, sizeof(g_exeDir) - 1);
		if (n > 0)
		{
			char *s;
			g_exeDir[n] = '\0';
			s = strrchr(g_exeDir, '/');
			if (s)
				*s = '\0';
		}
		else
		{
			char *d = dir_of(argv[0]);
			snprintf(g_exeDir, sizeof(g_exeDir), "%s", d);
			free(d);
		}
	}

	for (i = 1; i < argc; i++)
	{
		const char *a = argv[i];
		if (!strcmp(a, "-h") || !strcmp(a, "--help"))
		{
			usage(stdout);
			return 0;
		}
		else if (!strcmp(a, "--version"))
		{
			printf("NEC 78K/III Disassembler %s\n"
				"Copyright (C) 2026 Dave, KY4WI\n"
				"Portions Copyright (C) 2021 wonst719\n"
				"\n"
				"License: GPLv3-only\n"
				"This software comes with ABSOLUTELY NO WARRANTY.\n"
				"See LICENSE and LICENSE.MIT for details.\n", DISASM_VERSION);
			return 0;
		}
		else if (!strcmp(a, "--extract"))
			extractIn = need_arg(argc, argv, &i);
		else if (!strcmp(a, "-d") || !strcmp(a, "--db"))
			dbPath = need_arg(argc, argv, &i);
		else if (!strcmp(a, "--symbols"))
			symOut = need_arg(argc, argv, &i);
		else if (!strcmp(a, "--listing"))
			listOut = need_arg(argc, argv, &i);
		else if (!strcmp(a, "--source"))
			srcOut = need_arg(argc, argv, &i);
		else if (!strcmp(a, "--xref"))
			xrefOut = need_arg(argc, argv, &i);
		else if (!strcmp(a, "-b") || !strcmp(a, "--base"))
		{
			pendingBase = need_addr(need_arg(argc, argv, &i), "-b");
			havePendingBase = true;
		}
		else if (!strcmp(a, "-t") || !strcmp(a, "--entry"))
		{
			const char *v = need_arg(argc, argv, &i);
			(void)v;
			freshOpts = true;
		}
		else if (!strcmp(a, "-k") || !strcmp(a, "--data"))
		{
			need_arg(argc, argv, &i);
			freshOpts = true;
		}
		else if (!strcmp(a, "-a"))
			vectorsOpt = 0;
		else if (!strcmp(a, "--vectors"))
			vectorsOpt = (long)need_addr(need_arg(argc, argv, &i), "--vectors");
		else if (!strcmp(a, "-i") || !strcmp(a, "--include"))
		{
			GROW(includes, nincludes, capincludes);
			includes[nincludes++] = need_arg(argc, argv, &i);
		}
		else if (!strcmp(a, "-f") || !strcmp(a, "--follow"))
			g_follow = freshOpts = true;
		else if (!strcmp(a, "-N") || !strcmp(a, "--no-auto-labels"))
			g_noAuto = true;
		else if (!strcmp(a, "-m") || !strcmp(a, "--macro"))
		{
			GROW(macros, nmacros, capmacros);
			macros[nmacros++] = need_arg(argc, argv, &i);
		}
		else if (!strcmp(a, "--callt"))
		{
			calltOpt = (long)need_addr(need_arg(argc, argv, &i), "--callt");
			if (calltOpt + 0x3F >= NADDR)
				die("--callt 0x%04lX: the table's 32 entries (64 bytes) must fit below 0x10000", calltOpt);
		}
		else if (!strcmp(a, "--ignore-crc"))
			g_ignoreCrc = true;
		else if (a[0] == '-' && a[1])
			die("unknown option '%s' (see --help)", a);
		else
		{
			unsigned long base = pendingBase;
			if (!havePendingBase && !base_from_filename(a, &base))
				base = 0;
			GROW(roms, nroms, caproms);
			GROW(bases, nroms, capb);
			roms[nroms] = a;
			bases[nroms] = base;
			nroms++;
			pendingBase = 0;
			havePendingBase = false;
		}
	}

	if (extractIn)
	{
		if (nroms || listOut || srcOut || xrefOut || freshOpts || g_ignoreCrc || g_noAuto || calltOpt >= 0 || vectorsOpt >= 0 ||
			nincludes || nmacros)
			die("--extract only takes the listing and --db/--symbols outputs; the ROM is rebuilt\n"
				"  from the listing itself. Example:\n"
				"  78k_disasm --extract tkx4x_maskrom.lst --db tkx4x_maskrom.inc");
		add_output(dbPath);
		add_output(symOut);
		check_outputs();
		extract_listing(extractIn, dbPath, symOut);
		return 0;
	}

	if (symOut)
		die("--symbols is written by --extract (from a listing)");
	if (nroms == 0)
	{
		usage(stderr);
		return 1;
	}
	add_output(listOut);
	add_output(srcOut);
	add_output(xrefOut);
	check_outputs();
	for (i = 0; i < nroms; i++)
		load_image(roms[i], bases[i]);

	if (dbPath)
	{
		g_fresh = false;
		load_db(dbPath);
		check_db_roms();
	}
	else
		load_include(SFR_INCLUDE, NULL, false, true);

	/* first-look options (also usable with a database, to change layout) */
	for (i = 1; i < argc; i++)
	{
		const char *a = argv[i];
		if (!strcmp(a, "-k") || !strcmp(a, "--data"))
			apply_data_option(argv[++i]);
		else if (!strcmp(a, "-d") || !strcmp(a, "--db") || !strcmp(a, "--listing") || !strcmp(a, "--source") ||
			!strcmp(a, "--xref") || !strcmp(a, "-b") || !strcmp(a, "--base") || !strcmp(a, "--symbols") ||
			!strcmp(a, "--extract") || !strcmp(a, "--callt") || !strcmp(a, "--vectors") ||
			!strcmp(a, "-i") || !strcmp(a, "--include") || !strcmp(a, "-m") || !strcmp(a, "--macro"))
			i++;
	}
	for (i = 0; i < nincludes; i++)
	{
		const char *sl = strrchr(includes[i], '/');
		char *d = dir_of(includes[i]);
		/* found again by name next to the database, in the current
		 * directory or in sfr/, so the listing names it without a path */
		load_include(sl ? sl + 1 : includes[i], sl ? d : NULL, true, true);
		free(d);
	}
	for (i = 0; i < nmacros; i++)
		mac_on(macros[i], "--macro");
	if (vectorsOpt >= 0)
		set_vector_table((unsigned long)vectorsOpt, !dbPath);
	if (calltOpt >= 0)
		set_callt_table(calltOpt, !dbPath);
	for (i = 1; i < argc; i++)
	{
		const char *a = argv[i];
		if (!strcmp(a, "-t") || !strcmp(a, "--entry"))
		{
			char *s = xstrdup(argv[++i]);
			char *colon = strchr(s, ':');
			unsigned long ea;
			if (colon)
				*colon = '\0';
			ea = need_addr(s, "-t");
			add_entry(ea, true);
			if (colon && colon[1])
			{
				int k = sym_find(colon + 1);
				if (k >= 0)
					die("-t %s: '%s' is already defined", argv[i], colon + 1);
				k = sym_add(colon + 1, SK_LABEL, "command line", 0);
				if (g_labelAt[ea])
					die("-t %s: 0x%04lX already has the label '%s'", argv[i], ea, g_sym[g_labelAt[ea] - 1].name);
				set_label(k, ea);
				item_add(ea, IT_LABEL, colon + 1);
			}
			free(s);
		}
		else if (!strcmp(a, "-d") || !strcmp(a, "--db") || !strcmp(a, "--listing") || !strcmp(a, "--source") ||
			!strcmp(a, "--xref") || !strcmp(a, "-b") || !strcmp(a, "--base") || !strcmp(a, "-k") ||
			!strcmp(a, "--data") || !strcmp(a, "--callt") || !strcmp(a, "--vectors") ||
			!strcmp(a, "-i") || !strcmp(a, "--include") || !strcmp(a, "-m") || !strcmp(a, "--macro"))
			i++;
	}

	if (dbPath)
	{
		fill_layout_gaps();
		decode_layout();
		if (g_nentries > 0)
		{
			/* -t with a database: walk from there over data bytes */
			int e;
			for (e = 0; e < g_nentries; e++)
			{
				unsigned long x = g_entries[e];
				unsigned long k;
				for (k = x; k < NADDR && g_pop[k] && g_rt[k] != RT_CODE; k++)
					g_rt[k] = RT_NONE;
				walk(x);
				for (k = x; k < NADDR && g_pop[k] && g_rt[k] == RT_NONE; k++)
					g_rt[k] = (g_lc[k] == LC_CODE || g_lc[k] == LC_BODY) ? RT_CODE : RT_BYTE;
				g_rbeg[x] = true;
				for (k = x; k < NADDR && g_pop[k] && g_rt[k] == RT_CODE && !(k > x && g_rbeg[k]); k++)
					;
				if (k < NADDR && g_pop[k] && !g_rbeg[k])
					g_rbeg[k] = true;
			}
			decode_layout();
		}
	}
	else
		fresh_analysis();

	index_includes();
	make_auto_labels();
	make_callt_names();

	{
		Out o = { 0 };
		emit_listing(&o);
		if (listOut)
			write_listing(&o, listOut, true);
		if (srcOut)
			write_listing(&o, srcOut, false);
		if (xrefOut)
			write_xref(xrefOut);
		if (!listOut && !srcOut && !xrefOut)
			write_listing(&o, NULL, true);
		out_free(&o);
	}
	return 0;
}

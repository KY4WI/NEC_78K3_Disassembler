/*
 * SPDX-License-Identifier: GPL-3.0-only
 * NEC 78K/III Disassembler 1.0
 * Copyright (c) 2026 Dave, KY4WI
 * Project includes upstream MIT-licensed material (see LICENSE.MIT).
 */

#pragma once

/*
 * Instruction decoding into structured operands.
 *
 * decode_insn() turns the bytes at an address into an Insn: the opcode
 * table row plus one DOp per operand slot, in the order the assembler
 * reads them. Each DOp says what kind of thing the operand is and holds
 * its numeric value. Nothing here prints labels or knows about symbols;
 * the printer, the reference collector, the per-site bindings and the
 * extractor all work from these records, so there is exactly one place
 * where instruction bytes are interpreted.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "Types.h"
#include "Mnemonic.h"
#include "Operand.h"
#include "Opcode.h"
#include "Opcodes.h"

typedef enum OpKind
{
	K_TEXT,    /* fixed text: register, keyword, [de+], register list ... */
	K_IMM,     /* immediate: #value (width 8 or 16) */
	K_DATA,    /* data address: saddr, saddrp, [saddrp], sfr[], !addr16 */
	K_CODE,    /* code address: relative branch, !addr16 jump/call, !addr11 */
	K_BIT,     /* bit number 0-7 (its own operand field) */
	K_CYBIT,   /* cy.N (bit number attached to CY) */
	K_COUNT,   /* shift/rotate count */
	K_DISP,    /* [reg+disp]: the 8-bit displacement */
	K_INDEX,   /* [base+reg]: the 16-bit base address */
	K_CALLT    /* callt [n]: the 5-bit slot field */
} OpKind;

typedef enum DataForm
{
	DF_SADDR,   /* short direct, printed bare */
	DF_INDIR,   /* [saddrp], printed in brackets */
	DF_SFR,     /* sfr[...] */
	DF_ABS      /* !addr16 */
} DataForm;

typedef struct DOp
{
	OpKind kind;
	int type;          /* OperandType from the opcode table */
	long value;        /* address, immediate, bit number, ... */
	int width;         /* bits: 8 or 16 (immediates, displacements) */
	DataForm form;     /* K_DATA only */
	bool absolute;     /* K_CODE: !addr16 / !addr11 form */
	char pre[16];      /* text around the number, e.g. "[hl+" */
	char post[16];     /*                         and "]"     */
	char text[40];     /* K_TEXT: the whole operand */
	bool invalid;      /* a field code with no defined meaning */
} DOp;

typedef struct Insn
{
	unsigned long addr;
	int len;
	byte b[5];
	const Opcode *op;
	int n;
	DOp ops[3];
} Insn;

/* Memory supplied by the main program. */
static bool mem_read_byte(unsigned long addr, byte *out);

static uint16 d_word(const byte *b, int off)
{
	return (uint16)(b[off] | (b[off + 1] << 8));
}

static byte d_field(const Operand *o, const byte *b)
{
	return (byte)((b[o->byteOffset] & o->opMask) >> o->opShift);
}

/* Short-direct address of the first (or only) saddr operand. */
static uint16 d_saddr(byte f)
{
	return (uint16)(f < 0x20 ? 0xFF00 + f : 0xFE00 + f);
}

/* Source operand of the two-saddr forms (MOV saddr,saddr' and friends):
 * the instruction's second byte, mapped like any other saddr. The
 * destination is the third byte. Confirmed against real code: mask ROM
 * 179BH (38 26 2C) copies panel_rx_byte FE26H to FE2CH, and TK-941 AB00
 * C17FH (38 F0 29) loads eeprom_addr FE29H from X in an EEPROM write
 * loop. */
static uint16 d_saddr_fe(byte f)
{
	return d_saddr(f);
}

static const char *d_reg8(int code, bool full)
{
	/* r (4-bit field, full=true) and r1 (3-bit field) */
	static const char *const r[16] = {
		"x", "a", "c", "b", "x'", "a'", "c'", "b'",
		"vpl", "vph", "upl", "uph", "e", "d", "l", "h"
	};
	if (code < 0 || code > (full ? 15 : 7))
		return NULL;
	return r[code];
}

static const char *d_rp1(int code)
{
	/* rp1 / rpl: 3-bit field. Confirmed against the manual's rp1,!addr16
	 * tables (U12117EJ2V0UM00 p.137-138). */
	static const char *const r[8] = { "ax", "vp", "bc", "up", "ax'", "de", "bc'", "hl" };
	return (code >= 0 && code < 8) ? r[code] : NULL;
}

static void d_set_text(DOp *d, const char *s)
{
	d->kind = K_TEXT;
	if (s)
		snprintf(d->text, sizeof(d->text), "%s", s);
	else
	{
		snprintf(d->text, sizeof(d->text), "?");
		d->invalid = true;
	}
}

static void d_set_num(DOp *d, OpKind k, long v, int width, const char *pre, const char *post)
{
	d->kind = k;
	d->value = v;
	d->width = width;
	snprintf(d->pre, sizeof(d->pre), "%s", pre);
	snprintf(d->post, sizeof(d->post), "%s", post);
}

/* Fills one DOp from one opcode-table operand. */
static void d_operand(const Insn *in, const Operand *o, DOp *d)
{
	const byte *b = in->b;
	byte f;

	memset(d, 0, sizeof(*d));
	d->type = o->opType;

	switch (o->opType)
	{
	case OT_A:     d_set_text(d, "a"); break;
	case OT_A_S:   d_set_text(d, "as"); break;
	case OT_AX:    d_set_text(d, "ax"); break;
	case OT_CY:    d_set_text(d, "cy"); break;
	case OT_PSW:   d_set_text(d, "psw"); break;
	case OT_PSWL:  d_set_text(d, "pswl"); break;
	case OT_PSWH:  d_set_text(d, "pswh"); break;
	case OT_SP:    d_set_text(d, "sp"); break;
	case OT_X:     d_set_text(d, "x"); break;
	case OT_STBC:  d_set_text(d, "stbc"); break;
	case OT_WDM:   d_set_text(d, "wdm"); break;
	case OT_DE_A_P: d_set_text(d, "[de+]"); break;
	case OT_DE_S_P: d_set_text(d, "[de-]"); break;
	case OT_HL_A_P: d_set_text(d, "[hl+]"); break;
	case OT_HL_S_P: d_set_text(d, "[hl-]"); break;

	case OT_R:   d_set_text(d, d_reg8(d_field(o, b), true)); break;
	case OT_R1:  d_set_text(d, d_reg8(d_field(o, b), false)); break;
	case OT_R2:  d_set_text(d, d_field(o, b) <= 1 ? (d_field(o, b) ? "b" : "c") : NULL); break;

	case OT_RP:
	{
		static const char *const r[8] = { "ax", "bc", "ax'", "bc'", "vp", "up", "de", "hl" };
		f = d_field(o, b);
		d_set_text(d, f < 8 ? r[f] : NULL);
		break;
	}
	case OT_RP1:
	case OT_RPL:
		d_set_text(d, d_rp1(d_field(o, b)));
		break;
	case OT_RP1P:
	{
		const char *r = d_rp1(d_field(o, b));
		char t[16];
		if (r)
			snprintf(t, sizeof(t), "[%s]", r);
		d_set_text(d, r ? t : NULL);
		break;
	}
	case OT_RP2:
	{
		static const char *const r[4] = { "vp", "up", "de", "hl" };
		f = d_field(o, b);
		d_set_text(d, f < 4 ? r[f] : NULL);
		break;
	}

	case OT_MEM_S:
	{
		static const char *const m[6] = { "[de+]", "[hl+]", "[de-]", "[hl-]", "[de]", "[hl]" };
		f = d_field(o, b);
		d_set_text(d, f < 6 ? m[f] : NULL);
		break;
	}

	case OT_MEM:
	{
		byte mod = (byte)(b[0] & 0x1F);
		f = d_field(o, b);
		if (mod == 0x16)
		{
			static const char *const m[8] = { "[de+]", "[hl+]", "[de-]", "[hl-]", "[de]", "[hl]", "[vp]", "[up]" };
			d_set_text(d, m[f & 7]);
		}
		else if (mod == 0x17)
		{
			static const char *const m[6] = { "[de+a]", "[hl+a]", "[de+b]", "[hl+b]", "[vp+de]", "[vp+hl]" };
			d_set_text(d, f < 6 ? m[f] : NULL);
		}
		else if (mod == 0x06)
		{
			static const char *const r[5] = { "[de+", "[sp+", "[hl+", "[up+", "[vp+" };
			if (f < 5)
				d_set_num(d, K_DISP, b[2], 8, r[f], "]");
			else
				d_set_text(d, NULL);
		}
		else if (mod == 0x0A)
		{
			static const char *const r[4] = { "+de]", "+a]", "+hl]", "+b]" };
			if (f < 4)
				d_set_num(d, K_INDEX, d_word(b, 2), 16, "[", r[f]);
			else
				d_set_text(d, NULL);
		}
		else
			d_set_text(d, NULL);
		break;
	}

	case OT_BYTE:
		d_set_num(d, K_IMM, d_field(o, b), 8, "#", "");
		break;
	case OT_WORD:
		d_set_num(d, K_IMM, d_word(b, o->byteOffset), 16, "#", "");
		break;
	case OT_NUM:
		d_set_num(d, K_COUNT, d_field(o, b), 8, "", "");
		break;

	case OT_SADDR:
	case OT_SADDRP:
		d_set_num(d, K_DATA, d_saddr(d_field(o, b)), 16, "", "");
		d->form = DF_SADDR;
		break;
	case OT_SADDR_FE:
	case OT_SADDRP_FE:
		d_set_num(d, K_DATA, d_saddr_fe(d_field(o, b)), 16, "", "");
		d->form = DF_SADDR;
		break;
	case OT_SADDRPP:
		d_set_num(d, K_DATA, d_saddr(d_field(o, b)), 16, "[", "]");
		d->form = DF_INDIR;
		break;
	case OT_SFR:
	case OT_SFRP:
		d_set_num(d, K_DATA, 0xFF00 | d_field(o, b), 16, "sfr[", "]");
		d->form = DF_SFR;
		break;
	case OT_ADDR16:
	{
		Mnemonic m = in->op->mnemonic;
		/* RETCS !addr16 also names code: where execution resumes */
		bool code = (m == CALL || m == BR || m == RETCS);
		d_set_num(d, code ? K_CODE : K_DATA, d_word(b, o->byteOffset), 16, "!", "");
		d->form = DF_ABS;
		d->absolute = true;
		break;
	}
	case OT_ADDR11:
		/* CALLF !addr11: 0800H + 11-bit field (3 bits in byte 0, 8 in byte 1) */
		d_set_num(d, K_CODE, 0x0800 + (((b[0] & 0x07) << 8) | b[1]), 16, "!", "");
		d->absolute = true;
		break;
	case OT_ADDR16R:
	{
		long t = (long)in->addr + in->op->bytes + (signed char)b[o->byteOffset];
		d_set_num(d, K_CODE, t, 16, "", "");
		break;
	}
	case OT_ADDR5:
		d_set_num(d, K_CALLT, b[o->byteOffset] & 0x1F, 16, "[", "]");
		break;

	case OT_RB_NUM:
	{
		char t[8];
		snprintf(t, sizeof(t), "rb%d", d_field(o, b));
		d_set_text(d, t);
		break;
	}
	case OT_RB_NUM_ALT:
	{
		char t[16];
		snprintf(t, sizeof(t), "rb%d, alt", d_field(o, b));
		d_set_text(d, t);
		break;
	}

	case OT_POST_BYTE:
	case OT_POST_BYTE_U:
	{
		/* PUSH/POP/PUSHU/POPU register list: bit N selects RPN, low bit
		 * first, RP5 = up (PUSH/POP) or psw (PUSHU/POPU); manual
		 * U12117EJ2V0UM00 p.94, Figure 5-1. */
		static const char *const r[8] = { "ax", "bc", "ax'", "bc'", "vp", "up", "de", "hl" };
		char t[40] = "{";
		int i, cnt = 0;
		f = d_field(o, b);
		for (i = 0; i < 8; i++)
		{
			if (!(f & (1 << i)))
				continue;
			if (cnt++)
				strcat(t, ", ");
			strcat(t, (o->opType == OT_POST_BYTE_U && i == 5) ? "psw" : r[i]);
		}
		strcat(t, "}");
		d_set_text(d, t);
		if (f == 0)
			d->invalid = true;   /* encodes, but has no source text */
		break;
	}

	case OT_N_BIT:
		d_set_num(d, K_BIT, d_field(o, b) & 7, 8, "", "");
		break;
	case OT_CY_BIT:
		d_set_num(d, K_CYBIT, d_field(o, b) & 7, 8, "cy.", "");
		break;

	default:
		d_set_text(d, NULL);
		break;
	}
}

/* Decodes the instruction at addr. Returns false if nothing in the table
 * matches, or the instruction would run off the end of loaded memory. */
static bool decode_insn(unsigned long addr, Insn *in)
{
	byte b0;
	const Opcode *sel = NULL;
	int modBytes = 0;
	size_t i;
	int j;
	const Operand *ops[3];

	memset(in, 0, sizeof(*in));
	if (!mem_read_byte(addr, &b0))
		return false;

	for (i = 0; i < OPPS_COUNT && !sel; i++)
	{
		const Opcode *op = &opps[i];

		if ((b0 & op->opMask) != op->op)
			continue;
		if (op->specialOnebyteInstr && (b0 & 0x07) >= 6)
			continue;

		if (op->subOpMask != 0)
		{
			byte b1;
			if (!mem_read_byte(addr + 1, &b1) || (b1 & op->subOpMask) != op->subOp)
				continue;
			if (op->bytes == 0)
			{
				byte mod = (byte)(b0 & 0x1F);
				if (mod == 0x16 || mod == 0x17)
					modBytes = 2;
				else if (mod == 0x06)
					modBytes = 3;
				else if (mod == 0x0A)
					modBytes = 4;
				else
					continue;
			}
		}
		sel = op;
	}
	if (!sel)
		return false;

	in->addr = addr;
	in->op = sel;
	in->len = sel->bytes + modBytes;
	if (in->len < 1 || in->len > 5)
		return false;
	for (j = 0; j < in->len; j++)
		if (!mem_read_byte(addr + (unsigned long)j, &in->b[j]))
			return false;

	ops[0] = &sel->operand1;
	ops[1] = &sel->operand2;
	ops[2] = &sel->operand3;
	for (j = 0; j < 3; j++)
	{
		if (ops[j]->opType == OT_NONE)
			continue;
		d_operand(in, ops[j], &in->ops[in->n]);
		in->n++;
	}
	return true;
}

static const char *insn_mnemonic(const Insn *in)
{
	static char buf[16];
	const char *s = mnemonic_names[(int)in->op->mnemonic];
	size_t k;
	for (k = 0; s[k] && k < sizeof(buf) - 1; k++)
		buf[k] = (char)((s[k] >= 'A' && s[k] <= 'Z') ? s[k] + 32 : s[k]);
	buf[k] = '\0';
	return buf;
}

static bool insn_has_invalid(const Insn *in)
{
	int i;
	for (i = 0; i < in->n; i++)
		if (in->ops[i].invalid)
			return true;
	return false;
}

/* Whether the companion vasm backend reproduces this instruction's bytes
 * from its source text. Mirrors the boundary of vasm's cpus/78k3:
 *  - memory forms: only [reg+disp] (mod 06H) is parsed; the long
 *    register-indirect (16H), register-indexed (17H) and base-indexed
 *    (0AH) forms are not;
 *  - SEL RBn,ALT is unreachable (shadowed by SEL RBn);
 *  - MOV/XCH r,r1 with r = A (24H/25H prefix) prints like the shorter
 *    MOV/XCH A,r1, which the assembler picks;
 *  - an odd word address in saddrp (the assembler requires even ones);
 *  - [saddrp] above FF1EH (word pointer would leave the window);
 *  - MOV WDM/STBC with a guard byte that isn't the complement;
 *  - a relative branch whose target wraps around the 64K space;
 *  - any field with no defined meaning. */
static bool insn_asm_ok(const Insn *in)
{
	const Opcode *op = in->op;
	const Operand *slot[3];
	int i;

	if (insn_has_invalid(in))
		return false;

	slot[0] = &op->operand1;
	slot[1] = &op->operand2;
	slot[2] = &op->operand3;

	if ((op->mnemonic == MOV || op->mnemonic == XCH) &&
		slot[0]->opType == OT_R && slot[1]->opType == OT_R1 &&
		d_field(slot[0], in->b) == 1)
		return false;

	for (i = 0; i < 3; i++)
	{
		int t = slot[i]->opType;
		if (t == OT_UNK || t == OT_RB_NUM_ALT)
			return false;
		/* MOV's register-indirect [de+]...[hl] forms also have a 1-byte
		 * encoding, which the assembler picks for the same text */
		if (t == OT_MEM && slot[i]->isMemMod && op->mnemonic == MOV &&
			(in->b[0] & 0x1F) == 0x16 && d_field(slot[i], in->b) < 6)
			return false;
		if (t == OT_SADDRP && (d_saddr(d_field(slot[i], in->b)) & 1))
			return false;
		if (t == OT_SADDRP_FE && (d_saddr_fe(d_field(slot[i], in->b)) & 1))
			return false;
		if (t == OT_SADDRPP && d_saddr(d_field(slot[i], in->b)) > 0xFF1E)
			return false;
		if ((t == OT_WDM || t == OT_STBC) && in->len == 4 && in->b[2] != (byte)~in->b[3])
			return false;
		if (t == OT_ADDR16R)
		{
			long tg = (long)in->addr + op->bytes + (signed char)in->b[slot[i]->byteOffset];
			if (tg < 0 || tg > 0xFFFF)
				return false;
		}
	}
	return true;
}

/* Whether a 16-bit immediate in this instruction is plausibly an
 * address: only "movw rp, #word", the way pointers are loaded.
 * Arithmetic, compares and values written to SFRs or memory are
 * numbers. */
static bool insn_imm_is_pointer(const Insn *in)
{
	int t = in->op->operand1.opType;
	return in->op->mnemonic == MOVW &&
		(t == OT_RP || t == OT_RP1 || t == OT_RP2 || t == OT_AX);
}

/* Ends the flow of execution (a walk stops after it). */
static bool insn_is_return(const Insn *in)
{
	Mnemonic m = in->op->mnemonic;
	return m == RET || m == RETI || m == RETCS || m == BRK;
}

static bool insn_is_call(const Insn *in)
{
	Mnemonic m = in->op->mnemonic;
	return m == CALL || m == CALLF;
}

static bool insn_is_jump(const Insn *in)
{
	return in->op->mnemonic == BR;
}

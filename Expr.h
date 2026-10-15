/*
 * SPDX-License-Identifier: GPL-3.0-only
 * NEC 78K/III Disassembler 1.0
 * Copyright (c) 2026 Dave, KY4WI
 * Project includes upstream MIT-licensed material (see LICENSE.MIT).
 */

#pragma once

/*
 * Expression evaluator for operands and .equ values.
 *
 * C precedence, the same as vasm's: unary - ~ !, then * / %, + -, << >>,
 * &, ^, |. Numbers: 0x hex, 0b binary, decimal, 'c' character constants.
 * Names are looked up through a callback. Used by the database loader,
 * the extractor and the per-operand bindings, so a value means the same
 * thing everywhere.
 */

#include "Util.h"

typedef bool (*ExprLookup)(const char *name, long long *value, void *ctx);

typedef struct ExprState
{
	const char *p;
	ExprLookup lookup;
	void *ctx;
	char err[200];
	char missing[64];   /* first undefined name, if that was the error */
} ExprState;

static long long ex_or(ExprState *s);

static void ex_fail(ExprState *s, const char *msg)
{
	if (!s->err[0])
		snprintf(s->err, sizeof(s->err), "%s", msg);
}

static long long ex_primary(ExprState *s)
{
	const char *p = skipws(s->p);
	long long v = 0;

	if (*p == '(')
	{
		s->p = p + 1;
		v = ex_or(s);
		p = skipws(s->p);
		if (*p != ')')
		{
			ex_fail(s, "missing ')'");
			return 0;
		}
		s->p = p + 1;
		return v;
	}
	if (*p == '\'')
	{
		if (p[1] && p[1] != '\\' && p[2] == '\'')
		{
			s->p = p + 3;
			return (unsigned char)p[1];
		}
		ex_fail(s, "bad character constant");
		return 0;
	}
	if (isdigit((unsigned char)*p))
	{
		char *e;
		if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		{
			if (!isxdigit((unsigned char)p[2]))
			{
				ex_fail(s, "bad hex number");
				return 0;
			}
			v = (long long)strtoull(p + 2, &e, 16);
		}
		else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && (p[2] == '0' || p[2] == '1'))
			v = (long long)strtoull(p + 2, &e, 2);
		else
			v = (long long)strtoull(p, &e, 10);
		if (is_ident_char((unsigned char)*e))
		{
			ex_fail(s, "bad number");
			return 0;
		}
		s->p = e;
		return v;
	}
	if (is_ident_start((unsigned char)*p))
	{
		const char *q = p;
		char name[64];
		while (is_ident_char((unsigned char)*q))
			q++;
		if ((size_t)(q - p) >= sizeof(name))
		{
			ex_fail(s, "name too long");
			return 0;
		}
		memcpy(name, p, (size_t)(q - p));
		name[q - p] = '\0';
		s->p = q;
		if (!s->lookup || !s->lookup(name, &v, s->ctx))
		{
			if (!s->err[0])
			{
				snprintf(s->missing, sizeof(s->missing), "%s", name);
				snprintf(s->err, sizeof(s->err), "'%s' is not defined", name);
			}
			return 0;
		}
		return v;
	}
	ex_fail(s, *p ? "unexpected character in expression" : "expression expected");
	return 0;
}

static long long ex_unary(ExprState *s)
{
	const char *p = skipws(s->p);
	if (*p == '-') { s->p = p + 1; return -ex_unary(s); }
	if (*p == '+') { s->p = p + 1; return ex_unary(s); }
	if (*p == '~') { s->p = p + 1; return ~ex_unary(s); }
	if (*p == '!') { s->p = p + 1; return !ex_unary(s); }
	return ex_primary(s);
}

static long long ex_mul(ExprState *s)
{
	long long v = ex_unary(s);
	for (;;)
	{
		const char *p = skipws(s->p);
		char c = *p;
		long long r;
		if (c != '*' && c != '/' && c != '%')
			return v;
		s->p = p + 1;
		r = ex_unary(s);
		if (c == '*')
			v *= r;
		else if (r == 0)
		{
			ex_fail(s, "division by zero");
			return 0;
		}
		else if (c == '/')
			v /= r;
		else
			v %= r;
	}
}

static long long ex_add(ExprState *s)
{
	long long v = ex_mul(s);
	for (;;)
	{
		const char *p = skipws(s->p);
		if (*p == '+') { s->p = p + 1; v += ex_mul(s); }
		else if (*p == '-') { s->p = p + 1; v -= ex_mul(s); }
		else return v;
	}
}

static long long ex_shift(ExprState *s)
{
	long long v = ex_add(s);
	for (;;)
	{
		const char *p = skipws(s->p);
		if (p[0] == '<' && p[1] == '<') { s->p = p + 2; v <<= ex_add(s); }
		else if (p[0] == '>' && p[1] == '>') { s->p = p + 2; v >>= ex_add(s); }
		else return v;
	}
}

static long long ex_and(ExprState *s)
{
	long long v = ex_shift(s);
	for (;;)
	{
		const char *p = skipws(s->p);
		if (*p == '&' && p[1] != '&') { s->p = p + 1; v &= ex_shift(s); }
		else return v;
	}
}

static long long ex_xor(ExprState *s)
{
	long long v = ex_and(s);
	for (;;)
	{
		const char *p = skipws(s->p);
		if (*p == '^') { s->p = p + 1; v ^= ex_and(s); }
		else return v;
	}
}

static long long ex_or(ExprState *s)
{
	long long v = ex_xor(s);
	for (;;)
	{
		const char *p = skipws(s->p);
		if (*p == '|' && p[1] != '|') { s->p = p + 1; v |= ex_xor(s); }
		else return v;
	}
}

/* Evaluates the whole of text. Returns false with a message in err
 * (and the undefined name in missing, if that was the problem). */
static bool expr_eval(const char *text, ExprLookup lookup, void *ctx, long long *value,
	char *err, size_t errsz, char *missing, size_t missingsz)
{
	ExprState s;
	long long v;
	memset(&s, 0, sizeof(s));
	s.p = text;
	s.lookup = lookup;
	s.ctx = ctx;
	v = ex_or(&s);
	if (!s.err[0] && *skipws(s.p))
		ex_fail(&s, "unexpected text after expression");
	if (s.err[0])
	{
		if (err)
			snprintf(err, errsz, "%s", s.err);
		if (missing)
			snprintf(missing, missingsz, "%s", s.missing);
		return false;
	}
	*value = v;
	return true;
}

/* Whether text is a single name (so an .equ of it is an alias). */
static bool expr_is_name(const char *text)
{
	const char *p = skipws(text);
	if (!is_ident_start((unsigned char)*p))
		return false;
	while (is_ident_char((unsigned char)*p))
		p++;
	return *skipws(p) == '\0';
}

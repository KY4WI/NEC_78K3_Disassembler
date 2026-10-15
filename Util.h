/*
 * SPDX-License-Identifier: GPL-3.0-only
 * NEC 78K/III Disassembler 1.0
 * Copyright (c) 2026 Dave, KY4WI
 * Project includes upstream MIT-licensed material (see LICENSE.MIT).
 */

#pragma once

/* Small helpers shared by every part of the program. */

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q)
	{
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	return q;
}

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = (char *)xrealloc(NULL, n);
	memcpy(d, s, n);
	return d;
}

static char *xstrndup(const char *s, size_t n)
{
	char *d = (char *)xrealloc(NULL, n + 1);
	memcpy(d, s, n);
	d[n] = '\0';
	return d;
}

/* printf into a new heap string */
static char *xasprintf(const char *fmt, ...)
{
	va_list ap;
	int n;
	char *s;
	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	s = (char *)xrealloc(NULL, (size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf(s, (size_t)n + 1, fmt, ap);
	va_end(ap);
	return s;
}

/* Grows an array so index `count` is valid. */
#define GROW(arr, count, cap) do { \
		if ((count) >= (cap)) { \
			(cap) = (cap) ? (cap) * 2 : 64; \
			(arr) = xrealloc((arr), (size_t)(cap) * sizeof(*(arr))); \
		} \
	} while (0)

/* Growable string buffer. */
typedef struct Str
{
	char *s;
	size_t len, cap;
} Str;

static void str_addn(Str *b, const char *p, size_t n)
{
	if (b->len + n + 1 > b->cap)
	{
		size_t c = b->cap ? b->cap : 128;
		while (c < b->len + n + 1)
			c *= 2;
		b->s = (char *)xrealloc(b->s, c);
		b->cap = c;
	}
	memcpy(b->s + b->len, p, n);
	b->len += n;
	b->s[b->len] = '\0';
}

static void str_add(Str *b, const char *p)
{
	str_addn(b, p, strlen(p));
}

static void str_addf(Str *b, const char *fmt, ...)
{
	va_list ap;
	int n;
	char small[256];
	va_start(ap, fmt);
	n = vsnprintf(small, sizeof(small), fmt, ap);
	va_end(ap);
	if (n < (int)sizeof(small))
	{
		str_addn(b, small, (size_t)n);
		return;
	}
	{
		char *big = (char *)xrealloc(NULL, (size_t)n + 1);
		va_start(ap, fmt);
		vsnprintf(big, (size_t)n + 1, fmt, ap);
		va_end(ap);
		str_addn(b, big, (size_t)n);
		free(big);
	}
}

static void str_clear(Str *b)
{
	b->len = 0;
	if (b->s)
		b->s[0] = '\0';
}

static const char *str_c(const Str *b)
{
	return b->s ? b->s : "";
}

/* Pads with spaces to column col (or one space if already there). */
static void str_pad(Str *b, size_t col)
{
	if (b->len >= col)
	{
		str_add(b, " ");
		return;
	}
	while (b->len < col)
		str_addn(b, " ", 1);
}

static void rstrip(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
}

static const char *skipws(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

static bool is_ident_start(int c)
{
	return isalpha(c) || c == '_' || c == '.';
}

static bool is_ident_char(int c)
{
	return isalnum(c) || c == '_' || c == '.';
}

/* Reads a whole file into memory; returns NULL if it can't be opened. */
static char *read_file(const char *path, size_t *len)
{
	FILE *fp = fopen(path, "rb");
	char *buf;
	long n;
	if (!fp)
		return NULL;
	fseek(fp, 0, SEEK_END);
	n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	buf = (char *)xrealloc(NULL, (size_t)n + 1);
	if (n > 0 && fread(buf, 1, (size_t)n, fp) != (size_t)n)
	{
		fclose(fp);
		free(buf);
		return NULL;
	}
	fclose(fp);
	buf[n] = '\0';
	if (len)
		*len = (size_t)n;
	return buf;
}

/* Splits text into lines (in place); returns the count. A final line
 * without a newline counts; a trailing newline doesn't add an empty one.
 * '\r' before '\n' is removed. */
static int split_lines(char *text, char ***out)
{
	char **v = NULL;
	int n = 0, cap = 0;
	char *p = text;
	while (*p)
	{
		char *e = strchr(p, '\n');
		GROW(v, n, cap);
		v[n++] = p;
		if (!e)
			break;
		if (e > p && e[-1] == '\r')
			e[-1] = '\0';
		*e = '\0';
		p = e + 1;
	}
	*out = v;
	return n;
}

static unsigned long crc32_buf(const unsigned char *p, unsigned long n)
{
	unsigned long crc = 0xFFFFFFFFUL;
	unsigned long i;
	int k;
	for (i = 0; i < n; i++)
	{
		crc ^= p[i];
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
	}
	return crc ^ 0xFFFFFFFFUL;
}

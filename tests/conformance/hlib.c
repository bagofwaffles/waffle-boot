/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Support for the conformance harness's host programs (hlib.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hlib.h"

static char line[H_MAXLINE];
static int bad;
static int first = 1;

int h_next(struct h_cmd *c)
{
	char *p;

	for (;;) {
		if (!fgets(line, sizeof line, stdin))
			return 0;
		c->n = 0;
		for (p = strtok(line, " \t\r\n"); p && c->n < H_MAXTOK;
		     p = strtok(NULL, " \t\r\n"))
			c->tok[c->n++] = p;
		if (c->n > 0)
			return 1;
	}
}

const char *h_str(const struct h_cmd *c, int i)
{
	return i < c->n ? c->tok[i] : NULL;
}

unsigned long h_num(const struct h_cmd *c, int i)
{
	const char *s = h_str(c, i);
	char *end;
	unsigned long v;

	if (!s) {
		h_error(c, "missing number");
		return 0;
	}
	v = strtoul(s, &end, 0);
	if (*end != '\0')
		h_error(c, "bad number");
	return v;
}

static int nibble(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

size_t h_hex(const struct h_cmd *c, int i, uint8_t *out, size_t max)
{
	const char *s = h_str(c, i);
	size_t n = 0;

	if (!s) {
		h_error(c, "missing bytes");
		return 0;
	}
	if (strcmp(s, "-") == 0)
		return 0;
	while (s[0] && s[1]) {
		int hi = nibble(s[0]);
		int lo = nibble(s[1]);

		if (hi < 0 || lo < 0 || n == max) {
			h_error(c, "bad bytes");
			return n;
		}
		out[n++] = (uint8_t)(hi << 4 | lo);
		s += 2;
	}
	if (s[0])
		h_error(c, "odd hex length");
	return n;
}

static void sep(void)
{
	if (!first)
		putchar(' ');
	first = 0;
}

void h_put(const char *s)
{
	sep();
	fputs(s, stdout);
}

void h_putu(unsigned long v)
{
	sep();
	printf("%lu", v);
}

void h_putx(unsigned long v)
{
	sep();
	printf("0x%lx", v);
}

void h_puthex(const uint8_t *p, size_t n)
{
	size_t i;

	sep();
	if (n == 0)
		putchar('-');
	for (i = 0; i < n; i++)
		printf("%02x", p[i]);
}

void h_end(void)
{
	putchar('\n');
	first = 1;
}

void h_error(const struct h_cmd *c, const char *why)
{
	if (!first)
		h_end();
	printf("error %s: %s\n", why, c->n > 0 ? c->tok[0] : "");
	bad = 1;
}

int h_status(void)
{
	fflush(stdout);
	return bad;
}

uint32_t h_crc32(uint32_t crc, const uint8_t *buf, uint32_t len)
{
	static uint32_t table[256];
	uint32_t i;

	if (table[1] == 0u) {
		for (i = 0; i < 256u; i++) {
			uint32_t c = i, k;

			for (k = 0; k < 8u; k++)
				c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	}
	crc = ~crc;
	for (i = 0; i < len; i++)
		crc = table[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
	return ~crc;
}

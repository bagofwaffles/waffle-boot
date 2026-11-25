/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-14 log line: building a line, the tag grammar, and the text builder.
 */
#include "waffle/log/log.h"

static bool name_char(uint8_t c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
	       c == '_' || c == '-';
}

/* S-14.004 */
bool s14_tag_name_valid(const char *name)
{
	size_t n = 0;

	if (name == NULL)
		return false;
	while (name[n] != '\0') {
		if (n == S14_NAME_MAX || !name_char((uint8_t)name[n]))
			return false;
		n++;
	}
	return true;
}

/* Decimal digits of v, most significant first; returns the count (1..10). */
static size_t dec_digits(uint8_t out[10], uint32_t v)
{
	uint8_t tmp[10];
	size_t n = 0, i;

	do {
		tmp[n++] = (uint8_t)('0' + v % 10u);
		v /= 10u;
	} while (v != 0u);
	for (i = 0; i < n; i++)
		out[i] = tmp[n - 1u - i];
	return n;
}

/*
 * Writes bytes into the line while counting the full line's length: only
 * the first `keep` bytes are stored.
 */
struct sink {
	uint8_t *out;
	size_t keep;
	size_t full;
};

static void put(struct sink *s, uint8_t c)
{
	if (s->full < s->keep)
		s->out[s->full] = c;
	s->full++;
}

/* S-14.001, S-14.002, S-14.004, S-14.006, S-14.007, S-14.008 */
size_t s14_line_build(uint8_t out[S14_LINE_MAX], uint32_t timestamp,
		      struct s14_tag tag, const uint8_t *text, size_t text_len)
{
	struct sink s = { out, S14_CUT_KEEP, 0 };
	size_t tlen = 0, i;
	bool tagged = s14_tag_name_valid(tag.name);

	/* S-14.008: a 0x00 ends the text; the body never holds one (S-14.001). */
	if (text != NULL)
		while (tlen < text_len && text[tlen] != 0u)
			tlen++;

	/* S-14.002: u32 big-endian microseconds, mod 2^32. */
	put(&s, (uint8_t)(timestamp >> 24));
	put(&s, (uint8_t)(timestamp >> 16));
	put(&s, (uint8_t)(timestamp >> 8));
	put(&s, (uint8_t)timestamp);

	/* S-14.004, S-14.005: name "[" [line] "]: " */
	if (tagged) {
		for (i = 0; tag.name[i] != '\0'; i++)
			put(&s, (uint8_t)tag.name[i]);
		put(&s, '[');
		if (tag.line_no >= 1u && tag.line_no <= S14_LINE_NO_MAX) {
			uint8_t d[10];
			size_t n = dec_digits(d, tag.line_no);

			for (i = 0; i < n; i++)
				put(&s, d[i]);
		}
		put(&s, ']');
		put(&s, ':');
		put(&s, ' ');
	}

	for (i = 0; i < tlen; i++)
		put(&s, text[i]);

	/* S-14.006: a full line of 249 bytes or more (its final LF included)
	 * is cut to 246 bytes and "..". */
	if (s.full > S14_LINE_MAX) {
		out[S14_CUT_KEEP] = '.';
		out[S14_CUT_KEEP + 1u] = '.';
		return S14_LINE_MAX;
	}
	/* The whole line fits in the kept part except for its last 2 bytes;
	 * store them now. */
	for (i = S14_CUT_KEEP; i < s.full; i++)
		out[i] = text[tlen - (s.full - i)];

	/* S-14.007: one LF at the end of the text itself is removed. */
	if (tlen > 0u && text[tlen - 1u] == '\n')
		return s.full - 1u;
	return s.full;
}

/* S-14.004: the tag grammar, read from the start of a body. */
size_t s14_tag_len(const uint8_t *body, size_t len)
{
	size_t i = 0, digits = 0;
	uint32_t num = 0;

	while (i < len && i < S14_NAME_MAX && name_char(body[i]))
		i++;
	if (i >= len || body[i] != '[')
		return 0;
	i++;
	while (i < len && body[i] >= '0' && body[i] <= '9') {
		uint32_t d = (uint32_t)(body[i] - '0');

		if (digits == 0u && d == 0u)
			return 0;        /* leading zero, or line 0 */
		if (num > (S14_LINE_NO_MAX - d) / 10u)
			return 0;        /* above 2147483647 */
		num = num * 10u + d;
		digits++;
		i++;
	}
	if (i + 3u > len || body[i] != ']' || body[i + 1u] != ':' ||
	    body[i + 2u] != ' ')
		return 0;
	return i + 3u;
}

void s14_text_init(struct s14_text *t)
{
	t->len = 0;
	t->overflow = false;
}

void s14_text_bytes(struct s14_text *t, const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (t->len == S14_TEXT_CAP) {
			t->overflow = true;
			return;
		}
		t->buf[t->len++] = p[i];
	}
}

void s14_text_str(struct s14_text *t, const char *s)
{
	size_t n = 0;

	while (s[n] != '\0')
		n++;
	s14_text_bytes(t, (const uint8_t *)s, n);
}

void s14_text_dec(struct s14_text *t, uint32_t v)
{
	uint8_t d[10];
	size_t n = dec_digits(d, v);

	s14_text_bytes(t, d, n);
}

void s14_text_hex(struct s14_text *t, uint32_t v, unsigned min_digits)
{
	static const char hex[] = "0123456789abcdef";
	uint8_t d[8] = { 0 };
	unsigned n = 8;

	while (n > 1u && n > min_digits && (v >> ((n - 1u) * 4u)) == 0u)
		n--;
	for (unsigned i = 0; i < n; i++)
		d[i] = (uint8_t)hex[(v >> ((n - 1u - i) * 4u)) & 0xfu];
	s14_text_bytes(t, d, n);
}

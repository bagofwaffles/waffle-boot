/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-14 line format against tests/oracle/S-14-logging/vectors/lines.json.
 * Requirements: S-14.001, S-14.002, S-14.004, S-14.005, S-14.006, S-14.007,
 * S-14.008.
 */
#include <stdint.h>

#include "waffle/s14_log.h"
#include "s14_check.h"
#define S14V_LINES
#include "vectors/s14_vectors.h"

/* S-14.001: 4..248 bytes, no 0x00 in the body. */
static int line_ok(const uint8_t *l, size_t n)
{
	size_t i;

	if (n < 4u || n > S14_LINE_MAX)
		return 0;
	for (i = 4; i < n; i++)
		if (l[i] == 0u)
			return 0;
	return 1;
}

static void test_vectors(void)
{
	size_t i;

	for (i = 0; i < S14V_N(s14v_lines); i++) {
		const struct s14v_line *v = &s14v_lines[i];
		uint8_t out[S14_LINE_MAX];
		struct s14_tag tag = { v->name, v->line_no };
		size_t n = s14_line_build(out, v->timestamp, tag,
					  (const uint8_t *)v->text,
					  v->text_len);

		CHECK(mem_eq(out, n, v->line, v->line_len),
		      "S-14.001/006/007 line: %s (got %zu bytes, want %zu)",
		      v->note, n, v->line_len);
		CHECK(line_ok(out, n), "S-14.001 structure: %s", v->note);
		/* S-14.002: the first four bytes are the stamp, big-endian. */
		CHECK(((uint32_t)out[0] << 24 | (uint32_t)out[1] << 16 |
		       (uint32_t)out[2] << 8 | out[3]) == v->timestamp,
		      "S-14.002 timestamp: %s", v->note);
		/* A built tag parses back by the grammar (S-14.004). */
		if (v->name != NULL)
			CHECK(s14_tag_len(out + 4, n - 4u) > 0u,
			      "S-14.004 tag: %s", v->note);
	}
}

static void test_tag_grammar(void)
{
	size_t i;

	for (i = 0; i < S14V_N(s14v_tags); i++) {
		const struct s14v_tag *v = &s14v_tags[i];
		size_t n = s14_tag_len((const uint8_t *)v->body, v->body_len);

		CHECK(n == v->tag_len, "S-14.004 tag %zu: got %zu, want %zu",
		      i, n, v->tag_len);
	}
}

/* S-14.004, S-14.005: names outside the grammar give no tag; line 0 and
 * numbers above 2^31 - 1 give an empty number. */
static void test_tag_build(void)
{
	static const uint8_t t[] = "x";
	uint8_t out[S14_LINE_MAX];
	size_t n;

	n = s14_line_build(out, 0, (struct s14_tag){ "Bad", 5 }, t, 1);
	CHECK(mem_eq(out + 4, n - 4u, "x", 1), "upper-case name: no tag");
	n = s14_line_build(out, 0, (struct s14_tag){ "abcdefghijklmnopq", 5 },
			   t, 1);
	CHECK(mem_eq(out + 4, n - 4u, "x", 1), "17-byte name: no tag");
	n = s14_line_build(out, 0, (struct s14_tag){ "a", 0 }, t, 1);
	CHECK(mem_eq(out + 4, n - 4u, "a[]: x", 6), "line 0: empty number");
	n = s14_line_build(out, 0, (struct s14_tag){ "a", 2147483648u }, t, 1);
	CHECK(mem_eq(out + 4, n - 4u, "a[]: x", 6), "line 2^31: empty number");
	n = s14_line_build(out, 0, S14_NO_TAG, t, 1);
	CHECK(mem_eq(out + 4, n - 4u, "x", 1), "no tag");
	CHECK(s14_tag_name_valid(""), "empty name is valid");
	CHECK(!s14_tag_name_valid(NULL), "NULL name: no tag");
}

/* S-14.008: a 0x00 in the text ends it; the body never holds one. */
static void test_nul_ends_text(void)
{
	static const uint8_t t[] = { 'a', 'b', 0, 'c', '\n' };
	uint8_t out[S14_LINE_MAX];
	size_t n = s14_line_build(out, 1, S14_NO_TAG, t, sizeof t);

	CHECK(mem_eq(out + 4, n - 4u, "ab", 2), "S-14.008 0x00 ends the text");
	n = s14_line_build(out, 1, S14_NO_TAG, NULL, 5);
	CHECK(n == 4u, "NULL text: empty");
}

/* Every full length around the limit, every byte position of the cut. */
static void test_lengths(void)
{
	uint8_t text[400], out[S14_LINE_MAX];
	size_t len, i;

	for (i = 0; i < sizeof text; i++)
		text[i] = (uint8_t)('a' + i % 26u);
	for (len = 0; len <= 300; len++) {
		size_t full = 4 + 6 + len;   /* "t[1]: " */
		size_t n = s14_line_build(out, 7, (struct s14_tag){ "t", 1 },
					  text, len);

		if (full >= 249u) {
			CHECK(n == 248u && out[246] == '.' && out[247] == '.',
			      "S-14.006 cut at full length %zu", full);
			CHECK(memcmp(out + 10, text, 236) == 0,
			      "S-14.006 kept bytes at %zu", full);
		} else {
			CHECK(n == full, "S-14.006 whole at %zu", full);
			CHECK(memcmp(out + 10, text, len) == 0,
			      "text at %zu", full);
		}
	}
}

/* The text builder: no format is interpreted (S-14.008, S-16.099). */
static void test_text(void)
{
	struct s14_text t;
	uint8_t big[300];
	size_t i;

	s14_text_init(&t);
	s14_text_str(&t, "%s%n ");
	s14_text_dec(&t, 0);
	s14_text_str(&t, " ");
	s14_text_dec(&t, 4294967295u);
	s14_text_str(&t, " ");
	s14_text_hex(&t, 0x5a, 2);
	s14_text_str(&t, " ");
	s14_text_hex(&t, 0x7, 2);
	s14_text_str(&t, " ");
	s14_text_hex(&t, 0xdeadbeefu, 1);
	CHECK(mem_eq(t.buf, t.len, "%s%n 0 4294967295 5a 07 deadbeef", 32),
	      "text builder: %.*s", (int)t.len, (const char *)t.buf);

	for (i = 0; i < sizeof big; i++)
		big[i] = 'z';
	s14_text_init(&t);
	s14_text_bytes(&t, big, sizeof big);
	CHECK(t.len == S14_TEXT_CAP && t.overflow, "overflow kept and flagged");
}

int main(void)
{
	test_vectors();
	test_tag_grammar();
	test_tag_build();
	test_nul_ends_text();
	test_lengths();
	test_text();
	return check_done("test_s14_line");
}

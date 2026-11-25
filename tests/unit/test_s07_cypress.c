/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07.081-S-07.083: packets under both checksum variants, the enter packet,
 * probe verdicts, the exit packet, and programming runs replayed against the
 * conformance bootloader model's recorded replies (cypress.json and the
 * failure cases of test_s07.py Cypress.test_run_failures).
 */
#include "waffle/psoc/cypress.h"

#include "s06_test.h"
#include "vectors/s07_vectors.h"

static void test_packets(void)
{
	uint8_t out[600];
	size_t n;

	for (size_t i = 0; i < s07v_packets_count; i++) {
		const struct s07v_packet *v = &s07v_packets[i];

		t_case = "cypress.json packets";
		n = s07_cy_packet(v->code, v->data, v->len, S07_CY_SUM,
				  t_crc16_x25, out);
		CHECK_MEM(out, n, v->sum, v->sum_len);
		n = s07_cy_packet(v->code, v->data, v->len, S07_CY_CRC,
				  t_crc16_x25, out);
		CHECK_MEM(out, n, v->crc, v->crc_len);
	}
	t_case = "cypress.json checksum";
	for (size_t i = 0; i < s07v_checksum_count; i++) {
		const struct s07v_checksum *v = &s07v_checksum[i];

		CHECK_EQ(s07_cy_checksum(v->d, v->len, S07_CY_SUM, t_crc16_x25), v->sum);
		CHECK_EQ(s07_cy_checksum(v->d, v->len, S07_CY_CRC, t_crc16_x25), v->crc);
	}
	t_case = "enter";
	n = s07_cy_packet(S07_CY_ENTER, s07v_enter[0].unlock, 6, S07_CY_SUM,
			  t_crc16_x25, out);
	CHECK_MEM(out, n, s07v_enter[0].packet, s07v_enter[0].len);
	t_case = "exit";
	n = s07_cy_packet(S07_CY_EXIT, 0, 0, S07_CY_SUM, t_crc16_x25, out);
	CHECK_MEM(out, n, s07_cy_exit_packet, 7);
}

/* S-07.081, S-16.042 */
static void test_probe(void)
{
	t_case = "cypress.json probe";
	for (size_t i = 0; i < s07v_probe_count; i++) {
		const struct s07v_probe *v = &s07v_probe[i];

		if (v->ok >= 0)
			CHECK_EQ(s07_cy_probe_ok(v->reply, v->len), v->ok);
	}
}

struct replay {
	const struct s07v_xchg *x;
	size_t n, at;
	int mismatch;
};

static void rp_send(void *ctx, const uint8_t *p, size_t n)
{
	struct replay *r = ctx;

	if (r->at >= r->n) {
		r->mismatch++;
		return;
	}
	if (n != r->x[r->at].len || memcmp(p, r->x[r->at].p, n) != 0) {
		r->mismatch++;
		t_hex("  sent    ", p, n);
		t_hex("  expected", r->x[r->at].p, r->x[r->at].len);
	}
	r->at++;
}

static bool rp_recv(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_us)
{
	struct replay *r = ctx;
	const struct s07v_xchg *x;

	(void)timeout_us;
	if (r->at == 0)
		return false;
	x = &r->x[r->at - 1];
	if (!x->has_reply || x->rlen != n)
		return false;
	memcpy(buf, x->r, n);
	return true;
}

/* S-07.083: the published run's packets; each failure stops at its command */
static void test_runs(void)
{
	for (size_t i = 0; i < s07v_runs_count; i++) {
		const struct s07v_run *v = &s07v_runs[i];
		struct replay r = { s07v_xchg + v->first, v->n, 0, 0 };
		struct s07_cy_io io = { rp_send, rp_recv, &r };
		struct s07_cy_result res;
		struct s07_cyacd f;

		t_case = v->name;
		CHECK(s07_cyacd_open(&f, (const char *)v->cyacd, v->cyacd_len));
		CHECK_EQ(f.checksum_type, v->crc);
		s07_cy_run(&io, &f, v->unlock, t_crc16_x25, &res);
		CHECK_EQ(r.mismatch, 0);
		CHECK_EQ(r.at, r.n);
		CHECK_EQ(res.ok, v->fail_at == 0);
		CHECK_EQ(res.at, v->fail_at);
	}
}

/* S-07.083: the .cyacd reader */
static void test_cyacd(void)
{
	static const char good[] = "2E1230690000\r\n:0000050003AABBCCF0\r\n";
	static const char bad_header[] = "2E12306900\r\n";
	static const char bad_line[] = "2E1230690000\r\n:00000500";
	struct s07_cyacd f;
	struct s07_cyacd_row row;
	size_t pos;

	t_case = "cyacd";
	CHECK(s07_cyacd_open(&f, good, sizeof good - 1));
	CHECK_EQ(f.silicon_id, 0x2E123069);
	CHECK_EQ(f.silicon_rev, 0);
	pos = f.rows_at;
	CHECK_EQ(s07_cyacd_next(&f, &pos, &row), 1);
	CHECK_EQ(row.array, 0);
	CHECK_EQ(row.row, 5);
	CHECK_EQ(row.len, 3);
	CHECK_EQ(row.data[2], 0xCC);
	CHECK_EQ(row.check, 0xF0);
	CHECK_EQ(s07_cy_row_expected(&row), (0xF0 + 5 + 3) & 0xFF);
	CHECK_EQ(s07_cyacd_next(&f, &pos, &row), 0);
	CHECK(!s07_cyacd_open(&f, bad_header, sizeof bad_header - 1));
	CHECK(s07_cyacd_open(&f, bad_line, sizeof bad_line - 1));
	pos = f.rows_at;
	CHECK_EQ(s07_cyacd_next(&f, &pos, &row), -1);
}

/* A file of one 1-byte row in each of arrays 0..n-1, then tail. */
static size_t arrays_file(char *out, size_t max, unsigned n, const char *tail)
{
	int k = snprintf(out, max, "2E1230690000\r\n");

	for (unsigned a = 0; a < n; a++)
		k += snprintf(out + k, max - (size_t)k, ":%02X000000010000\r\n", a);
	k += snprintf(out + k, max - (size_t)k, "%s", tail);
	return (size_t)k;
}

/* The whole file read before any packet (S-07.083, s07_cyacd_check) */
static void test_check(void)
{
	static const char good[] = "2E1230690000\r\n:0000050003AABBCCF0\r\n";
	static const char bad_cc[] = "2E1230690000\r\n:0000050003AABBCC00\r\n";
	static const char header_only[] = "2E1230690000\r\n";
	static const char mid_bad[] =
		"2E1230690000\r\n:0000050003AABBCCF0\r\n:000006GG\r\n"
		":0000070003AABBCCEE\r\n";
	static const char short_line[] =
		"2E1230690000\r\n:0000050004AABBCCF0\r\n";
	char text[512];
	struct s07_cyacd f;
	size_t len;

	t_case = "check";
	CHECK(s07_cyacd_open(&f, good, sizeof good - 1));
	CHECK(s07_cyacd_check(&f));
	/* a wrong line check is the verify row's to find (S-07.083) */
	CHECK(s07_cyacd_open(&f, bad_cc, sizeof bad_cc - 1));
	CHECK(s07_cyacd_check(&f));
	CHECK(s07_cyacd_open(&f, header_only, sizeof header_only - 1));
	CHECK(!s07_cyacd_check(&f));
	CHECK(s07_cyacd_open(&f, mid_bad, sizeof mid_bad - 1));
	CHECK(!s07_cyacd_check(&f));
	CHECK(s07_cyacd_open(&f, short_line, sizeof short_line - 1));
	CHECK(!s07_cyacd_check(&f));
	len = arrays_file(text, sizeof text, 8, "");
	CHECK(s07_cyacd_open(&f, text, len));
	CHECK(s07_cyacd_check(&f));
	len = arrays_file(text, sizeof text, 9, "");
	CHECK(s07_cyacd_open(&f, text, len));
	CHECK(!s07_cyacd_check(&f));
	len = arrays_file(text, sizeof text, 2, ":");
	CHECK(s07_cyacd_open(&f, text, len));
	CHECK(!s07_cyacd_check(&f));
}

/* S-07.083: the published files, bad or not ('either': not judged) */
static void test_images(void)
{
	for (size_t i = 0; i < s07v_images_count; i++) {
		const struct s07v_image *v = &s07v_images[i];
		struct s07_cyacd f;
		bool good;

		t_case = v->why;
		good = s07_cyacd_open(&f, (const char *)v->cyacd, v->len) &&
		       s07_cyacd_check(&f);
		if (v->bad >= 0)
			CHECK_EQ(!good, v->bad);
	}
}

int main(void)
{
	test_packets();
	test_probe();
	test_runs();
	test_cyacd();
	test_check();
	test_images();
	return t_finish("test_s07_cypress");
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-04 persistent records.
 * Requirements: S-04.001-.011, S-04.020-.029 (the rules and the reset
 * sequence), S-04.024, S-04.040-.046, S-04.060-.065 (payloads),
 * S-04.066, S-04.070, S-04.071; on the OP-1F, S-04.040, S-04.043, S-04.046
 * and S-04.066.
 * Vectors: tests/oracle/S-04-records reboot_record.json, reason_table.tsv,
 * host_modes.tsv, writers.tsv, metadata.json, rtc.json, psoc_status.json,
 * metadata_1f.json, psoc_status_1f.json (vectors/s04_vectors.h); the name
 * cases of R-W4.
 */
#include <string.h>

#include "waffle/s04_records.h"
#include "s02_check.h"
#include "vectors/s04_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

/* S-04.001-.004 */
static void test_reboot_record(void)
{
	uint8_t rec[8];
	unsigned i;

	CHECK_EQ(S04_REBOOT_ADDR, V_RR_ADDR);
	for (i = 0; i < N(v_rr_encode); i++) {
		memset(rec, 0x5A, sizeof rec);
		s04_reboot_write(rec, v_rr_encode[i].reason);
		CHECK(memcmp(rec, v_rr_encode[i].bytes6, 6) == 0);
		CHECK_EQ(rec[6], 0x5Au);
		CHECK_EQ(rec[7], 0x5Au);
	}
	for (i = 0; i < N(v_rr_decode); i++) {
		memcpy(rec, v_rr_decode[i].bytes, 8);
		CHECK_EQ(s04_reboot_present(rec), (unsigned)v_rr_decode[i].present);
		if (v_rr_decode[i].present)
			CHECK_EQ(s04_reboot_consume(rec), v_rr_decode[i].reason);
	}
	for (i = 0; i < N(v_rr_write); i++) {
		memcpy(rec, v_rr_write[i].before, 8);
		s04_reboot_write(rec, v_rr_write[i].reason);
		CHECK(memcmp(rec, v_rr_write[i].after, 8) == 0);
	}
	for (i = 0; i < N(v_rr_consume); i++) {
		memcpy(rec, v_rr_consume[i].before, 8);
		CHECK_EQ(s04_reboot_consume(rec), v_rr_consume[i].taken);
		CHECK(memcmp(rec, v_rr_consume[i].after, 8) == 0);
	}
}

/* S-04.004, S-04.006-.011 */
static void test_reason_table(void)
{
	unsigned i, v;

	for (i = 0; i < N(v_reasons); i++) {
		const struct v_reason *r = &v_reasons[i];
		struct s04_reason_effect e;
		uint8_t rec[8];
		uint16_t taken;

		memcpy(rec, r->record, 8);
		taken = s04_reboot_consume(rec);
		CHECK_EQ(taken, r->taken);
		CHECK(memcmp(rec, r->after, 8) == 0);
		s04_reason_effect(taken, &e);
		CHECK_EQ(e.boot_os, (unsigned)r->boots);
		CHECK_EQ(e.screen, r->screen);
		CHECK_EQ(e.cdc, (unsigned)r->cdc);
		CHECK_EQ(e.transport, r->transport);
	}
	/* Every 16-bit value not named in S-04.006 boots (S-04.011). */
	for (v = 0; v <= 0xFFFFu; v++) {
		struct s04_reason_effect e;

		s04_reason_effect((uint16_t)v, &e);
		CHECK_EQ(e.boot_os, !(v == 10u || v == 30u || v == 40u || v == 60u));
	}
}

/* S-04.021 */
static void test_host_modes(void)
{
	unsigned i;

	for (i = 0; i < N(v_host_modes); i++)
		CHECK_EQ(s04_host_mode_reason(v_host_modes[i].payload, v_host_modes[i].len),
			 v_host_modes[i].reason);
}

/* --- S-04.020-.029 --- */

static char g_seq[32];
static unsigned g_nseq;
static uint16_t g_written;
static uint32_t g_delay;

static void ev(char c)
{
	if (g_nseq + 1u < sizeof g_seq)
		g_seq[g_nseq++] = c;
	g_seq[g_nseq] = '\0';
}

static void op_psoc(void *ctx) { (void)ctx; ev('P'); }
static void op_shutdown(void *ctx) { (void)ctx; ev('S'); }
static void op_enter(void *ctx) { (void)ctx; ev('E'); }
static void op_exit(void *ctx) { (void)ctx; ev('X'); }
static void op_link_stop(void *ctx) { (void)ctx; ev('L'); }
static void op_delay(void *ctx, uint32_t ms) { (void)ctx; g_delay = ms; ev('D'); }
static void op_drain(void *ctx) { (void)ctx; ev('U'); }
static void op_write(void *ctx, uint16_t r) { (void)ctx; g_written = r; ev('W'); }
static void op_hard(void *ctx) { (void)ctx; ev('H'); }
static void op_wait(void *ctx) { (void)ctx; ev('Q'); }

static const struct s04_reboot_ops g_reboot_ops = {
	NULL, op_psoc, op_shutdown, op_enter, op_exit, op_link_stop, op_delay, op_drain,
	op_write, op_hard, op_wait,
};

static void test_writers(void)
{
	static const struct { int id; uint16_t arg; const char *seq; } runs[] = {
		{1, 30, "SEUWH"}, {1, 10, "SEUWH"}, {1, 50, "SEUWH"}, {1, 40, "SEUWH"},
		{2, 0, "PSXUWQ"}, {3, 0, "SXDUWH"}, {4, 0, "SEUWH"},
		{6, 0, "SXLUWH"}, {7, 20, "SUWH"}, {7, 30, "SUWH"}, {9, 0, "UWH"},
		{11, 0, "SXUWH"}, {12, 0, "SXUWH"},
	};
	unsigned i;

	for (i = 0; i < N(v_writers); i++) {
		const struct v_writer *v = &v_writers[i];
		struct s04_writer_rule r;
		uint16_t arg = v->reason == -1 ? 30u : v->reason == -2 ? 40u : 0u;

		s04_writer_rule((enum s04_writer)v->id, arg, &r);
		CHECK_EQ(r.reason, v->reason >= 0 ? (uint16_t)v->reason : arg);
		CHECK_EQ(r.reset, v->reset);
		CHECK_EQ(r.nordic, v->nordic);
		CHECK_EQ(r.shutdown, v->shutdown == 1);
		CHECK_EQ(r.psoc_reset_request, (unsigned)v->psoc_reset);
		CHECK_EQ(r.min_delay_ms, v->delay);
		/* S-04.029: a soft reboot writes 20 only. */
		if (r.reset == S04_RESET_SOFT)
			CHECK_EQ(r.reason, 20u);
	}
	/* W1 with every host mode reason: the Nordic step follows S-04.027. */
	for (i = 0; i < N(v_host_modes); i++) {
		struct s04_writer_rule r;

		s04_writer_rule(S04_W1, v_host_modes[i].reason, &r);
		CHECK_EQ(r.nordic, S04_NORDIC_ENTER_DFU);
	}
	CHECK_EQ(s04_nordic_step(20), S04_NORDIC_EXIT_DFU);
	CHECK_EQ(s04_nordic_step(0), S04_NORDIC_NONE);
	CHECK_EQ(s04_nordic_step(30), S04_NORDIC_ENTER_DFU);
	/* The sequences: S-04.022 (W2), S-04.027 (W6), S-04.028 (record, then reset),
	 * S-13b.072 (the links drained before the record). */
	for (i = 0; i < N(runs); i++) {
		g_nseq = 0;
		g_seq[0] = '\0';
		g_written = 0xFFFF;
		g_delay = 0;
		s04_reboot(&g_reboot_ops, (enum s04_writer)runs[i].id, runs[i].arg);
		CHECK(strcmp(g_seq, runs[i].seq) == 0);
		if (runs[i].id == 1 || runs[i].id == 7)
			CHECK_EQ(g_written, runs[i].arg);
		if (runs[i].id == 3)
			CHECK(g_delay >= 10u);		/* S-04.023 */
		if (runs[i].id == 12)
			CHECK_EQ(g_written, S04_REASON_OS_FILE);	/* S-04.014 */
	}
	/* S-04.014: reason 90 boots, and a present record acts once. */
	{
		uint8_t rec[S04_REBOOT_SIZE] = { 0 };
		struct s04_reason_effect e;

		s04_reboot_write(rec, S04_REASON_OS_FILE);
		CHECK_EQ(s04_reboot_consume(rec), S04_REASON_OS_FILE);
		s04_reason_effect(S04_REASON_OS_FILE, &e);
		CHECK(e.boot_os && e.screen == S04_SCREEN_MAIN && !e.cdc);
		CHECK_EQ(s04_reboot_consume(rec), S04_REASON_DEFAULT);
	}
}

/* S-04.024 (scenario R-W4's names) */
static void test_cdc_file(void)
{
	static const struct { const char *names[3]; int regular[3]; int reset; } cases[] = {
		{{"cdc_please", NULL, NULL}, {1, 0, 0}, 1},
		{{"please-cdc.txt", NULL, NULL}, {1, 0, 0}, 1},
		{{"CDC_PLEASE", NULL, NULL}, {1, 0, 0}, 0},
		{{".cdc_please", NULL, NULL}, {1, 0, 0}, 0},
		{{"cdc_please", NULL, NULL}, {0, 0, 0}, 0},	/* a directory */
		{{"cdc_please", "junk.tfw", NULL}, {1, 1, 0}, 0},
		{{"cdc_please", "notes_tfw.txt", NULL}, {1, 1, 0}, 0},
		{{"cdc_please", ".x.tfw", NULL}, {1, 1, 0}, 1},
		{{"cdc_please", "tfw", NULL}, {1, 0, 0}, 1},	/* a directory named tfw */
		{{"readme.txt", NULL, NULL}, {1, 0, 0}, 0},
	};
	unsigned i, j;

	for (i = 0; i < N(cases); i++) {
		struct s04_cdc_scan s;

		s04_cdc_scan_init(&s);
		for (j = 0; j < 3u && cases[i].names[j] != NULL; j++)
			s04_cdc_scan_entry(&s, cases[i].names[j], cases[i].regular[j] != 0);
		CHECK_EQ(s04_cdc_scan_result(&s), (unsigned)cases[i].reset);
	}
	/* "cdc/please" is a path, not a root name: no root entry has that name. */
}

static void fields_to_meta(const struct v_meta_fields *f, struct s04_meta *m)
{
	unsigned i;

	memset(m, 0, sizeof *m);
	strncpy(m->serial, f->serial, 8);
	m->sku = f->sku;
	m->factory = f->factory;
	for (i = 0; i < 4u; i++) {
		m->os_version[i] = f->os[i];
		m->sw_version[i] = f->sw[i];
	}
	m->state = f->state;
}

static void check_fields(const struct s04_meta *m, const struct v_meta_fields *f)
{
	unsigned i;

	if (f->has & 1)
		CHECK(strcmp(m->serial, f->serial) == 0);
	if (f->has & 2)
		CHECK_EQ(m->sku, f->sku);
	if (f->has & 4)
		CHECK_EQ(m->factory, f->factory);
	for (i = 0; i < 4u; i++) {
		if (f->has & 8)
			CHECK_EQ(m->os_version[i], f->os[i]);
		if (f->has & 16)
			CHECK_EQ(m->sw_version[i], f->sw[i]);
	}
	if (f->has & 32)
		CHECK_EQ(m->state, f->state);
}

/* S-04.040-.046 */
static void test_metadata(void)
{
	struct s04_meta m;
	uint8_t out[S04_META_SIZE];
	unsigned i;

	/* encode: the reader's view of the sample, and the CV words. */
	const struct wb_device *dev = &wb_device_opxy;

	CHECK_EQ(s04_meta_size(dev), S04_META_SIZE);
	s04_meta_read(dev, v_meta_encode_bytes, &m);
	check_fields(&m, &v_meta_encode_fields);
	CHECK_EQ(m.cv_gain, V_META_ENCODE_CV_GAIN);
	CHECK_EQ(m.cv_zero, V_META_ENCODE_CV_ZERO);
	CHECK_EQ(m.cv_pair2[0], V_META_ENCODE_CV_P0);
	CHECK_EQ(m.cv_pair2[1], V_META_ENCODE_CV_P1);
	/* Built from itself in factory mode, the record is unchanged. */
	s04_meta_build(dev, v_meta_encode_bytes, &m, true, out);
	CHECK(memcmp(out, v_meta_encode_bytes, S04_META_SIZE) == 0);

	for (i = 0; i < N(v_meta_reads); i++) {
		s04_meta_read(dev, v_meta_reads[i].bytes, &m);
		check_fields(&m, &v_meta_reads[i].f);
		CHECK(strlen(m.serial) <= 8u);
	}
	for (i = 0; i < N(v_meta_writes); i++) {
		const struct v_meta_write *v = &v_meta_writes[i];

		fields_to_meta(&v->cur, &m);
		s04_meta_build(dev, v->before, &m, v->factory_mode != 0, out);
		CHECK(memcmp(out, v->after, S04_META_SIZE) == 0);
	}
	for (i = 0; i < N(v_setmetas); i++) {
		const struct v_setmeta *v = &v_setmetas[i];
		struct s04_setmeta req;
		struct s04_meta before;
		bool writes;

		memset(&m, 0, sizeof m);
		strcpy(m.serial, "OLD00000");
		m.sku = 77;
		m.factory = 8;
		before = m;
		req.serial = v->serial;
		req.sku_variant = v->sku;
		req.factory = v->factory;
		writes = s04_set_metadata(dev, &m, v->factory_mode != 0, v->option != 0, &req);
		CHECK_EQ(writes, (unsigned)v->writes);
		if (!writes)
			CHECK(memcmp(&m, &before, sizeof m) == 0);
		check_fields(&m, &v->result);
		if (writes && !(v->result.has & 2))
			CHECK_EQ(m.sku, 77u);	/* absent keys leave their fields */
	}
	for (i = 0; i < N(v_m6); i++)
		CHECK_EQ(s04_m6_needed(v_m6[i].sw), (unsigned)v_m6[i].writes);
}

/*
 * S-04.040, S-04.043, S-04.046 on the OP-1F: 0x28 bytes, no factory or CV
 * field, +0x10..+0x11 kept by every write, the factory key passed over.
 */
static void test_metadata_1f(void)
{
	const struct wb_device *dev = &wb_device_op1f;
	struct s04_meta m;
	uint8_t out[S04_META_SIZE];
	unsigned i;

	CHECK_EQ(s04_meta_size(dev), V_META1F_SIZE);
	s04_meta_read(dev, v_meta1f_encode_bytes, &m);
	check_fields(&m, &v_meta1f_encode_fields);
	CHECK_EQ(m.factory, 0u);
	CHECK_EQ(m.cv_gain, 0u);
	CHECK_EQ(m.cv_zero, 0u);
	/* the build writes 0x28 bytes and nothing past them */
	memset(out, 0xA5, sizeof out);
	s04_meta_build(dev, v_meta1f_encode_bytes, &m, true, out);
	CHECK(memcmp(out, v_meta1f_encode_bytes, V_META1F_SIZE) == 0);
	for (i = V_META1F_SIZE; i < S04_META_SIZE; i++)
		CHECK_EQ(out[i], 0xA5u);
	for (i = 0; i < N(v_meta1f_reads); i++) {
		s04_meta_read(dev, v_meta1f_reads[i].bytes, &m);
		check_fields(&m, &v_meta1f_reads[i].f);
		CHECK_EQ(m.factory, 0u);
	}
	for (i = 0; i < N(v_meta1f_writes); i++) {
		const struct v_meta1f_write *v = &v_meta1f_writes[i];

		fields_to_meta(&v->cur, &m);
		s04_meta_build(dev, v->before, &m, v->factory_mode != 0, out);
		CHECK(memcmp(out, v->after, V_META1F_SIZE) == 0);
		CHECK(memcmp(out + 0x10, v->before + 0x10, 2) == 0);
	}
	for (i = 0; i < N(v_meta1f_setmetas); i++) {
		const struct v_setmeta *v = &v_meta1f_setmetas[i];
		struct s04_setmeta req;
		struct s04_meta before;
		bool writes;

		memset(&m, 0, sizeof m);
		strcpy(m.serial, "OLD00000");
		m.sku = 77;
		before = m;
		req.serial = v->serial;
		req.sku_variant = v->sku;
		req.factory = v->factory;
		writes = s04_set_metadata(dev, &m, v->factory_mode != 0, v->option != 0, &req);
		CHECK_EQ(writes, (unsigned)v->writes);
		CHECK_EQ(m.factory, 0u);
		if (!writes)
			CHECK(memcmp(&m, &before, sizeof m) == 0);
		check_fields(&m, &v->result);
	}
}

static void check_msg_1f(const struct s04_psoc_msg *m, const struct v_msg *v)
{
	CHECK_EQ(m->id, v->id);
	CHECK_EQ(m->len, v->len);
	CHECK(memcmp(m->payload, v->payload, v->len) == 0);
}

/* S-04.066 */
struct shut_rig {
	bool mounted;
	int fail;
	char log[8];
	unsigned n;
};

static bool shut_mounted(void *ctx)
{
	return ((struct shut_rig *)ctx)->mounted;
}

static int shut_emmc(void *ctx)
{
	struct shut_rig *r = ctx;

	r->log[r->n++] = 'e';
	return r->fail;
}

static void shut_reports(void *ctx)
{
	struct shut_rig *r = ctx;

	r->log[r->n++] = 'r';
}

static void test_shutdown_one_cpu(void)
{
	static const struct { bool mounted; int fail; const char *want; } cases[] = {
		{true, 0, "er"}, {true, -1, "er"}, {false, 0, "r"},
	};
	unsigned i;

	for (i = 0; i < N(cases); i++) {
		struct shut_rig r = { cases[i].mounted, cases[i].fail, {0}, 0 };
		const struct s04_shutdown_ops ops = { &r, shut_mounted, shut_emmc, shut_reports };

		s04_shutdown_one_cpu(&ops);
		CHECK(strcmp(r.log, cases[i].want) == 0);
	}
	/* the reports: 0x51 {00 00 02} alone (no 0x40 {01}, S-07.046) */
	CHECK_EQ(N(v_psoc1f_shutdown), 1u);
	check_msg_1f(&s04_msg_shutdown, &v_psoc1f_shutdown[0]);
	check_msg_1f(&s04_msg_held, &v_psoc1f_held[0]);
	CHECK_EQ(N(v_psoc1f_held), 1u);
	check_msg_1f(&s04_msg_handover, &v_psoc1f_handover[0]);
	check_msg_1f(&s04_msg_reset_request, &v_psoc1f_reset_request[0]);
	check_msg_1f(&s04_msg_shutdown, &v_psoc1f_reset_request[1]);
}

/* S-04.060-.065: the payloads */
static void check_msg(const struct s04_psoc_msg *m, const struct v_msg *v)
{
	CHECK_EQ(m->id, v->id);
	CHECK_EQ(m->len, v->len);
	CHECK(memcmp(m->payload, v->payload, v->len) == 0);
}

static void test_psoc_reports(void)
{
	check_msg(&s04_msg_start, &v_psoc_start[0]);
	check_msg(&s04_msg_held, &v_psoc_held[0]);
	check_msg(&s04_msg_held_screen, &v_psoc_held[1]);
	check_msg(&s04_msg_held_bar, &v_psoc_held[2]);
	check_msg(&s04_msg_handover, &v_psoc_handover[0]);
	check_msg(&s04_msg_shutdown, &v_psoc_shutdown[0]);
	check_msg(&s04_msg_shutdown_40, &v_psoc_shutdown[1]);
	check_msg(&s04_msg_held, &v_psoc_regreet[0]);
	check_msg(&s04_msg_reset_request, &v_psoc_reset_request[0]);
	check_msg(&s04_msg_shutdown, &v_psoc_reset_request[1]);
}

/* S-04.070 */
static void test_rtc(void)
{
	unsigned i;

	for (i = 0; i < N(v_rtc_encode); i++) {
		const struct v_rtc *v = &v_rtc_encode[i];
		struct s04_datetime t;
		uint32_t clk = 0;

		CHECK(s04_rtc_encode(v->y, v->mo, v->d, v->h, v->mi, v->s, &clk));
		CHECK_EQ(clk, v->clk);
		CHECK_EQ(clk >> 17, v->days);
		s04_rtc_decode(clk, &t);
		/* Round trip, except 2000-12-30 and -31 (to 2001-01-01 and -02). */
		if (v->y == 2000u && v->mo == 12u && v->d >= 30u) {
			CHECK_EQ(t.year, 2001u);
			CHECK_EQ(t.month, 1u);
			CHECK_EQ(t.day, v->d - 29u);
		} else {
			CHECK_EQ(t.year, v->y);
			CHECK_EQ(t.month, v->mo);
			CHECK_EQ(t.day, v->d);
		}
		CHECK_EQ(t.hour, v->h);
		CHECK_EQ(t.minute, v->mi);
		CHECK_EQ(t.second, v->s);
	}
	for (i = 0; i < N(v_rtc_decode); i++) {
		struct s04_datetime t;

		s04_rtc_decode(v_rtc_decode[i].days << 17, &t);
		CHECK_EQ(t.year, v_rtc_decode[i].y);
		CHECK_EQ(t.month, v_rtc_decode[i].mo);
		CHECK_EQ(t.day, v_rtc_decode[i].d);
	}
	{
		uint32_t clk;

		CHECK(s04_rtc_encode(2059, 9, 20, 0, 0, 0, &clk));	/* the last date */
		CHECK_EQ(clk >> 17, 32767u);
		CHECK(!s04_rtc_encode(2059, 9, 21, 0, 0, 0, &clk));
		CHECK(!s04_rtc_encode(1969, 12, 31, 0, 0, 0, &clk));
		CHECK(!s04_rtc_encode(2023, 2, 29, 0, 0, 0, &clk));
	}
}

/* S-04.071 */
static void test_fatal_record(void)
{
	static const struct { int cause; uint32_t want[3]; } cases[] = {
		/* S-04.071's DDR row: not an unhandled event's (8, 0xB) */
		{S04_FATAL_DDR_CHECK, {0xB, 1, 0x84000000u}},
	};
	unsigned i, k;

	CHECK_EQ(S04_FATAL_ADDR, 0x0803DFD4u);
	CHECK(S04_FATAL_ADDR + S04_FATAL_SIZE <= 0x0803DFE4u);
	for (i = 0; i < N(cases); i++) {
		uint8_t out[16];

		s04_fatal_record((enum s04_fatal_cause)cases[i].cause, 0x11A01234u,
				 out);
		for (k = 0; k < 3u; k++) {
			uint32_t got = (uint32_t)out[4 * k] | ((uint32_t)out[4 * k + 1] << 8) |
				       ((uint32_t)out[4 * k + 2] << 16) |
				       ((uint32_t)out[4 * k + 3] << 24);

			CHECK_EQ(got, cases[i].want[k]);
		}
		CHECK_EQ(out[12], 0x34u);
		CHECK_EQ(out[15], 0x11u);
	}
}

int main(void)
{
	test_reboot_record();
	test_reason_table();
	test_host_modes();
	test_writers();
	test_cdc_file();
	test_metadata();
	test_metadata_1f();
	test_shutdown_one_cpu();
	test_psoc_reports();
	test_rtc();
	test_fatal_record();
	return finish("test_s04_records");
}

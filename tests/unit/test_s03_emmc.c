/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-03 eMMC against the RAM fake.
 * Requirements: S-03.001, .002, .004-.011, .020-.027, .033, .040-.048,
 * .050-.064, .066-.068, .070-.073.
 * Vectors: tests/oracle/S-03-emmc/vectors headers.json, load_table.json,
 * write_sequences.json, user_area.json (vectors/s03_vectors.h);
 * scenarios SC-03-LOAD-TABLE, SC-03-WRITE-TABLE, SC-03-WRITE-CUT,
 * SC-03-WRITE-TORN, SC-03-WRITE-CACHE, SC-03-SELECT-ERR, SC-03-READ-ERR, SC-03-FINAL-SELECT-ERR,
 * SC-03-BOOT-SIZE, SC-03-FEATURES, SC-03-POWER-OFF, SC-03-POWER-OFF-ERR,
 * SC-03-EMMC-OTHER.
 */
#include <string.h>

#include "s02_check.h"
#include "s03_fake_emmc.h"
#include "vectors/s03_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define L_A 12288u
#define L_B 12544u

static struct fake_emmc g_e;
static struct s03_emmc_ops g_ops;
static uint8_t g_staging[S03_OS_MAX];
static uint8_t g_new[FE_B_MAX];
static uint8_t g_old[16384];
static uint8_t g_img[FE_B_MAX];

static void fill(uint8_t *dst, uint32_t n, uint32_t seed)
{
	uint32_t x = seed * 2654435761u + 0x9E3779B9u, i;

	for (i = 0; i < n; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		dst[i] = (uint8_t)(x >> 9);
	}
}

/* --- headers (S-03.021-.026) --- */

static void test_headers(void)
{
	unsigned i;

	for (i = 0; i < N(v_headers); i++) {
		const struct v_hdr *v = &v_headers[i];
		uint8_t raw[S03_SECTOR];
		struct s03_header h;

		memset(raw, v->reserved_ff ? 0xFF : 0x00, sizeof raw);
		memcpy(raw, v->raw, 14);
		s03_header_decode(raw, &h);
		CHECK_EQ(h.format, v->format);
		CHECK_EQ(h.length, v->length);
		CHECK_EQ(h.crc, v->crc);
		CHECK_EQ(s03_header_active(&h), (unsigned)v->active);
		CHECK_EQ(s03_header_mode(&h), v->mode);
	}
	{
		uint8_t raw[S03_SECTOR];
		unsigned k;

		s03_header_encode(0x200100u, 0x12345678u, 1, raw);
		CHECK(memcmp(raw, v_headers[0].raw, 16) == 0);	/* writer_plain */
		for (k = 0x0E; k < S03_SECTOR; k++)
			CHECK_EQ(raw[k], 0);
	}
	CHECK_EQ(s03_sectors(0), 0);
	CHECK_EQ(s03_sectors(1), 1);
	CHECK_EQ(s03_sectors(512), 1);
	CHECK_EQ(s03_sectors(513), 2);
}

/* --- the load (S-03.040-.048) --- */

static struct s03_env env_now(void)
{
	return fe_env(&g_e, &g_ops);
}

/* The vectors' partition states (gen_vectors.py STATES), with this test's images. */
static void build_state(unsigned part, const char *st, uint32_t seed)
{
	uint8_t *p = fe_part(&g_e, part);
	uint32_t B = g_e.B;

	memset(p, 0, B);
	if (strcmp(st, "erased") == 0)
		return;
	if (strcmp(st, "good") == 0 || strcmp(st, "bad_crc") == 0) {
		fill(g_img, L_B, seed);
		fe_install(&g_e, part, g_img, L_B, 3, 1, 0, 0, 0);
		if (st[0] == 'b')
			p[S03_SECTOR + 100u] ^= 0xFFu;
	} else if (strcmp(st, "good512") == 0) {
		fill(g_img, L_A, seed);
		fe_install(&g_e, part, g_img, L_A, 3, 1, 0, 0, 0);
	} else if (strcmp(st, "inactive_good") == 0) {
		fill(g_img, L_B, seed);
		fe_install(&g_e, part, g_img, L_B, 3, 0, 0, 0, 0);
	} else if (strcmp(st, "zero_length") == 0) {
		fe_header_only(&g_e, part, 3, 0, 0, 1, 0);
	} else if (strcmp(st, "too_long") == 0) {
		fe_header_only(&g_e, part, 3, 0x800001u, 0, 1, 0);
	} else if (strcmp(st, "past_partition") == 0) {
		fe_header_only(&g_e, part, 3, B - 511u, 0, 1, 0);
	} else if (strcmp(st, "pad_garbage") == 0) {
		fill(g_img, L_B, seed);
		fe_install(&g_e, part, g_img, L_B, 3, 1, 0, 0xA5, 0xFF);
	} else if (strcmp(st, "active_ff") == 0) {
		fill(g_img, L_B, seed);
		fe_install(&g_e, part, g_img, L_B, 3, 0xFF, 0, 0, 0);
	} else if (strcmp(st, "enc_f3") == 0) {
		fill(g_img, L_B, seed);
		fe_install(&g_e, part, g_img, L_B, 3, 1, 1, 0, 0);
	} else if (strcmp(st, "enc_f2") == 0) {
		fill(g_img, 0x11000u, seed);
		fe_install(&g_e, part, g_img, 0x11000u, 2, 1, 1, 0, 0);
	} else if (strcmp(st, "f0_enc_byte") == 0) {
		fill(g_img, L_B, seed);
		fe_install(&g_e, part, g_img, L_B, 0, 1, 1, 0, 0);
	} else if (strcmp(st, "partition_full") == 0) {
		fill(g_img, B - 512u, seed);
		fe_install(&g_e, part, g_img, B - 512u, 3, 1, 0, 0, 0);
	} else if (strcmp(st, "format4") == 0) {
		fill(g_img, L_B, seed);
		fe_install(&g_e, part, g_img, L_B, 4, 1, 0, 0, 0);
	} else if (strcmp(st, "format_max") == 0) {
		fill(g_img, L_B, seed);
		fe_install(&g_e, part, g_img, L_B, 0xFFFFFFFFu, 1, 0, 0, 0);
	} else if (strcmp(st, "written_plain") == 0) {
		/* What the writer leaves in its target from an erased eMMC (S-03.033). */
		static uint8_t other[FE_B_MAX];
		struct s03_env env = env_now();
		struct s03_write w;
		unsigned o = part == S03_PART_BP1 ? S03_PART_BP2 : S03_PART_BP1;

		memcpy(other, fe_part(&g_e, o), B);
		memset(fe_part(&g_e, o), 0, B);
		fill(g_img, L_B, seed);
		s03_write_os(&env, g_img, L_B, &w);
		CHECK_EQ(w.result, S03_WRITE_OK);
		CHECK_EQ(w.target, S03_PART_BP1);
		if (part == S03_PART_BP2)
			memcpy(p, g_fe_bp1, B);
		memcpy(fe_part(&g_e, o), other, B);
		fe_reset(&g_e);
	}
}

static int result_reason(enum s03_load_result r)
{
	switch (r) {
	case S03_LOAD_OK: return 0;
	case S03_LOAD_NO_ACTIVE: return 1;
	case S03_LOAD_ZERO_LENGTH: return 2;
	case S03_LOAD_TOO_LONG: return 3;
	case S03_LOAD_PAST_PARTITION: return 4;
	case S03_LOAD_FORMAT: return 5;
	case S03_LOAD_ENCRYPTED: return 6;
	case S03_LOAD_CRC: return 7;
	case S03_LOAD_IO: return 8;
	default: return 9;
	}
}

static void test_load_table(void)
{
	unsigned i, k;

	fe_init(&g_e, V_B);
	for (i = 0; i < N(v_loads); i++) {
		const struct v_load *v = &v_loads[i];
		struct s03_env env;
		struct s03_load ld;
		unsigned nsel = 0, nhdr = 0, part;
		uint8_t sels[8] = {0}, hdrs[4] = {0};
		uint32_t lo = ~0u, hi = 0;

		/* Seeds as the vectors (1 for BP1, 2 for BP2). */
		build_state(S03_PART_BP2, v_load_states[v->p2], 2);
		build_state(S03_PART_BP1, v_load_states[v->p1], 1);
		/* The headers the vectors show, but for the CRC words of our own images. */
		CHECK(memcmp(g_fe_bp1, v->h1, 8) == 0 && memcmp(g_fe_bp1 + 12, v->h1 + 12, 4) == 0);
		CHECK(memcmp(g_fe_bp2, v->h2, 8) == 0 && memcmp(g_fe_bp2 + 12, v->h2 + 12, 4) == 0);
		fe_reset(&g_e);
		env = env_now();
		s03_load_os(&env, g_staging, sizeof g_staging, &ld);
		CHECK_EQ(result_reason(ld.result), v->reason);
		CHECK_EQ(ld.result == S03_LOAD_OK ? ld.part : 0u, (unsigned)v->expect);
		CHECK_EQ(ld.length, v->loaded_length);
		CHECK(!ld.select0_failed);
		/* What a failed load knew: no failed request, the chosen
		 * header as read, and a CRC mismatch's two words. */
		CHECK_EQ(ld.step, S03_STEP_NONE);
		CHECK(ld.result != S03_LOAD_CRC ||
		      (ld.crc != ld.header.crc && ld.part != 0u &&
		       ld.header.length != 0u));
		/* S-03.041: the selects, the header reads; no write, no erase. */
		part = 0;
		for (k = 0; k < g_e.nlog; k++) {
			const struct fe_op *o = &g_e.log[k];

			CHECK(o->kind == 's' || o->kind == 'r');
			if (o->kind == 's') {
				if (nsel < N(sels))
					sels[nsel] = (uint8_t)o->a;
				nsel++;
				part = o->a;
			} else if (o->a == 0u) {
				if (nhdr < N(hdrs))
					hdrs[nhdr] = (uint8_t)part;
				nhdr++;
			} else {
				if (o->a < lo)
					lo = o->a;
				if (o->a + o->b - 1u > hi)
					hi = o->a + o->b - 1u;
			}
		}
		CHECK_EQ(nsel, v->nselects);
		for (k = 0; k < nsel && k < v->nselects; k++)
			CHECK_EQ(sels[k], v->selects[k]);
		CHECK(nsel >= 1u && nsel <= N(sels) && sels[nsel - 1u] == 0);	/* S-03.007 */
		CHECK_EQ(nhdr, v->nreads);
		for (k = 0; k < nhdr && k < v->nreads; k++)
			CHECK_EQ(hdrs[k], v->reads[k]);
		/* S-03.043: the extent, and nothing past N + 1 or the partition. */
		if (v->ext_last != 0u) {
			CHECK_EQ(lo, v->ext_first);
			CHECK(hi >= v->ext_last && hi <= v->ext_last + 1u);
			CHECK(!g_e.past_end);
		} else if (v->reason != 7) {
			CHECK_EQ(hi, 0);		/* S-03.042, S-03.048: no image read */
		}
		if (ld.result == S03_LOAD_OK) {
			const uint8_t *chosen = fe_part(&g_e, ld.part) + S03_SECTOR;

			CHECK(memcmp(g_staging, chosen, ld.length) == 0);	/* S-03.047 */
		}
	}
}

/* SC-03-SELECT-ERR, SC-03-READ-ERR, SC-03-FINAL-SELECT-ERR (S-03.010, .046) */
static void test_load_failures(void)
{
	/* With the request that failed, as the boot's account keeps it
	 * (S-05.006): the step and its partition. */
	static const struct {
		const char *p1;
		enum fe_fail fail;
		enum s03_load_step step;
		unsigned part;
	} cases[] = {
		{"good", FE_FAIL_SELECT1, S03_STEP_SELECT, 1},
		{"inactive_good", FE_FAIL_SELECT2, S03_STEP_SELECT, 2},
		{"good", FE_FAIL_READ_HEADER1, S03_STEP_HEADER, 1},
		{"good", FE_FAIL_READ_IMAGE, S03_STEP_IMAGE, 1},
		{"good", FE_FAIL_SELECT0, S03_STEP_USER, 0},
		{"bad_crc", FE_FAIL_SELECT0, S03_STEP_USER, 0},
	};
	unsigned i;

	fe_init(&g_e, V_B);
	for (i = 0; i < N(cases); i++) {
		struct s03_env env;
		struct s03_load ld;

		build_state(S03_PART_BP1, cases[i].p1, 1);
		build_state(S03_PART_BP2, "good", 2);
		fe_reset(&g_e);
		g_e.fail = cases[i].fail;
		g_e.target = S03_PART_BP1;	/* "image" reads are BP1's */
		env = env_now();
		s03_load_os(&env, g_staging, sizeof g_staging, &ld);
		CHECK(ld.result != S03_LOAD_OK);
		CHECK_EQ(ld.length, 0);
		CHECK_EQ(ld.select0_failed, cases[i].fail == FE_FAIL_SELECT0);
		CHECK_EQ(ld.result, S03_LOAD_IO);
		CHECK_EQ(ld.step, cases[i].step);
		CHECK_EQ(ld.step_part, cases[i].part);
		/* the read that failed: the image's first, all of it */
		CHECK(cases[i].step != S03_STEP_IMAGE ||
		      (ld.sector == 1u && ld.count == s03_sectors(ld.header.length)));
		/* The last select attempted is 0 (S-03.007), after the retries (S-03.010). */
		CHECK(g_e.log[g_e.nlog - 1u].kind == 's' && g_e.log[g_e.nlog - 1u].a == 0u);
		CHECK(g_e.nlog < 40u);
	}
	/* SC-03-BOOT-SIZE: the same image refused at one B and accepted at another. */
	fe_init(&g_e, 0x20000u);
	fill(g_img, 0x1FE00u, 5);
	fe_install(&g_e, S03_PART_BP1, g_img, 0x1FE00u, 3, 1, 0, 0, 0);
	{
		struct s03_env env = env_now();
		struct s03_load ld;

		env.boot_size = 0x20000u;
		s03_load_os(&env, g_staging, sizeof g_staging, &ld);
		CHECK_EQ(ld.result, S03_LOAD_OK);
		env.boot_size = 0x1FE00u;
		s03_load_os(&env, g_staging, sizeof g_staging, &ld);
		CHECK_EQ(ld.result, S03_LOAD_PAST_PARTITION);
		CHECK(s03_image_acceptable(0x1FC00u, 0x20000u));
		CHECK(!s03_image_acceptable(0x1FE01u, 0x20000u));
	}
	/* max_xfer splits the transfers but not the result. */
	fe_init(&g_e, V_B);
	build_state(S03_PART_BP1, "good", 1);
	{
		struct s03_env env = env_now();
		struct s03_load ld;

		env.max_xfer = 4;
		s03_load_os(&env, g_staging, sizeof g_staging, &ld);
		CHECK_EQ(ld.result, S03_LOAD_OK);
		CHECK_EQ(ld.length, L_B);
	}
}

/* --- the writer (S-03.050-.067) --- */

static void start_state(int start)
{
	memset(g_fe_bp1, 0, g_e.B);
	memset(g_fe_bp2, 0, g_e.B);
	switch (start) {
	case 0:		/* p1_active */
		fe_install(&g_e, 1, g_old, sizeof g_old, 3, 1, 0, 0, 0);
		break;
	case 1:		/* p1_inactive_p2_active */
		fill(g_img, L_B, 9);
		fe_install(&g_e, 1, g_img, L_B, 3, 0, 0, 0, 0);
		fe_install(&g_e, 2, g_old, sizeof g_old, 3, 1, 0, 0, 0);
		break;
	case 2:		/* both_active */
		fe_install(&g_e, 1, g_old, sizeof g_old, 3, 1, 0, 0, 0);
		fill(g_img, L_B, 9);
		fe_install(&g_e, 2, g_img, L_B, 3, 1, 0, 0, 0);
		break;
	case 4:		/* p1_bad */
		fe_install(&g_e, 1, g_old, sizeof g_old, 3, 1, 0, 0, 0);
		g_fe_bp1[S03_SECTOR + 7u] ^= 0xFFu;
		break;
	default:	/* neither */
		break;
	}
}

/* 0 old, 1 new, 2 none, 3 anything else */
static int boots(uint32_t new_len)
{
	struct s03_env env;
	struct s03_load ld;

	fe_reset(&g_e);
	env = env_now();
	s03_load_os(&env, g_staging, sizeof g_staging, &ld);
	if (ld.result != S03_LOAD_OK)
		return 2;
	if (ld.length == sizeof g_old && memcmp(g_staging, g_old, sizeof g_old) == 0)
		return 0;
	if (ld.length == new_len && memcmp(g_staging, g_new, new_len) == 0)
		return 1;
	return 3;
}

/* Compare the log with the vector's ops: retries collapsed, adjacent transfers merged. */
static bool ops_match(const struct v_wcase *c)
{
	struct fe_op m[FE_LOG_MAX];
	unsigned n = 0, i;

	for (i = 0; i < g_e.nlog && i < FE_LOG_MAX; i++) {
		const struct fe_op *o = &g_e.log[i];

		if (n > 0u && m[n - 1u].error && m[n - 1u].kind == o->kind &&
		    m[n - 1u].part == o->part && m[n - 1u].a == o->a && o->error)
			continue;			/* a retry of a failing op */
		if (n > 0u && (o->kind == 'r' || o->kind == 'w') && m[n - 1u].kind == o->kind &&
		    !m[n - 1u].error && !o->error && m[n - 1u].part == o->part &&
		    m[n - 1u].a + m[n - 1u].b == o->a && o->a != 0u) {
			m[n - 1u].b += o->b;
			continue;
		}
		m[n++] = *o;
	}
	if (n != c->nops)
		return false;
	for (i = 0; i < n; i++) {
		const struct v_eop *v = &c->ops[i];

		if (v->kind != m[i].kind || v->error != m[i].error)
			return false;
		if (v->kind == 's' && v->part != m[i].a)
			return false;
		if (v->kind != 's' && (v->part != m[i].part || v->a != m[i].a ||
				       (v->kind != 'e' && v->b != m[i].b &&
						 !(v->error && m[i].b <= v->b))))	/* a failed transfer may be a part */
			return false;
	}
	return true;
}

static const enum fe_fail g_fail_map[] = {
	FE_FAIL_NONE, FE_FAIL_SELECT1, FE_FAIL_READ_HEADER1, FE_FAIL_SELECT2, FE_FAIL_ERASE,
	FE_FAIL_WRITE_IMAGE, FE_FAIL_READ_IMAGE, FE_FAIL_WRITE_HEADER, FE_FAIL_READ_HEADER,
	FE_FAIL_WRITE_HEADER_ACTIVE, FE_FAIL_READ_HEADER_ACTIVE, FE_FAIL_SELECT_OTHER,
	FE_FAIL_READ_OTHER, FE_FAIL_DEACTIVATE, FE_FAIL_ERASE_OTHER, FE_FAIL_SELECT0,
	FE_FAIL_FLUSH_IMAGE, FE_FAIL_FLUSH_HEADER, FE_FAIL_FLUSH_FINAL,
};

static void setup_case(const struct v_wcase *c)
{
	fe_init(&g_e, V_B);
	start_state(c->start);
	if (c->length >= 0x30u)
		fill(g_new, c->length, 4);
	else
		memset(g_new, 0, c->length);
	fe_reset(&g_e);
	g_e.fail = g_fail_map[c->fail];
	g_e.target = c->target;
	g_e.corrupt_sector = c->corrupt_sector;
	g_e.corrupt_header = c->corrupt_header;
}

/* What a watched write reported (s03_write_os_watched). */
static struct {
	enum s03_write_stage stages[16];
	unsigned nstages;
	uint32_t done, total;
	unsigned progress;
	bool bad_progress;	/* past its total, or down, within a stage */
	unsigned target_at_erase;
} g_wr;

static void wr_stage(void *ctx, enum s03_write_stage s, const struct s03_write *w)
{
	(void)ctx;
	if (g_wr.nstages < N(g_wr.stages))
		g_wr.stages[g_wr.nstages++] = s;
	if (s == S03_WS_ERASE)
		g_wr.target_at_erase = w->target;
	g_wr.done = g_wr.total = 0;
}

static void wr_progress(void *ctx, uint32_t done, uint32_t total)
{
	(void)ctx;
	if (done > total || done < g_wr.done || (g_wr.total != 0u && total != g_wr.total))
		g_wr.bad_progress = true;
	g_wr.done = done;
	g_wr.total = total;
	g_wr.progress++;
}

static const struct s03_write_watch g_watch = { NULL, wr_stage, wr_progress };

/* Every run of the table goes through the watched write: the same eMMC
 * operations as s03_write_os() (test_watched_is_the_write). */
static int run_case(const struct v_wcase *c, struct s03_write *w)
{
	struct s03_env env = env_now();

	memset(&g_wr, 0, sizeof g_wr);
	s03_write_os_watched(&env, g_new, c->length, w, &g_watch);
	return (int)w->result;
}

/* Where a write of the table stopped, as struct s03_write says it. */
static void check_detail(const struct v_wcase *c, const struct s03_write *w)
{
	unsigned other = c->target == S03_PART_BP1 ? S03_PART_BP2 : S03_PART_BP1, i;
	enum s03_write_fail fail = S03_FAIL_NONE;
	enum s03_write_stage stage = S03_WS_DONE;
	unsigned part = 0;
	uint32_t at = 0;

	if (c->result == 1) {
		fail = S03_FAIL_LENGTH;
		stage = S03_WS_ACCEPT;
	} else if (c->corrupt_sector >= 1) {
		fail = S03_FAIL_MISMATCH;
		stage = S03_WS_VERIFY;
		part = c->target;
		at = (uint32_t)c->corrupt_sector;
	} else if (c->corrupt_header != 0) {
		fail = S03_FAIL_MISMATCH;
		stage = S03_WS_HEADER;
		part = c->target;
	} else {
		switch (g_fail_map[c->fail]) {
		case FE_FAIL_SELECT1:
			fail = S03_FAIL_SELECT; stage = S03_WS_TARGET; part = 1; break;
		case FE_FAIL_READ_HEADER1:
			fail = S03_FAIL_READ; stage = S03_WS_TARGET; part = 1; break;
		case FE_FAIL_SELECT2:
			fail = S03_FAIL_SELECT; stage = S03_WS_TARGET; part = 2; break;
		case FE_FAIL_ERASE:
			fail = S03_FAIL_ERASE; stage = S03_WS_ERASE; part = c->target; break;
		case FE_FAIL_WRITE_IMAGE:
			fail = S03_FAIL_WRITE; stage = S03_WS_WRITE; part = c->target; at = 1; break;
		case FE_FAIL_READ_IMAGE:
			fail = S03_FAIL_READ; stage = S03_WS_VERIFY; part = c->target; at = 1; break;
		case FE_FAIL_WRITE_HEADER:
		case FE_FAIL_WRITE_HEADER_ACTIVE:
			fail = S03_FAIL_WRITE; stage = S03_WS_HEADER; part = c->target; break;
		case FE_FAIL_READ_HEADER:
		case FE_FAIL_READ_HEADER_ACTIVE:
			fail = S03_FAIL_READ; stage = S03_WS_HEADER; part = c->target; break;
		case FE_FAIL_SELECT_OTHER:
			fail = S03_FAIL_SELECT; stage = S03_WS_DEACTIVATE; part = other; break;
		case FE_FAIL_READ_OTHER:
			fail = S03_FAIL_READ; stage = S03_WS_DEACTIVATE; part = other; break;
		case FE_FAIL_DEACTIVATE:
			fail = S03_FAIL_WRITE; stage = S03_WS_DEACTIVATE; part = other; break;
		case FE_FAIL_FLUSH_IMAGE:
			fail = S03_FAIL_FLUSH; stage = S03_WS_HEADER; part = c->target; break;
		case FE_FAIL_FLUSH_HEADER:
			fail = S03_FAIL_FLUSH; stage = S03_WS_DEACTIVATE; part = c->target; break;
		case FE_FAIL_FLUSH_FINAL:
			fail = S03_FAIL_FLUSH; stage = S03_WS_FLUSH; part = other; break;
		case FE_FAIL_NONE:
		case FE_FAIL_ERASE_OTHER:	/* this writer erases nothing there */
		case FE_FAIL_SELECT0:		/* S-03.063: the result kept */
		default:
			break;
		}
	}
	if (!CHECK(w->fail == fail && w->stage == stage && w->part == part && w->at == at &&
		   !w->recovery_failed))
		printf("    %s: fail %d stage %d part %u at %u\n", c->name, (int)w->fail, (int)w->stage,
		       w->part, (unsigned)w->at);
	/* The stages in order, from S03_WS_ACCEPT to where it stopped. */
	CHECK(g_wr.nstages == (unsigned)stage + 1u);
	for (i = 0; i < g_wr.nstages; i++)
		CHECK(g_wr.stages[i] == (enum s03_write_stage)i);
	CHECK(!g_wr.bad_progress);
	if (stage > S03_WS_ERASE)
		CHECK_EQ(g_wr.target_at_erase, c->target);
	if (w->result == S03_WRITE_OK)
		CHECK(g_wr.progress >= 3u);	/* the erase, the writes, the reads */
}

/* The watched write makes the plain one's eMMC operations, in order, and
 * gives its result; NULL watches nothing. */
static void test_watched_is_the_write(void)
{
	static struct fe_op plain[FE_LOG_MAX];
	unsigned i;

	for (i = 0; i < N(v_wcases); i++) {
		const struct v_wcase *c = &v_wcases[i];
		struct s03_env env;
		struct s03_write a, b;
		unsigned n;

		setup_case(c);
		env = env_now();
		s03_write_os(&env, g_new, c->length, &a);
		n = g_e.nlog;
		memcpy(plain, g_e.log, sizeof plain);
		setup_case(c);
		env = env_now();
		s03_write_os_watched(&env, g_new, c->length, &b, &g_watch);
		CHECK(a.result == b.result && a.target == b.target &&
		      a.select0_failed == b.select0_failed && a.fail == b.fail);
		CHECK_EQ(g_e.nlog, n);
		for (unsigned k = 0; k < n && k < FE_LOG_MAX; k++)
			CHECK(plain[k].kind == g_e.log[k].kind && plain[k].part == g_e.log[k].part &&
			      plain[k].a == g_e.log[k].a && plain[k].b == g_e.log[k].b &&
			      plain[k].error == g_e.log[k].error);
		setup_case(c);
		env = env_now();
		s03_write_os_watched(&env, g_new, c->length, &b, NULL);
		CHECK(a.result == b.result && g_e.nlog == n);
	}
}

/* Which model mutating op (1-based) my k-th mutating op belongs to, and whether it ends it. */
static unsigned model_op(const struct v_wcase *c, const struct fe_op *mine, bool *last,
			 unsigned *from)
{
	unsigned i, k = 0;

	/* *from is the vector op the previous one of mine matched; search on from it. */
	*last = false;
	for (i = 0; i < c->nops; i++) {
		const struct v_eop *v = &c->ops[i];

		if (v->kind != 'e' && v->kind != 'w')
			continue;
		k++;			/* the model counts every erase and write, failed ones too */
		if (i < *from || v->error || v->kind != mine->kind || v->part != mine->part)
			continue;
		if (v->kind == 'e' && v->a == mine->a) {	/* the log holds the group */
			*last = true;
			*from = i + 1u;
			return k;
		}
		if (v->kind == 'w' && mine->a >= v->a && mine->a + mine->b <= v->a + v->b) {
			*last = mine->a + mine->b == v->a + v->b;
			*from = *last ? i + 1u : i;
			return k;
		}
	}
	return 0;
}

static void test_write_table(void)
{
	static struct fe_op mut[64];
	static uint8_t other_hdr[S03_SECTOR];
	unsigned i;

	fill(g_old, sizeof g_old, 3);
	for (i = 0; i < N(v_wcases); i++) {
		const struct v_wcase *c = &v_wcases[i];
		struct s03_write w;
		unsigned nmut = 0, j, k, other, from;
		int before;

		setup_case(c);
		before = boots(c->length);
		CHECK_EQ(before, c->boots_before);
		setup_case(c);
		other = c->target == S03_PART_BP1 ? S03_PART_BP2 : S03_PART_BP1;
		memcpy(other_hdr, fe_part(&g_e, other), S03_SECTOR);
		run_case(c, &w);
		CHECK_EQ(w.result, c->result == 0 ? S03_WRITE_OK : c->result == 1 ?
			 S03_WRITE_REFUSED : S03_WRITE_FAILED);
		check_detail(c, &w);
		if (c->result == 1) {			/* S-03.051: no eMMC operation */
			CHECK_EQ(g_e.nlog, 0);
			continue;
		}
		if (c->target != 0u && w.target != 0u)
			CHECK_EQ(w.target, c->target);
		if (!c->rewrite_erase && !CHECK(ops_match(c))) {
			printf("    ops %s:", c->name);
			for (j = 0; j < g_e.nlog; j++)
				printf(" %c%u.%u.%u%s", g_e.log[j].kind, g_e.log[j].part, g_e.log[j].a,
				       g_e.log[j].b, g_e.log[j].error ? "!" : "");
			printf("\n");
		}
		CHECK_EQ(w.select0_failed, c->fail == 15);
		CHECK(!g_e.user_access && !g_e.past_end);	/* S-03.054 */
		/* S-03.054, S-03.027: nothing past last_writable on the target. */
		for (j = 0; j < g_e.nlog; j++) {
			const struct fe_op *o = &g_e.log[j];

			if (o->kind == 'w' && o->part == c->target)
				CHECK(o->a + o->b - 1u <= c->last_writable);
			if ((o->kind == 'e' || o->kind == 'w') && !o->error)
				if (nmut < N(mut))
					mut[nmut++] = *o;
			CHECK(!(o->kind == 's' && o->a == 3u));		/* S-03.006 */
		}
		/* The final state. */
		CHECK_EQ(g_fe_bp1[S03_ACTIVE_OFF] != 0u, (unsigned)c->final_p1);
		CHECK_EQ(g_fe_bp2[S03_ACTIVE_OFF] != 0u, (unsigned)c->final_p2);
		if (c->result == 0) {
			const uint8_t *t = fe_part(&g_e, c->target);
			uint32_t crc = ref_crc32(0, g_new, c->length);

			/* S-03.053: the header, our CRC in place of the vector's. */
			CHECK(memcmp(t, c->header, 8) == 0 && memcmp(t + 12, c->header + 12, 4) == 0);
			CHECK_EQ((uint32_t)t[8] | ((uint32_t)t[9] << 8) | ((uint32_t)t[10] << 16) |
				 ((uint32_t)t[11] << 24), crc);
			CHECK(memcmp(t + S03_SECTOR, g_new, c->length) == 0);	/* S-03.033 */
		}
		CHECK_EQ(boots(c->length), c->final_boots);
		/* S-03.058: after a completed write the other header differs in byte 0x0C alone. */
		if (c->result == 0 && c->target != 0u && !c->rewrite_erase) {
			other_hdr[S03_ACTIVE_OFF] = 0;
			CHECK(memcmp(fe_part(&g_e, other), other_hdr, S03_SECTOR) == 0);
		}

		/* SC-03-WRITE-CUT, SC-03-WRITE-TORN (S-03.059, S-03.067). */
		from = 0;
		for (k = 1; k <= nmut && k <= N(mut); k++) {
			bool last;
			unsigned mk = model_op(c, &mut[k - 1u], &last, &from);
			int b, p;

			if (!CHECK(mk >= 1u && mk <= c->ncuts))
				printf("    cut %s k %u\n", c->name, k);
			if (mk < 1u || mk > c->ncuts)
				continue;
			setup_case(c);
			g_e.cut_after = k;
			run_case(c, &w);
			/* S-03.057: an eMMC lost at a header write takes no
			 * recovery write either */
			CHECK(w.result == S03_WRITE_FAILED && w.fail != S03_FAIL_NONE &&
			      (w.stage != S03_WS_HEADER || w.recovery_failed));
			b = boots(c->length);
			if (last && !CHECK_EQ(b, c->cuts[mk - 1u].boots))
				printf("    %s k %u mk %u\n", c->name, k, mk);
			else if (c->has_torn)
				CHECK(c->cuts[mk - 1u].torn_mask & (1 << b));
			if (!c->has_torn)
				continue;
			for (p = 0; p < 6; p++) {
				setup_case(c);
				g_e.tear = k;
				g_e.tear_pattern = p;
				run_case(c, &w);
				b = boots(c->length);
				CHECK(b < 3 && (c->cuts[mk - 1u].torn_mask & (1 << b)));
			}
		}
	}
}

/*
 * S-03.055 step 1, S-03.056: on an eMMC whose erased bytes read 0xFF, the
 * target's header is made inactive before the copy and after a failure of the
 * first steps, so a cut during the copy or a failed erase boots the old image.
 */
static void test_erased_ff(void)
{
	static const uint32_t lens[] = {0x3000u, 0x90000u};	/* one group, two */
	struct s03_env env;
	struct s03_write w;
	unsigned i, k;

	fill(g_old, sizeof g_old, 3);
	for (i = 0; i < N(lens); i++) {
		const unsigned groups = 1u + lens[i] / (S03_GROUP_SECTORS * S03_SECTOR);

		fill(g_new, lens[i], 4);
		/* A whole write: the new image boots. */
		fe_init(&g_e, V_B);
		start_state(1);			/* target BP1 */
		g_e.erase_ff = true;
		env = env_now();
		s03_write_os(&env, g_new, lens[i], &w);
		CHECK_EQ(w.result, S03_WRITE_OK);
		CHECK_EQ(w.target, S03_PART_BP1);
		CHECK_EQ(boots(lens[i]), 1);
		/* A cut after the zero sector, or during the copy: the old one.
		 * (A cut between the erase and the zero sector is the window
		 * S-03.055 names.) */
		for (k = groups + 1u; k <= groups + 3u; k++) {
			fe_init(&g_e, V_B);
			start_state(1);
			g_e.erase_ff = true;
			g_e.cut_after = k;
			env = env_now();
			s03_write_os(&env, g_new, lens[i], &w);
			if (!CHECK_EQ(boots(lens[i]), 0))
				printf("    len %#x cut %u\n", (unsigned)lens[i], k);
		}
	}
	/* Group 1's erase fails after group 0 read 0xFF: the old image boots. */
	fe_init(&g_e, V_B);
	start_state(1);
	g_e.erase_ff = true;
	g_e.fail_erase_from = 1;
	env = env_now();
	s03_write_os(&env, g_new, 0x90000u, &w);
	CHECK_EQ(w.result, S03_WRITE_FAILED);
	CHECK_EQ(w.fail, S03_FAIL_ERASE);
	CHECK_EQ(g_fe_bp1[S03_ACTIVE_OFF], 0u);
	CHECK_EQ(boots(0x90000u), 0);
}

/*
 * SC-03-WRITE-CACHE (S-03.068, S-03.059): the eMMC holds every erase and write
 * in a volatile cache until a flush. A power cut after any of them, with the
 * eMMC having committed any subset of what the last flush left, boots the old
 * image or the new one, and nothing is left in the cache when the write returns.
 */
static void test_cache(void)
{
	unsigned i, checked = 0;

	fill(g_old, sizeof g_old, 3);
	for (i = 0; i < N(v_wcases); i++) {
		const struct v_wcase *c = &v_wcases[i];
		struct s03_write w;
		unsigned k, mask, muts = 0, j;

		if (c->result != 0 || c->fail != 0 || c->corrupt_sector >= 1 || c->corrupt_header != 0 ||
		    c->rewrite_erase || (c->length != L_A && c->length != L_B))
			continue;
		checked++;
		setup_case(c);
		fe_cache_start(&g_e);
		run_case(c, &w);
		CHECK_EQ(w.result, S03_WRITE_OK);
		CHECK_EQ(g_e.ncached, 0);		/* the third flush */
		CHECK(!g_e.past_end);
		for (j = 0; j < g_e.nlog && j < FE_LOG_MAX; j++)
			muts += (g_e.log[j].kind == 'e' || g_e.log[j].kind == 'w') && !g_e.log[j].error;
		CHECK(memcmp(g_fe_dur1, g_fe_bp1, FE_CACHE_SPAN) == 0 &&
		      memcmp(g_fe_dur2, g_fe_bp2, FE_CACHE_SPAN) == 0);
		fe_cache_lose(&g_e, 0);
		CHECK_EQ(boots(c->length), 1);
		for (k = 1; k <= muts; k++) {
			unsigned nc;

			setup_case(c);
			fe_cache_start(&g_e);
			g_e.cut_after = k;
			run_case(c, &w);
			CHECK_EQ(w.result, S03_WRITE_FAILED);
			nc = g_e.ncached;
			for (mask = 0; mask < (1u << nc); mask++) {
				int b;

				fe_cache_lose(&g_e, mask);
				b = boots(c->length);
				if (!CHECK(c->boots_before == 0 ? b == 0 || b == 1 : b == 1 || b == 2))
					printf("    %s cut %u committed %x of %u: boots %d\n", c->name, k,
					       mask, nc, b);
			}
		}
	}
	g_e.cache_on = false;
	CHECK_EQ(checked, 10);	/* five starting states, two lengths */
}

/* SC-03-FEATURES, SC-03-POWER-OFF (S-03.009, S-03.011) */
static int g_unmounted_at;
static bool g_unmount_fails;

static int unmount(void *ctx)
{
	(void)ctx;
	g_unmounted_at = (int)g_e.nlog;
	return g_unmount_fails ? -1 : 0;
}

/*
 * SC-03-POWER-OFF-ERR (S-03.011, S-03.010): one step fails on every attempt;
 * step 2 is the unmount's writes.  Every later step is still made, in order,
 * the failing switch at most S03_RETRIES times, and the notification is last.
 */
static void test_power_off_errors(void)
{
	static const uint8_t off[][2] = {{32, 1}, {33, 0}, {163, 0}, {34, 3}};
	static const int failing[] = {0, 1, -1, 2, 3};	/* -1: the unmount */
	struct s03_env env;
	unsigned f, i, k, tries;

	fe_init(&g_e, V_B);
	env = env_now();
	for (f = 0; f < N(failing); f++) {
		fe_reset(&g_e);
		g_unmounted_at = -1;
		g_unmount_fails = failing[f] < 0;
		if (failing[f] >= 0)
			g_e.fail_switch = off[failing[f]][0];
		CHECK_EQ(s03_emmc_shutdown(&env, unmount, NULL), -1);
		/* The log, failed attempts folded: the steps of SC-03-POWER-OFF in order. */
		for (i = 0, k = 0; i < g_e.nlog && i < FE_LOG_MAX; k++) {
			const struct fe_op *o = &g_e.log[i];

			CHECK(k < N(off) && o->kind == 'x' && o->a == off[k][0] && o->b == off[k][1]);
			if (k == 2u)
				CHECK_EQ(g_unmounted_at, (int)i);	/* before BKOPS_EN */
			for (tries = 0; i < g_e.nlog && g_e.log[i].a == o->a; i++)
				tries++;
			if ((int)k == failing[f])
				CHECK(tries >= 1u && tries <= S03_RETRIES && o->error);
			else
				CHECK(tries == 1u && !g_e.log[i - 1u].error);
		}
		CHECK_EQ(k, N(off));
		CHECK_EQ(g_e.log[g_e.nlog - 1u].a, 34u);	/* nothing after the notification */
	}
	g_unmount_fails = false;
}

static void test_features(void)
{
	static const uint8_t on[][2] = {{34, 1}, {33, 1}, {163, 2}};
	static const uint8_t off[][2] = {{32, 1}, {33, 0}, {163, 0}, {34, 3}};
	struct s03_env env;
	unsigned i;

	fe_init(&g_e, V_B);
	env = env_now();
	CHECK_EQ(s03_emmc_features_on(&env), 0);
	CHECK_EQ(g_e.nlog, N(on));
	for (i = 0; i < N(on) && i < g_e.nlog; i++) {
		CHECK_EQ(g_e.log[i].a, on[i][0]);
		CHECK_EQ(g_e.log[i].b, on[i][1]);
	}
	fe_reset(&g_e);
	g_unmounted_at = -1;
	CHECK_EQ(s03_emmc_shutdown(&env, unmount, NULL), 0);
	CHECK_EQ(g_e.nlog, N(off));
	CHECK_EQ(g_unmounted_at, 2);	/* after the flush and the cache switch-off */
	for (i = 0; i < N(off) && i < g_e.nlog; i++) {
		CHECK_EQ(g_e.log[i].a, off[i][0]);
		CHECK_EQ(g_e.log[i].b, off[i][1]);
	}
}

/* S-03.001, S-03.002, S-03.070-.073 */
static void test_emmc_and_user_area(void)
{
	unsigned i;

	CHECK_EQ(V_EMMC_SECTORS, S03_USER_SECTORS);
	CHECK_EQ(V_EMMC_GROUP, S03_GROUP_SECTORS);
	for (i = 0; i < N(v_opt_ok); i++)
		CHECK(s03_emmc_supported(V_EMMC_SECTORS, V_EMMC_GROUP, v_opt_ok[i]));
	for (i = 0; i < N(v_emmc_refused); i++)
		CHECK(!s03_emmc_supported(v_emmc_refused[i].sectors, v_emmc_refused[i].group,
					  v_emmc_refused[i].opt));
	CHECK(!s03_emmc_supported(V_EMMC_SECTORS, V_EMMC_GROUP, 0));
	for (i = 0; i < N(v_map); i++) {
		CHECK_EQ(s03_user_region(v_map[i].first), v_map[i].region);
		CHECK_EQ(s03_user_region(v_map[i].last), v_map[i].region);
		CHECK_EQ(v_map[i].first % S03_GROUP_SECTORS, 0);
		if (i + 1u < N(v_map))
			CHECK_EQ(v_map[i].last + 1u, v_map[i + 1u].first);
	}
	CHECK_EQ(v_map[N(v_map) - 1u].last + 1u, S03_USER_SECTORS);
	CHECK_EQ(s03_user_region(S03_USER_SECTORS), S03_REGION_NONE);
	for (i = 0; i < N(v_fat_map); i++)
		CHECK_EQ(s03_fat_volume_to_user(v_fat_map[i].volume), v_fat_map[i].user);
	CHECK_EQ((V_YAFFS2_LAST_BLOCK + 1u) * V_YAFFS2_BLOCK - 1u, S03_YAFFS2_LAST);
	CHECK_EQ(V_YAFFS2_CHUNK, S03_YAFFS2_CHUNK_SECTORS);
	CHECK_EQ(V_FR_GROUP, S03_FACTORY_RESET_GROUP);
	CHECK_EQ(V_FR_FIRST, S03_FACTORY_RESET_GROUP * S03_GROUP_SECTORS);
	CHECK_EQ(V_FR_FIRST, s03_fat_volume_to_user(0));
	CHECK_EQ(V_FR_LAST, V_FR_FIRST + S03_GROUP_SECTORS - 1u);
	CHECK_EQ(s03_user_region(V_FR_FIRST), S03_REGION_FAT);
}

int main(void)
{
	test_headers();
	test_load_table();
	test_load_failures();
	test_write_table();
	test_watched_is_the_write();
	test_cache();
	test_erased_ff();
	test_features();
	test_power_off_errors();
	test_emmc_and_user_area();
	return finish("test_s03_emmc");
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-05 OS hand-off.
 * Requirements: S-05.001, .002, .005-.009, .012, .013, .018-.021, .024
 * (and S-01.017 through it); S-05.023, and on the OP-1F .036, .039, .040.
 * Vectors: tests/oracle/S-05-os-handoff boot_decision.tsv, vectors/compatibility_cases.json
 * and its .ldr streams (read at run time), synthetic_os_a/b.json,
 * boot_config.json (vectors/s05_vectors.h); scenarios H-DEC,
 * H-CFG, H-CALLS, H-ENTRY, H-ORDER, F-NOOS, F-ENC, F-REFUSE, F-SELECT, SL-LOAD, SL-NOOS;
 * os_destinations_1f.json, entry_state_1f.json; H-ORDER-1F, H-NOOS-1F.
 */
#include <stdio.h>
#include <string.h>

#include "waffle/s05_handoff.h"
#include "waffle/s01_memmap.h"
#include "s02_check.h"
#include "s02_ref_crc.h"
#include "vectors/s05_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define VEC_DIR "tests/oracle/S-05-os-handoff/vectors/"

static uint8_t g_img[S01_STAGING_MAX];

static uint32_t read_ldr(const char *name)
{
	char path[256];
	FILE *f;
	size_t n;

	snprintf(path, sizeof path, "%s%s", VEC_DIR, name);
	f = fopen(path, "rb");
	if (f == NULL) {
		CHECK(f != NULL);
		printf("  cannot open %s (run from the repository root)\n", path);
		return 0;
	}
	n = fread(g_img, 1, sizeof g_img, f);
	fclose(f);
	return (uint32_t)n;
}

/* S-05.001, S-05.002 */
static void test_decision(void)
{
	unsigned i;

	for (i = 0; i < N(v_decisions); i++) {
		const struct v_dec *v = &v_decisions[i];
		struct s05_decision d;
		uint16_t reason = v->reason < 0 ? 20u : (uint16_t)v->reason;

		s05_boot_decision(reason, v->state, (enum s05_keys)v->keys, true, &d);
		CHECK_EQ(d.boot_os, (unsigned)v->boot);
		/* S-11.080: the state alone says an install was interrupted,
		 * whatever the keys and the slave; boot_os as it was. */
		CHECK_EQ(d.interrupted, v->cleared == 1);
		CHECK_EQ(v->cleared == 1, v->state == 0xAB0Du);
		s05_boot_decision(reason, v->state, (enum s05_keys)v->keys, false, &d);
		CHECK_EQ(d.boot_os, (unsigned)v->boot);
		CHECK_EQ(d.interrupted, v->cleared == 1);
	}
}

/* S-05.024 on every compatibility case */
static void test_compatibility(void)
{
	unsigned i;

	for (i = 0; i < N(v_compat); i++) {
		uint32_t len = read_ldr(v_compat[i].image);

		CHECK_EQ(s05_check_image(g_img, len), v_compat[i].reason);
	}
	/* Longer than the staging area. */
	CHECK_EQ(s05_check_image(g_img, S01_STAGING_MAX + 1u), S05_REFUSE_LENGTH);
	/* A stream truncated before its FINAL block. */
	{
		uint32_t len = read_ldr("synthetic_os_b.ldr");

		CHECK_EQ(s05_check_image(g_img, len), S05_ACCEPT);
		CHECK_EQ(s05_check_image(g_img, len - 1u), S05_REFUSE_PARSE);
		CHECK_EQ(s05_check_image(g_img, 15u), S05_REFUSE_PARSE);
	}
}

/* --- S-05.008, S-05.009, S-05.012, S-05.013: a kernel model drives the load function --- */

struct kmem {
	uint32_t dest[64], count[64], crc[64];
	unsigned n;
	uint8_t header[S05_LDR_HEADER];
	uint32_t header_buffer;
	struct v_call calls[64];
	unsigned ncalls;
};

static void copy(void *ctx, uint32_t dest, const uint8_t *src, uint32_t count)
{
	struct kmem *k = ctx;

	if (dest == k->header_buffer && count == S05_LDR_HEADER) {
		memcpy(k->header, src, S05_LDR_HEADER);
		return;
	}
	if (k->n < N(k->dest)) {
		k->dest[k->n] = dest;
		k->count[k->n] = count;
		k->crc[k->n] = ref_crc32(0, src, count);
		k->n++;
	}
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* The kernel, as kernel_model.py: it moves nothing itself, the load function does. */
static uint32_t kernel(uint8_t cfg[S05_CONFIG_SIZE], const uint8_t *staging, struct kmem *k)
{
	k->header_buffer = get32(cfg + S05_CFG_PHEADER);
	for (;;) {
		struct s05_block b;

		put32(cfg + S05_CFG_PDEST, k->header_buffer);
		put32(cfg + S05_CFG_COUNT, S05_LDR_HEADER);
		k->calls[k->ncalls].psource = get32(cfg + S05_CFG_PSOURCE);
		k->calls[k->ncalls].dest = k->header_buffer;
		k->calls[k->ncalls++].count = S05_LDR_HEADER;
		s05_load_step(cfg, staging, copy, k);
		s05_ldr_decode(k->header, &b);
		if (b.flags & S05_FLAG_FILL) {
			if (b.count != 0u && k->n < N(k->dest)) {
				uint8_t w[4];
				uint32_t c = 0, j;

				put32(w, b.argument);
				for (j = 0; j < b.count; j++)
					c = ref_crc32(c, &w[j % 4u], 1);
				k->dest[k->n] = b.target;
				k->count[k->n] = b.count;
				k->crc[k->n++] = c;
			}
		} else if (!(b.flags & S05_FLAG_IGNORE) && b.count != 0u) {
			put32(cfg + S05_CFG_PDEST, b.target);
			put32(cfg + S05_CFG_COUNT, b.count);
			k->calls[k->ncalls].psource = get32(cfg + S05_CFG_PSOURCE);
			k->calls[k->ncalls].dest = b.target;
			k->calls[k->ncalls++].count = b.count;
			s05_load_step(cfg, staging, copy, k);
		}
		if ((b.flags & S05_FLAG_FINAL) || k->ncalls + 2u > N(k->calls))
			return b.target;	/* RCU0_SVECT0, as the model sets it */
	}
}

static void run_os(const char *file, const struct v_call *calls, unsigned ncalls,
		   const struct v_mem *mem, unsigned nmem, uint32_t len, uint32_t entry,
		   uint32_t hb, uint32_t final)
{
	static struct kmem k;
	uint8_t cfg[S05_CONFIG_SIZE];
	unsigned i, j;

	CHECK_EQ(read_ldr(file), len);
	CHECK_EQ(s05_check_image(g_img, len), S05_ACCEPT);
	memset(&k, 0, sizeof k);
	s05_config_build(cfg, hb, 0x0803D000u);
	CHECK_EQ(kernel(cfg, g_img, &k), entry);
	CHECK_EQ(get32(cfg + S05_CFG_PSOURCE), final);	/* pSource ends at the length */
	CHECK_EQ(k.ncalls, ncalls);
	for (i = 0; i < ncalls && i < k.ncalls; i++) {
		CHECK_EQ(k.calls[i].psource, calls[i].psource);
		CHECK_EQ(k.calls[i].dest, calls[i].dest);
		CHECK_EQ(k.calls[i].count, calls[i].count);
	}
	/* S-05.012: each destination holds its bytes (exact counts: 3, 5, 13 among them). */
	for (i = 0; i < nmem; i++) {
		bool found = false;

		for (j = 0; j < k.n; j++)
			if (k.dest[j] == mem[i].start && k.count[j] == mem[i].length)
				found = k.crc[j] == mem[i].crc32;
		CHECK(found);
	}
}

static void test_kernel_and_load_function(void)
{
	uint8_t cfg[S05_CONFIG_SIZE];
	unsigned i;

	CHECK_EQ(S05_CONFIG_SIZE, V_CONFIG_SIZE);
	CHECK_EQ(S05_CFG_PHEADER, V_PHEADER);
	CHECK_EQ(S05_CFG_PLOADFN, V_PLOADFN);
	CHECK_EQ(S05_CFG_PSOURCE, V_PSOURCE);
	CHECK_EQ(S05_CFG_PDEST, V_PDEST);
	CHECK_EQ(S05_CFG_COUNT, V_COUNT);
	CHECK_EQ(S01_ROM_BOOT_KERNEL, V_KERNEL);
	memset(cfg, 0xA5, sizeof cfg);
	s05_config_build(cfg, 0x88A34D5Cu, 0x0803C100u);
	for (i = 0; i < S05_CONFIG_SIZE; i++)
		if (i < S05_CFG_PHEADER || i >= S05_CFG_PLOADFN + 4u ||
		    (i >= S05_CFG_PHEADER + 4u && i < S05_CFG_PLOADFN))
			CHECK_EQ(cfg[i], 0);
	CHECK_EQ(get32(cfg + S05_CFG_PHEADER), 0x88A34D5Cu);
	CHECK_EQ(get32(cfg + S05_CFG_PLOADFN), 0x0803C100u);
	/* A count of 0 changes nothing. */
	put32(cfg + S05_CFG_PSOURCE, 0x40);
	put32(cfg + S05_CFG_COUNT, 0);
	s05_load_step(cfg, g_img, copy, NULL);
	CHECK_EQ(get32(cfg + S05_CFG_PSOURCE), 0x40u);

	run_os("synthetic_os_a.ldr", v_calls_a, N(v_calls_a), v_mem_a, N(v_mem_a), V_A_LEN,
	       V_A_ENTRY, V_A_HB, V_A_FINAL);
	run_os("synthetic_os_b.ldr", v_calls_b, N(v_calls_b), v_mem_b, N(v_mem_b), V_B_LEN,
	       V_B_ENTRY, V_B_HB, V_B_FINAL);
}

/* --- S-05.005-.007, S-05.018-.021: the sequences with recording ops --- */

static char g_ev[64];
static unsigned g_nev;
static enum s05_fail g_stage_result, g_held_cause;
static int g_release_rc;
static uint8_t g_reply_value;
static uint32_t g_jumped, g_len;

static void ev(char c)
{
	if (g_nev + 1u < sizeof g_ev)
		g_ev[g_nev++] = c;
	g_ev[g_nev] = '\0';
}

static void m_pre(void *c) { (void)c; ev('P'); }
static enum s05_fail m_stage(void *c, uint32_t *len) { (void)c; ev('S'); *len = g_len; return g_stage_result; }
static int m_release(void *c, uint8_t *reply) { (void)c; ev('R'); *reply = g_reply_value; return g_release_rc; }
static void m_teardown(void *c) { (void)c; ev('T'); }
static void m_kernel(void *c, uint32_t a) { (void)c; (void)a; ev('K'); }
static uint32_t m_svect(void *c) { (void)c; ev('V'); return 0x11A00000u; }
static void m_jump(void *c, uint32_t e) { (void)c; g_jumped = e; ev('J'); }
static void m_clear(void *c) { (void)c; ev('C'); }
static void m_held(void *c, enum s05_fail f) { (void)c; g_held_cause = f; ev('H'); }
static void s_reply(void *c, uint8_t v) { (void)c; ev(v == 0u ? '0' : '1'); }
static void s_stop(void *c) { (void)c; ev('X'); }

static void m_accepted(void *c) { (void)c; ev('A'); }

static const struct s05_master_ops g_mops = {
	NULL, m_pre, m_stage, m_release, m_teardown, m_kernel, m_svect, m_jump, m_clear, m_held,
	m_accepted,
};
static const struct s05_master_ops g_mops_bare = {
	NULL, m_pre, m_stage, m_release, m_teardown, m_kernel, m_svect, m_jump, m_clear, m_held,
	NULL,
};
static const struct s05_slave_ops g_sops = {
	NULL, m_stage, s_reply, s_stop, m_teardown, m_kernel, m_svect, m_jump,
};

static void test_sequences(void)
{
	static const struct {
		enum s05_fail stage;
		const char *file;
		int release_rc;
		uint8_t reply;
		bool sw_nonzero;
		const char *seq;
		enum s05_fail cause;
		bool from_file;		/* S-05.035: a file start */
	} cases[] = {
		{S05_FAIL_NONE, "synthetic_os_b.ldr", 0, 0, true, "PSRTKVJ", S05_FAIL_NONE, false},	/* H-ORDER */
		{S05_FAIL_NO_OS, NULL, 0, 0, true, "PSCH", S05_FAIL_NO_OS, false},		/* F-NOOS, F-BADCRC */
		{S05_FAIL_NO_OS, NULL, 0, 0, false, "PSH", S05_FAIL_NO_OS, false},		/* M6 not needed */
		{S05_FAIL_ENCRYPTED, NULL, 0, 0, true, "PSCH", S05_FAIL_ENCRYPTED, false},	/* F-ENC */
		{S05_FAIL_SELECT, NULL, 0, 0, true, "PSH", S05_FAIL_SELECT, false},		/* F-SELECT: no M6 */
		{S05_FAIL_NONE, "refuse_ddr_reserve.ldr", 0, 0, true, "PSCH", S05_FAIL_REFUSED, false},	/* F-REFUSE */
		{S05_FAIL_NONE, "synthetic_os_b.ldr", 0, 1, true, "PSRH", S05_FAIL_SLAVE, false},	/* reply 1 */
		{S05_FAIL_NONE, "synthetic_os_b.ldr", -1, 0, true, "PSRH", S05_FAIL_SLAVE, false},	/* no reply */
		{S05_FAIL_NONE, "synthetic_os_b.ldr", 0, S05_REPLY_NO_START, true, "PSRH",
		 S05_FAIL_SLAVE_START, false},	/* H-OSSTART: no teardown, no kernel call */
		{S05_FAIL_NONE, "synthetic_os_b.ldr", -1, S05_REPLY_NO_START, true, "PSRH",
		 S05_FAIL_SLAVE, false},	/* no reply: what the op left is not read */
		/* S-05.035: a file start's failures clear nothing (no M6) */
		{S05_FAIL_FILE, NULL, 0, 0, true, "PSH", S05_FAIL_FILE, true},
		{S05_FAIL_NO_OS, NULL, 0, 0, true, "PSH", S05_FAIL_NO_OS, true},
		{S05_FAIL_NONE, "refuse_ddr_reserve.ldr", 0, 0, true, "PSH", S05_FAIL_REFUSED, true},
		{S05_FAIL_NONE, "synthetic_os_b.ldr", 0, 1, true, "PSRH", S05_FAIL_SLAVE, true},
		{S05_FAIL_NONE, "synthetic_os_b.ldr", 0, 0, true, "PSRTKVJ", S05_FAIL_NONE, true},
	};
	unsigned i;

	for (i = 0; i < N(cases); i++) {
		struct s05_master m;

		g_nev = 0;
		g_ev[0] = '\0';
		g_stage_result = cases[i].stage;
		g_len = cases[i].file != NULL ? read_ldr(cases[i].file) : 0u;
		g_release_rc = cases[i].release_rc;
		g_reply_value = cases[i].reply;
		g_held_cause = S05_FAIL_NONE;
		g_jumped = 0;
		m.staging = g_img;
		m.config_addr = 0x0803D000u;
		m.sw_version_nonzero = cases[i].sw_nonzero;
		m.file = cases[i].from_file;
		m.dev = &wb_device_opxy;
		s05_master_handoff(&g_mops, &m);
		CHECK(strcmp(g_ev, cases[i].seq) == 0);
		CHECK_EQ(g_held_cause, cases[i].cause);
		if (cases[i].seq[g_nev - 1u] == 'J')
			CHECK_EQ(g_jumped, 0x11A00000u);	/* S-05.013: what the ROM left */
	}
	/* The slave (S-05.020, S-05.021). */
	g_nev = 0;
	g_ev[0] = '\0';
	g_stage_result = S05_FAIL_NONE;
	g_len = read_ldr("synthetic_os_a.ldr");
	CHECK(s05_slave_release(&g_sops, g_img, 0x0803D000u));
	CHECK(strcmp(g_ev, "S0XTKVJ") == 0);
	g_nev = 0;
	g_ev[0] = '\0';
	g_stage_result = S05_FAIL_NO_OS;
	CHECK(!s05_slave_release(&g_sops, g_img, 0x0803D000u));
	CHECK(strcmp(g_ev, "S1") == 0);
	g_nev = 0;
	g_ev[0] = '\0';
	g_stage_result = S05_FAIL_NONE;
	g_len = read_ldr("refuse_flag_init.ldr");
	CHECK(!s05_slave_release(&g_sops, g_img, 0x0803D000u));
	CHECK(strcmp(g_ev, "S1") == 0);
}

/*
 * S-05.036 on the OP-1F: no release, no wait; the accepted op, the
 * teardown, the kernel call; a failed load as on the OP-XY (H-ORDER-1F,
 * H-NOOS-1F).
 */
static void test_sequences_1f(void)
{
	static const struct {
		enum s05_fail stage;
		const char *file;
		bool sw_nonzero, from_file, bare;
		const char *seq;
		enum s05_fail cause;
	} cases[] = {
		{S05_FAIL_NONE, "synthetic_os_b.ldr", true, false, false, "PSATKVJ", S05_FAIL_NONE},
		{S05_FAIL_NONE, "synthetic_os_b.ldr", true, false, true, "PSTKVJ", S05_FAIL_NONE},
		{S05_FAIL_NO_OS, NULL, true, false, false, "PSCH", S05_FAIL_NO_OS},
		{S05_FAIL_SELECT, NULL, true, false, false, "PSH", S05_FAIL_SELECT},
		{S05_FAIL_NONE, "refuse_ddr_reserve.ldr", true, false, false, "PSCH", S05_FAIL_REFUSED},
		{S05_FAIL_FILE, NULL, true, true, false, "PSH", S05_FAIL_FILE},
		{S05_FAIL_NONE, "synthetic_os_b.ldr", true, true, false, "PSATKVJ", S05_FAIL_NONE},
	};
	unsigned i;

	for (i = 0; i < N(cases); i++) {
		struct s05_master m;

		g_nev = 0;
		g_ev[0] = '\0';
		g_stage_result = cases[i].stage;
		g_len = cases[i].file != NULL ? read_ldr(cases[i].file) : 0u;
		g_release_rc = 0;
		g_reply_value = 1;	/* a release would hold: none is made */
		g_held_cause = S05_FAIL_NONE;
		m.staging = g_img;
		m.config_addr = 0x0803D000u;
		m.sw_version_nonzero = cases[i].sw_nonzero;
		m.file = cases[i].from_file;
		m.dev = &wb_device_op1f;
		s05_master_handoff(cases[i].bare ? &g_mops_bare : &g_mops, &m);
		CHECK(strcmp(g_ev, cases[i].seq) == 0);
		CHECK(strchr(g_ev, 'R') == NULL);
		CHECK_EQ(g_held_cause, cases[i].cause);
	}
}

/*
 * S-05.045: a RAM start from held mode: the steps of a start, but no
 * accepted op and no software-version reset; a failure comes back with
 * its cause and never enters held mode again.
 */
static void test_ram_start(void)
{
	static const struct {
		const struct wb_device *dev;
		enum s05_fail stage;
		const char *file;
		int release_rc;
		uint8_t reply;
		const char *seq;
		enum s05_fail cause;
	} cases[] = {
		{&wb_device_opxy, S05_FAIL_NONE, "synthetic_os_b.ldr", 0, 0, "PSRTKVJ", S05_FAIL_NONE},
		{&wb_device_opxy, S05_FAIL_NO_OS, NULL, 0, 0, "PS", S05_FAIL_NO_OS},
		{&wb_device_opxy, S05_FAIL_NONE, "refuse_ddr_reserve.ldr", 0, 0, "PS", S05_FAIL_REFUSED},
		{&wb_device_opxy, S05_FAIL_NONE, "synthetic_os_b.ldr", 0, 1, "PSR", S05_FAIL_SLAVE},
		{&wb_device_opxy, S05_FAIL_NONE, "synthetic_os_b.ldr", -1, 0, "PSR", S05_FAIL_SLAVE},
		{&wb_device_opxy, S05_FAIL_NONE, "synthetic_os_b.ldr", 0, S05_REPLY_NO_START, "PSR",
		 S05_FAIL_SLAVE_START},
		{&wb_device_op1f, S05_FAIL_NONE, "synthetic_os_b.ldr", 0, 1, "PSTKVJ", S05_FAIL_NONE},
		{&wb_device_op1f, S05_FAIL_NONE, "refuse_ddr_reserve.ldr", 0, 1, "PS", S05_FAIL_REFUSED},
	};
	unsigned i;

	for (i = 0; i < N(cases); i++) {
		struct s05_master m;
		enum s05_fail f;

		g_nev = 0;
		g_ev[0] = '\0';
		g_stage_result = cases[i].stage;
		g_len = cases[i].file != NULL ? read_ldr(cases[i].file) : 0u;
		g_release_rc = cases[i].release_rc;
		g_reply_value = cases[i].reply;
		g_held_cause = S05_FAIL_NONE;
		m.staging = g_img;
		m.config_addr = 0x0803D000u;
		m.sw_version_nonzero = true;	/* never cleared by a RAM start */
		m.file = false;
		m.dev = cases[i].dev;
		f = s05_master_ram_start(&g_mops, &m);
		CHECK(strcmp(g_ev, cases[i].seq) == 0);
		CHECK_EQ(f, cases[i].cause);
		CHECK(strchr(g_ev, 'A') == NULL && strchr(g_ev, 'C') == NULL &&
		      strchr(g_ev, 'H') == NULL);
	}
}

/* S-05.023, S-05.039: the contract per device */
static void test_contract(void)
{
	CHECK(strcmp(S05_CONTRACT_VERSION, "XY-HANDOFF-2") == 0);
	CHECK(strcmp(s05_contract(&wb_device_opxy), S05_CONTRACT_VERSION) == 0);
	CHECK(strcmp(s05_contract(&wb_device_op1f), "1F-HANDOFF-1") == 0);
}

/* S-05.040: OS 1.7.3's destinations pass S-05.024's check (vectors:
 * os_destinations_1f.json); the second DDR range starts above the display
 * frame memory's 480 rows. */
static void test_os_destinations_1f(void)
{
	uint32_t off = 0;
	unsigned n = 0, i;
	struct s05_block b;

	CHECK_EQ(s05_check_image(v_os1f_stream, sizeof v_os1f_stream),
		 V_OS1F_ACCEPTED ? S05_ACCEPT : S05_REFUSE_SPACE);
	CHECK_EQ(s05_check_image(v_os1f_stream, sizeof v_os1f_stream), S05_ACCEPT);
	while (s05_ldr_next(v_os1f_stream, sizeof v_os1f_stream, &off, &b) == 0) {
		if (s05_block_has_destination(&b)) {
			CHECK(n < N(v_os1f_ranges));
			if (n < N(v_os1f_ranges)) {
				CHECK_EQ(b.target, v_os1f_ranges[n].start);
				CHECK_EQ(b.target + b.count, v_os1f_ranges[n].end);
			}
			n++;
		}
		if (b.flags & S05_FLAG_FINAL)
			break;
	}
	CHECK_EQ(n, N(v_os1f_ranges));
	for (i = 0; i < N(v_os1f_ranges); i++)
		CHECK_EQ(s01_os_destination(v_os1f_ranges[i].start,
					    v_os1f_ranges[i].end - v_os1f_ranges[i].start), S01_OS_OK);
	CHECK(v_os1f_ranges[N(v_os1f_ranges) - 1u].start > 0x8C03C000u);
	/* S-05.038: the vectors' table, for the board's entry */
	CHECK_EQ(v_entry1f_evt[2], 0x11A00C18u);
	CHECK_EQ(v_entry1f_evt[15], 0x11A000CAu);
}

int main(void)
{
	test_decision();
	test_compatibility();
	test_kernel_and_load_function();
	test_sequences();
	test_sequences_1f();
	test_ram_start();
	test_contract();
	test_os_destinations_1f();
	return finish("test_s05_handoff");
}

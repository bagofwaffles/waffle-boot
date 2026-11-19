/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The boot flow (src/boot_flow.c) over fake steps: the master through
 * every row of S-05's boot decision table (boot_decision.tsv) with the
 * slave present and absent, the load and each of its failures, the DDR
 * check, held-mode entry, the boot prompt (S-05.025), the setting to start
 * in the boot menu (S-15.175), and the slave's boot and release.
 */
#include <string.h>

#include "support/check.h"

#include "waffle/boot.h"
#include "waffle/s04_records.h"
#include "vectors/s05_vectors.h"

/* Each step appends its letter, so a run's order reads as a string. */
static char trace[64];
static size_t ntrace;

static void step(char c)
{
	if (ntrace + 1u < sizeof(trace))
		trace[ntrace++] = c;
	trace[ntrace] = '\0';
}

/* --- An LDR stream the hand-off accepts --- */

static uint8_t staging[64];
static uint32_t staged_len;

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* One block header; the checksum makes the 16 bytes XOR to zero. */
static void ldr_block(uint8_t *h, uint16_t flags, uint32_t target,
		      uint32_t count, uint32_t arg)
{
	uint8_t x = 0;
	unsigned i;

	put32(h, 0xAD000000u | flags | 1u);
	put32(h + 4, target);
	put32(h + 8, count);
	put32(h + 12, arg);
	for (i = 0; i < 16u; i++)
		x ^= h[i];
	h[2] = x;
}

static void stage_image(uint16_t flags)
{
	memset(staging, 0, sizeof(staging));
	ldr_block(staging, (uint16_t)(S05_FLAG_FIRST | S05_FLAG_IGNORE),
		  0x11A00000u, 0u, 32u);
	ldr_block(staging + 16, (uint16_t)(S05_FLAG_FINAL | flags),
		  0x11A00000u, 0u, 0u);
	staged_len = 32u;
}

/* --- The master --- */

struct master {
	bool ddr_ok;
	uint32_t state;
	bool nonzero;
	uint16_t reason;
	bool present;
	enum s05_keys keys;
	enum s05_fail stage;
	int release_rc;
	uint8_t reply;
	/* what held mode was told */
	bool held;
	struct boot_held h;
	unsigned meta_writes;
	int prompt;		/* the prompt op's answer */
	bool start_menu;	/* the start_in_menu op's answer (S-15.175) */
	unsigned menu_asks;	/* its calls */
};

static struct master mf;
static const struct boot_master_ops master_ops;

static void m_bringup(void *ctx) { (void)ctx; step('B'); }
static bool m_ddr(void *ctx) { (void)ctx; step('D'); return mf.ddr_ok; }
static void m_fatal(void *ctx) { (void)ctx; step('F'); }

static void m_meta(void *ctx, uint32_t *state, bool *nonzero)
{
	(void)ctx;
	step('M');
	*state = mf.state;
	*nonzero = mf.nonzero;
}

static uint16_t m_reason(void *ctx) { (void)ctx; step('R'); return mf.reason; }
static bool m_slave(void *ctx) { (void)ctx; step('S'); return mf.present; }
static enum s05_keys m_psoc(void *ctx) { (void)ctx; step('K'); return mf.keys; }

static void m_held(void *ctx, const struct boot_held *h)
{
	(void)ctx;
	step('H');
	mf.held = true;
	mf.h = *h;
}

static void h_pre(void *ctx) { (void)ctx; step('P'); }

static enum s05_fail h_stage(void *ctx, uint32_t *length)
{
	(void)ctx;
	step('G');
	*length = staged_len;
	return mf.stage;
}

static int h_release(void *ctx, uint8_t *reply)
{
	(void)ctx;
	step('L');
	*reply = mf.reply;
	return mf.release_rc;
}

static void h_teardown(void *ctx) { (void)ctx; step('T'); }
static void h_kernel(void *ctx, uint32_t a) { (void)ctx; (void)a; step('k'); }
static uint32_t h_svect(void *ctx) { (void)ctx; step('V'); return 0x11A00000u; }
static void h_jump(void *ctx, uint32_t e) { (void)ctx; (void)e; step('J'); }

static void h_clear(void *ctx)
{
	(void)ctx;
	step('Z');
	mf.meta_writes++;
}

/* The board's account of the failed load: a marked one, kept whole. */
static struct boot_start account;

static void h_held(void *ctx, enum s05_fail cause)
{
	(void)ctx;
	memset(&account, 0, sizeof(account));
	account.load.result = S03_LOAD_IO;
	account.load.step = S03_STEP_IMAGE;
	account.load.sector = 1281u;
	account.lost = true;
	account.why.cmd = S09_READ_SECTORS;
	account.why.fault[3] = 0xA5u;
	boot_master_load_failed(&master_ops, cause, &account);
}

static const struct s05_master_ops handoff_ops = {
	NULL, h_pre, h_stage, h_release, h_teardown, h_kernel, h_svect,
	h_jump, h_clear, h_held,
};

static const struct boot_master_ops master_ops = {
	NULL, m_bringup, m_ddr, m_fatal, m_meta, m_reason,
	m_psoc, m_slave, &handoff_ops, staging, 0x0803D000u, m_held, NULL,
	NULL,
};

/* S-05.025: a build with the boot prompt. */
static int m_prompt(void *ctx) { (void)ctx; step('Q'); return mf.prompt; }

static const struct boot_master_ops prompt_ops = {
	NULL, m_bringup, m_ddr, m_fatal, m_meta, m_reason,
	m_psoc, m_slave, &handoff_ops, staging, 0x0803D000u, m_held,
	m_prompt, NULL,
};

/* S-15.175: the boot store's setting, with and without the prompt. */
static bool m_menu(void *ctx)
{
	(void)ctx;
	step('U');
	mf.menu_asks++;
	return mf.start_menu;
}

static const struct boot_master_ops menu_ops = {
	NULL, m_bringup, m_ddr, m_fatal, m_meta, m_reason,
	m_psoc, m_slave, &handoff_ops, staging, 0x0803D000u, m_held, NULL,
	m_menu,
};

static const struct boot_master_ops menu_prompt_ops = {
	NULL, m_bringup, m_ddr, m_fatal, m_meta, m_reason,
	m_psoc, m_slave, &handoff_ops, staging, 0x0803D000u, m_held,
	m_prompt, m_menu,
};

static void master_reset(void)
{
	memset(&mf, 0, sizeof(mf));
	mf.ddr_ok = true;
	mf.present = true;
	mf.reason = BOOT_REASON_NONE;
	mf.keys = S05_KEYS_NOT_HELD;
	ntrace = 0;
	trace[0] = '\0';
	stage_image(0);
}

/* Every row of boot_decision.tsv, slave present and absent. */
static void test_decision_table(void)
{
	size_t i;
	int absent;

	for (i = 0; i < ARRAY_LEN(v_decisions); i++) {
		const struct v_dec *v = &v_decisions[i];

		for (absent = 0; absent < 2; absent++) {
			master_reset();
			mf.reason = v->reason < 0 ? BOOT_REASON_NONE :
						    (uint16_t)v->reason;
			mf.state = v->state;
			mf.keys = (enum s05_keys)v->keys;
			mf.present = absent == 0;
			mf.nonzero = true;
			boot_master(&master_ops);
			if (v->boot && !absent) {
				CHECK(strcmp(trace, "BDMRKSPGLTkVJ") == 0);
				CHECK(!mf.held);
			} else {
				/* No load: no PMIC step, no staging, no write. */
				CHECK(strcmp(trace, "BDMRKSH") == 0);
				CHECK(mf.held && !mf.h.load_attempted);
				CHECK(mf.h.slave_absent == (absent != 0));
				/* S-11.080: whatever the keys and the slave */
				CHECK(mf.h.interrupted == (v->cleared == 1));
				CHECK(mf.h.reason == mf.reason);
			}
			/* S-05.002, S-09.008: no metadata write. */
			CHECK(mf.meta_writes == 0u);
		}
	}
}

static void test_ddr_check(void)
{
	master_reset();
	mf.ddr_ok = false;
	boot_master(&master_ops);
	/* S-13a.021: before the decision; no slave stream, no PSoC. */
	CHECK(strcmp(trace, "BDF") == 0);
	CHECK(!mf.held);
}

/* S-05.006, S-05.007, S-05.024: each failed load, with and without a
 * nonzero software version. */
static void test_load_failures(void)
{
	static const enum s05_fail fails[] = {
		S05_FAIL_NO_OS, S05_FAIL_ENCRYPTED, S05_FAIL_SELECT,
	};
	size_t i;
	int nz;

	for (i = 0; i < ARRAY_LEN(fails); i++) {
		for (nz = 0; nz < 2; nz++) {
			master_reset();
			mf.stage = fails[i];
			mf.nonzero = nz != 0;
			boot_master(&master_ops);
			/* S-05.007: a failed select keeps the version. */
			CHECK(strcmp(trace, nz && fails[i] != S05_FAIL_SELECT ?
					    "BDMRKSPGZH" : "BDMRKSPGH") == 0);
			CHECK(mf.held && mf.h.load_attempted);
			CHECK(mf.h.cause == fails[i] && !mf.h.interrupted);
			/* The board's account reaches held mode whole. */
			CHECK(mf.h.start.load.result == S03_LOAD_IO &&
			      mf.h.start.load.step == S03_STEP_IMAGE &&
			      mf.h.start.load.sector == 1281u && mf.h.start.lost &&
			      mf.h.start.why.cmd == S09_READ_SECTORS &&
			      mf.h.start.why.fault[3] == 0xA5u);
		}
	}
	/* A boot after it that holds the keys attempts nothing and keeps no
	 * account (S-15.163 shows no notice). */
	master_reset();
	mf.keys = S05_KEYS_HELD;
	boot_master(&master_ops);
	CHECK(mf.held && !mf.h.load_attempted && mf.h.cause == S05_FAIL_NONE);
	CHECK(!mf.h.start.lost && mf.h.start.load.result == S03_LOAD_OK &&
	      mf.h.start.load.sector == 0u && mf.h.start.why.fault[3] == 0u);

	/* S-05.024: a staged image with an INIT block is refused. */
	master_reset();
	stage_image(S05_FLAG_INIT);
	mf.nonzero = true;
	boot_master(&master_ops);
	CHECK(strcmp(trace, "BDMRKSPGZH") == 0);
	CHECK(mf.h.cause == S05_FAIL_REFUSED);

	/* S-05.021: the slave answers 1; held, no metadata write. */
	master_reset();
	mf.reply = 1u;
	mf.nonzero = true;
	boot_master(&master_ops);
	CHECK(strcmp(trace, "BDMRKSPGLH") == 0);
	CHECK(mf.h.cause == S05_FAIL_SLAVE && mf.meta_writes == 0u);

	/* S-05.035: a file start's failures, its refusal included, say
	 * nothing of the installed OS: no metadata write (S-04.045 M6). */
	for (i = 0; i < ARRAY_LEN(fails); i++) {
		master_reset();
		mf.reason = S04_REASON_OS_FILE;
		mf.stage = fails[i];
		mf.nonzero = true;
		boot_master(&master_ops);
		CHECK(strcmp(trace, "BDMRKSPGH") == 0 && mf.meta_writes == 0u);
		CHECK(mf.h.load_attempted && mf.h.cause == fails[i]);
	}
	master_reset();
	mf.reason = S04_REASON_OS_FILE;
	mf.stage = S05_FAIL_FILE;
	mf.nonzero = true;
	boot_master(&master_ops);
	CHECK(strcmp(trace, "BDMRKSPGH") == 0 && mf.h.cause == S05_FAIL_FILE &&
	      mf.meta_writes == 0u);
	master_reset();
	mf.reason = S04_REASON_OS_FILE;
	stage_image(S05_FLAG_INIT);
	mf.nonzero = true;
	boot_master(&master_ops);
	CHECK(strcmp(trace, "BDMRKSPGH") == 0 && mf.h.cause == S05_FAIL_REFUSED &&
	      mf.meta_writes == 0u);
	/* ... and a start from the file that goes through hands over */
	master_reset();
	mf.reason = S04_REASON_OS_FILE;
	boot_master(&master_ops);
	CHECK(strcmp(trace, "BDMRKSPGLTkVJ") == 0 && !mf.held);

	/* S-09.052: no reply within T_RELEASE. */
	master_reset();
	mf.release_rc = -1;
	boot_master(&master_ops);
	CHECK(strcmp(trace, "BDMRKSPGLH") == 0);
	CHECK(mf.h.cause == S05_FAIL_SLAVE);
}

/*
 * S-05.025: the prompt is asked only on a decision to boot with the slave
 * present and outside a Power + M4 session, after the slave's start and
 * before the hand-off's steps. Digit 1 goes on to the hand-off unchanged;
 * digit 2 is held mode as a boot that attempted no load, with no cause and
 * no metadata write. The op left NULL changes nothing (test_decision_table).
 */
static void test_boot_prompt(void)
{
	size_t i;

	master_reset();
	mf.prompt = BOOT_PROMPT_START;
	boot_master(&prompt_ops);
	CHECK(strcmp(trace, "BDMRKSQPGLTkVJ") == 0);
	CHECK(!mf.held && mf.meta_writes == 0u);

	master_reset();
	mf.prompt = BOOT_PROMPT_HELD;
	mf.nonzero = true;
	boot_master(&prompt_ops);
	CHECK(strcmp(trace, "BDMRKSQH") == 0);
	CHECK(mf.held && !mf.h.load_attempted && mf.h.cause == S05_FAIL_NONE);
	CHECK(!mf.h.interrupted && !mf.h.slave_absent);
	CHECK(mf.h.reason == BOOT_REASON_NONE && mf.meta_writes == 0u);

	/* S-05.006 after digit 1: a failed load is still a failed load. */
	master_reset();
	mf.prompt = BOOT_PROMPT_START;
	mf.stage = S05_FAIL_NO_OS;
	mf.nonzero = true;
	boot_master(&prompt_ops);
	CHECK(strcmp(trace, "BDMRKSQPGZH") == 0);
	CHECK(mf.h.load_attempted && mf.h.cause == S05_FAIL_NO_OS);

	/* No prompt: the slave absent, and every row whose decision is not
	 * to boot (held keys, an interrupted install). Power + M4 never gets here: the
	 * front runs recovery instead (S-02.084). */
	master_reset();
	mf.prompt = BOOT_PROMPT_START;
	mf.present = false;
	boot_master(&prompt_ops);
	CHECK(strcmp(trace, "BDMRKSH") == 0 && mf.h.slave_absent);
	for (i = 0; i < ARRAY_LEN(v_decisions); i++) {
		const struct v_dec *v = &v_decisions[i];

		if (v->boot)
			continue;
		master_reset();
		mf.prompt = BOOT_PROMPT_START;
		mf.reason = v->reason < 0 ? BOOT_REASON_NONE :
					    (uint16_t)v->reason;
		mf.state = v->state;
		mf.keys = (enum s05_keys)v->keys;
		boot_master(&prompt_ops);
		CHECK(strcmp(trace, "BDMRKSH") == 0);
		CHECK(mf.h.interrupted == (v->cleared == 1));
	}
}

/*
 * S-15.175, S-05.001: the setting is asked only on a decision to boot with
 * the slave present, once, after the slave's start and before the boot
 * prompt. On: held mode as a boot that attempted no load (no cause, no
 * account, no metadata write, no prompt). Off: the boot as before. Every
 * other start (each reason, held keys, a silent PSoC, an interrupted
 * install, an absent slave) never asks it.
 */
static void test_start_in_menu(void)
{
	size_t i;
	int absent, on;

	for (i = 0; i < ARRAY_LEN(v_decisions); i++) {
		const struct v_dec *v = &v_decisions[i];

		for (absent = 0; absent < 2; absent++) {
			for (on = 0; on < 2; on++) {
				master_reset();
				mf.reason = v->reason < 0 ? BOOT_REASON_NONE :
							    (uint16_t)v->reason;
				mf.state = v->state;
				mf.keys = (enum s05_keys)v->keys;
				mf.present = absent == 0;
				mf.nonzero = true;
				mf.start_menu = on != 0;
				boot_master(&menu_ops);
				if (v->boot && !absent && on) {
					CHECK(strcmp(trace, "BDMRKSUH") == 0);
					CHECK(mf.held && !mf.h.load_attempted &&
					      mf.h.cause == S05_FAIL_NONE);
					CHECK(!mf.h.slave_absent && !mf.h.interrupted);
					CHECK(mf.h.reason == mf.reason);
					CHECK(!mf.h.start.lost &&
					      mf.h.start.load.result == S03_LOAD_OK);
				} else if (v->boot && !absent) {
					CHECK(strcmp(trace, "BDMRKSUPGLTkVJ") == 0);
					CHECK(!mf.held);
				} else {
					CHECK(strcmp(trace, "BDMRKSH") == 0);
					CHECK(mf.menu_asks == 0u);
					CHECK(mf.h.slave_absent == (absent != 0));
					CHECK(mf.h.interrupted == (v->cleared == 1));
				}
				CHECK(mf.menu_asks <= 1u && mf.meta_writes == 0u);
			}
		}
	}

	/* S-04.013: the start after Continue to the OS never asks it, and
	 * boots with it on; held keys still hold. */
	master_reset();
	mf.reason = S04_REASON_OS_ONCE;
	mf.start_menu = true;
	boot_master(&menu_ops);
	CHECK(strcmp(trace, "BDMRKSPGLTkVJ") == 0 && !mf.held && mf.menu_asks == 0u);
	master_reset();
	mf.reason = S04_REASON_OS_ONCE;
	mf.start_menu = true;
	mf.keys = S05_KEYS_HELD;
	boot_master(&menu_ops);
	CHECK(strcmp(trace, "BDMRKSH") == 0 && mf.held && mf.menu_asks == 0u);
	master_reset();
	mf.reason = S04_REASON_OS_ONCE;
	mf.start_menu = true;
	mf.prompt = BOOT_PROMPT_START;
	boot_master(&menu_prompt_ops);
	CHECK(strcmp(trace, "BDMRKSQPGLTkVJ") == 0 && mf.menu_asks == 0u);

	/* S-04.014: the start after Start /os.ldr once asks it as any other
	 * start does: on, held mode with no load; off, the boot path. */
	master_reset();
	mf.reason = S04_REASON_OS_FILE;
	mf.start_menu = true;
	boot_master(&menu_ops);
	CHECK(strcmp(trace, "BDMRKSUH") == 0 && mf.held && !mf.h.load_attempted &&
	      mf.h.reason == S04_REASON_OS_FILE);
	master_reset();
	mf.reason = S04_REASON_OS_FILE;
	boot_master(&menu_ops);
	CHECK(strcmp(trace, "BDMRKSUPGLTkVJ") == 0 && !mf.held);

	/* The prompt comes after it, and only with the setting off. */
	master_reset();
	mf.start_menu = true;
	mf.prompt = BOOT_PROMPT_START;
	boot_master(&menu_prompt_ops);
	CHECK(strcmp(trace, "BDMRKSUH") == 0 && !mf.h.load_attempted);
	master_reset();
	mf.prompt = BOOT_PROMPT_START;
	boot_master(&menu_prompt_ops);
	CHECK(strcmp(trace, "BDMRKSUQPGLTkVJ") == 0);
	master_reset();
	mf.prompt = BOOT_PROMPT_HELD;
	boot_master(&menu_prompt_ops);
	CHECK(strcmp(trace, "BDMRKSUQH") == 0 && !mf.h.load_attempted);

	/* Off, a failed load is still a failed load. */
	master_reset();
	mf.stage = S05_FAIL_NO_OS;
	mf.nonzero = true;
	boot_master(&menu_ops);
	CHECK(strcmp(trace, "BDMRKSUPGZH") == 0);
	CHECK(mf.h.load_attempted && mf.h.cause == S05_FAIL_NO_OS);
}

/* --- Held-mode entry --- */

static void e_greet(void *ctx) { (void)ctx; step('N'); }
static void e_info(void *ctx) { (void)ctx; step('I'); }
static void e_state(void *ctx) { (void)ctx; step('X'); }

static void e_service(void *ctx, const struct boot_held *h)
{
	(void)ctx;
	(void)h;
	step('Q');
}

static void test_held_entry(void)
{
	static const struct boot_held_ops ops = {
		NULL, e_greet, e_info, e_state, e_service,
	};
	struct boot_held h;

	memset(&h, 0, sizeof(h));
	ntrace = 0;
	boot_held_enter(&ops, &h);
	/* S-08.001, then S-07.023 step 8, then the service loop. */
	CHECK(strcmp(trace, "NIXQ") == 0);

	/* S-11.080: an interrupted install adds no step here. */
	h.interrupted = true;
	ntrace = 0;
	boot_held_enter(&ops, &h);
	CHECK(strcmp(trace, "NIXQ") == 0);
}

/* --- The slave --- */

static const uint8_t *serve_script;
static enum s05_fail slave_stage;

static void s_bringup(void *ctx) { (void)ctx; step('b'); }
static bool s_ddr(void *ctx) { (void)ctx; step('d'); return mf.ddr_ok; }
static void s_fatal(void *ctx) { (void)ctx; step('f'); }

static uint8_t s_serve(void *ctx)
{
	(void)ctx;
	step('s');
	return *serve_script++;
}

static enum s05_fail s_stage(void *ctx, uint32_t *length)
{
	(void)ctx;
	step('g');
	*length = staged_len;
	return slave_stage;
}

static void s_reply(void *ctx, uint8_t v) { (void)ctx; step(v ? '1' : '0'); }
static void s_stop(void *ctx) { (void)ctx; step('o'); }

static const struct s05_slave_ops slave_handoff = {
	NULL, s_stage, s_reply, s_stop, h_teardown, h_kernel, h_svect, h_jump,
};

static const struct boot_slave_ops slave_ops = {
	NULL, s_bringup, s_ddr, s_fatal, s_serve, &slave_handoff, staging,
	0x0803D000u,
};

static void test_slave(void)
{
	static const uint8_t ok[] = { 0 };
	static const uint8_t noos_then_ok[] = { 1, 1, 0 };

	master_reset();
	slave_stage = S05_FAIL_NONE;
	serve_script = ok;
	boot_slave(&slave_ops);
	/* S-05.020: served, staged and replied 0, then the hand-off. */
	CHECK(strcmp(trace, "bdsg0oTkVJ") == 0);

	/* S-05.021: replies of 1 keep the server running. */
	master_reset();
	serve_script = noos_then_ok;
	boot_slave(&slave_ops);
	CHECK(strcmp(trace, "bdsssg0oTkVJ") == 0);

	master_reset();
	mf.ddr_ok = false;
	serve_script = ok;
	boot_slave(&slave_ops);
	CHECK(strcmp(trace, "bdf") == 0);
}

int main(void)
{
	RUN(test_decision_table);
	RUN(test_ddr_check);
	RUN(test_load_failures);
	RUN(test_boot_prompt);
	RUN(test_start_in_menu);
	RUN(test_held_entry);
	RUN(test_slave);
	return check_summary("test_boot_flow");
}

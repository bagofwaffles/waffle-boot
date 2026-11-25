/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tests for the start-up steps (src/drivers/bf70x/crt.c). */
#include <stdio.h>
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x/coremmr.h"
#include "waffle/drivers/bf70x/rcu.h"
#include "waffle/drivers/bf70x/start.h"

static void reset(void)
{
	fake_mmr_reset();
	fake_core_reset();
}

static void test_sections(void)
{
	static const char init[8] = "abcdefg";
	char data[8], bss[16], same[4] = "xyz";
	const struct wb_crt_section t[] = {
		{ data, init, sizeof(data) },
		{ bss, NULL, sizeof(bss) },
		{ same, same, sizeof(same) },
		{ NULL, NULL, 0 },
	};
	unsigned i;

	memset(data, 0x55, sizeof(data));
	memset(bss, 0x55, sizeof(bss));
	wb_crt_sections(t, ARRAY_LEN(t));
	CHECK(memcmp(data, init, sizeof(data)) == 0);
	for (i = 0; i < sizeof(bss); i++)
		CHECK(bss[i] == 0);
	CHECK(strcmp(same, "xyz") == 0);
}

/*
 * S-13a.040: every L1-space access (0x11800000-0x11C00000, which holds
 * the parity, tag and dirty-bit views too) lies inside one of the ranges.
 */
static bool l1_accesses_inside(const struct wb_l1_range *r, size_t n)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i, j, count = fake_mmr_count();

	for (i = 0; i < count; i++) {
		uint32_t a = log[i].addr;
		bool in = false;

		if (a < 0x11800000u || a >= 0x11C00000u)
			continue;
		for (j = 0; j < n; j++)
			in = in || (a >= r[j].start && a < r[j].end);
		if (!in) {
			printf("  L1 access at 0x%08x outside the ranges\n",
			       (unsigned)a);
			return false;
		}
	}
	return true;
}

static void test_parity_pass(void)
{
	static const struct wb_l1_range r[] = {
		{ 0x11800000u, 0x11800008u },
		{ 0x11B01F00u, 0x11B01F04u },
	};
	static const struct fake_mmr_access want[] = {
		/* Read checking off on both sides, then ENX. */
		FAKE_R32(L1IM_ICTL, L1IM_ICTL_RDCHK | L1IM_ICTL_ENCPLB),
		FAKE_W32(L1IM_ICTL, L1IM_ICTL_ENCPLB),
		FAKE_R32(L1DM_DCTL, L1DM_DCTL_CBYPASS | L1DM_DCTL_RDCHK),
		FAKE_W32(L1DM_DCTL, L1DM_DCTL_CBYPASS | L1DM_DCTL_ENX),
		FAKE_CSYNC,
		/* Each word from its own address, back to it. */
		FAKE_R32(0x11800000u, 0x11111111u),
		FAKE_W32(0x11800000u, 0x11111111u),
		FAKE_R32(0x11800004u, 0x22222222u),
		FAKE_W32(0x11800004u, 0x22222222u),
		FAKE_R32(0x11B01F00u, 0x5245434Fu),
		FAKE_W32(0x11B01F00u, 0x5245434Fu),
		FAKE_SYNC,
		FAKE_R32(L1DM_DCTL, L1DM_DCTL_CBYPASS | L1DM_DCTL_ENX),
		FAKE_W32(L1DM_DCTL, L1DM_DCTL_CBYPASS),
		FAKE_CSYNC,
	};

	reset();
	fake_mmr_set(L1IM_ICTL, L1IM_ICTL_RDCHK | L1IM_ICTL_ENCPLB);
	fake_mmr_set(L1DM_DCTL, L1DM_DCTL_CBYPASS | L1DM_DCTL_RDCHK);
	fake_mmr_set(0x11800000u, 0x11111111u);
	fake_mmr_set(0x11800004u, 0x22222222u);
	/* The reboot record keeps its value (S-01.008). */
	fake_mmr_set(0x11B01F00u, 0x5245434Fu);
	/* What a +0x80000 read would return: parity bits, not the word. */
	fake_mmr_set(0x11880000u, 0x01010101u);
	fake_mmr_set(0x11880004u, 0x01000100u);
	fake_mmr_set(0x11B81F00u, 0x00010001u);
	wb_l1_parity_init(r, ARRAY_LEN(r));
	CHECK(fake_mmr_expect("parity", want, ARRAY_LEN(want)));
	CHECK(l1_accesses_inside(r, ARRAY_LEN(r)));
	CHECK(fake_mmr_get(0x11800000u) == 0x11111111u);
	CHECK(fake_mmr_get(0x11800004u) == 0x22222222u);
	CHECK(fake_mmr_get(0x11B01F00u) == 0x5245434Fu);
	/* Checking is not turned back on by the pass. */
	CHECK((fake_mmr_get(L1IM_ICTL) & L1IM_ICTL_RDCHK) == 0);
	CHECK((fake_mmr_get(L1DM_DCTL) & L1DM_DCTL_RDCHK) == 0);
	CHECK(fake_mmr_errors() == 0);
}

static void test_bf703_ranges(void)
{
	/* S-01.012's L1 spaces, the cache halves included. */
	static const struct wb_l1_range s01[] = {
		{ 0x11800000u, 0x11808000u },
		{ 0x11900000u, 0x11908000u },
		{ 0x11A00000u, 0x11A10000u },
		{ 0x11B00000u, 0x11B02000u },
	};
	unsigned i;

	for (i = 0; i < ARRAY_LEN(s01); i++) {
		CHECK(wb_l1_bf703[i].start == s01[i].start);
		CHECK(wb_l1_bf703[i].end == s01[i].end);
		CHECK((wb_l1_bf703[i].start & 3u) == 0);
		CHECK((wb_l1_bf703[i].end & 3u) == 0);
	}
}

static unsigned count_writes(wb_mmr_addr addr)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i, n = fake_mmr_count();
	unsigned c = 0;

	for (i = 0; i < n; i++)
		if (log[i].op == FAKE_MMR_WRITE && log[i].addr == addr)
			c++;
	return c;
}

static const struct wb_l1_range one_word[] = {
	{ 0x11800000u, 0x11800004u },
};

static void test_start_cold(void)
{
	char bss[4] = "abc";
	const struct wb_crt_section s[] = { { bss, NULL, sizeof(bss) } };
	const struct wb_start_config cfg = {
		.l1 = one_word, .n_l1 = 1,
		.sections = s, .n_sections = 1,
		.cplb = &wb_cplb_bf703_default,
	};

	reset();
	fake_mmr_set(RCU0_MSG, 0);
	CHECK(wb_start_init(&cfg) == WB_CPLB_OK);
	CHECK(count_writes(0x11800000u) == 1);
	CHECK(count_writes(RCU0_MSG_SET) == 1);
	CHECK(fake_mmr_get(RCU0_MSG_SET) == RCU_MSG_C0L1INIT);
	CHECK(bss[0] == 0 && bss[3] == 0);
	CHECK(fake_mmr_get(L1IM_ICTL) & L1IM_ICTL_ENCPLB);
	CHECK(fake_mmr_get(L1DM_DCTL) & L1DM_DCTL_ENCPLB);
	CHECK((fake_mmr_get(L1DM_DCTL) & L1DM_DCTL_ENX) == 0);
	/* S-13a.040: read parity checking on, through the cache set-up. */
	CHECK(fake_mmr_get(L1IM_ICTL) & L1IM_ICTL_RDCHK);
	CHECK(fake_mmr_get(L1DM_DCTL) & L1DM_DCTL_RDCHK);
	CHECK(fake_mmr_errors() == 0);
}

/*
 * S-13a.040: RDCHK set on both sides straight after the pass (after its
 * last L1 access), and in every later write of the two registers; on a
 * warm start at the same point, with no pass.
 */
static void rdchk_after_pass(uint32_t msg)
{
	const struct wb_start_config cfg = {
		.l1 = one_word, .n_l1 = 1,
		.sections = NULL, .n_sections = 0,
		.cplb = &wb_cplb_bf703_default,
	};
	const struct fake_mmr_access *log;
	size_t i, n, last_l1 = 0, ion = 0, don = 0;

	reset();
	fake_mmr_set(RCU0_MSG, msg);
	CHECK(wb_start_init(&cfg) == WB_CPLB_OK);
	log = fake_mmr_log();
	n = fake_mmr_count();
	for (i = 0; i < n; i++) {
		const bool w = log[i].op == FAKE_MMR_WRITE;

		if (log[i].addr == 0x11800000u)
			last_l1 = i + 1u;
		if (w && log[i].addr == L1IM_ICTL) {
			if (ion)
				CHECK(log[i].value & L1IM_ICTL_RDCHK);
			else if (log[i].value & L1IM_ICTL_RDCHK)
				ion = i + 1u;
		}
		if (w && log[i].addr == L1DM_DCTL) {
			if (don)
				CHECK(log[i].value & L1DM_DCTL_RDCHK);
			else if (log[i].value & L1DM_DCTL_RDCHK)
				don = i + 1u;
		}
	}
	CHECK(ion != 0u && don != 0u);
	CHECK(ion > last_l1 && don > last_l1);
	CHECK(fake_mmr_get(L1IM_ICTL) & L1IM_ICTL_RDCHK);
	CHECK(fake_mmr_get(L1DM_DCTL) & L1DM_DCTL_RDCHK);
}

static void test_start_rdchk_after_pass(void)
{
	rdchk_after_pass(0);			/* cold */
	CHECK(count_writes(0x11800000u) == 1);
	rdchk_after_pass(RCU_MSG_C0L1INIT);	/* warm */
	CHECK(count_writes(0x11800000u) == 0);
}

/*
 * S-13a.040: no L1 range is cache while the pass writes it. With both
 * caches configured at entry, the writes that clear L1IM_ICTL.CFG and
 * L1DM_DCTL.CFG come before the one that sets ENX.
 */
static void test_start_caches_off_first(void)
{
	const struct wb_start_config cfg = {
		.l1 = one_word, .n_l1 = 1,
		.sections = NULL, .n_sections = 0,
		.cplb = NULL,
	};
	const struct fake_mmr_access *log;
	size_t i, n;
	size_t icfg_off = 0, dcfg_off = 0, enx_on = 0, l1 = 0;

	reset();
	fake_mmr_set(RCU0_MSG, 0);
	fake_mmr_set(L1IM_ICTL, L1IM_ICTL_CFG | L1IM_ICTL_ENCPLB);
	fake_mmr_set(L1DM_DCTL, L1DM_DCTL_CFG_AB | L1DM_DCTL_ENCPLB);
	CHECK(wb_start_init(&cfg) == WB_CPLB_OK);
	log = fake_mmr_log();
	n = fake_mmr_count();
	for (i = 0; i < n; i++) {
		if (log[i].op != FAKE_MMR_WRITE)
			continue;
		if (!icfg_off && log[i].addr == L1IM_ICTL &&
		    !(log[i].value & L1IM_ICTL_CFG))
			icfg_off = i + 1u;
		if (!dcfg_off && log[i].addr == L1DM_DCTL &&
		    !(log[i].value & L1DM_DCTL_CFG_MASK))
			dcfg_off = i + 1u;
		if (!enx_on && log[i].addr == L1DM_DCTL &&
		    (log[i].value & L1DM_DCTL_ENX))
			enx_on = i + 1u;
		if (!l1 && log[i].addr == 0x11800000u)
			l1 = i + 1u;
	}
	CHECK(icfg_off != 0 && dcfg_off != 0 && enx_on != 0 && l1 != 0);
	CHECK(icfg_off < enx_on && dcfg_off < enx_on && enx_on < l1);
}

/*
 * S-13a.040: entered with read parity checking on in both controllers, the
 * cold start has it off on both sides before its first L1 read.
 */
static void test_start_rdchk_off_first(void)
{
	const struct wb_start_config cfg = {
		.l1 = one_word, .n_l1 = 1,
		.sections = NULL, .n_sections = 0,
		.cplb = NULL,
	};
	const struct fake_mmr_access *log;
	uint32_t ictl = L1IM_ICTL_RDCHK, dctl = L1DM_DCTL_RDCHK;
	size_t i, n;
	bool seen = false;

	reset();
	fake_mmr_set(RCU0_MSG, 0);
	fake_mmr_set(L1IM_ICTL, ictl);
	fake_mmr_set(L1DM_DCTL, dctl);
	CHECK(wb_start_init(&cfg) == WB_CPLB_OK);
	log = fake_mmr_log();
	n = fake_mmr_count();
	for (i = 0; i < n && !seen; i++) {
		if (log[i].op == FAKE_MMR_WRITE && log[i].addr == L1IM_ICTL)
			ictl = log[i].value;
		if (log[i].op == FAKE_MMR_WRITE && log[i].addr == L1DM_DCTL)
			dctl = log[i].value;
		if (log[i].op == FAKE_MMR_READ && log[i].addr == 0x11800000u)
			seen = true;
	}
	CHECK(seen);
	CHECK((ictl & L1IM_ICTL_RDCHK) == 0);
	CHECK((dctl & L1DM_DCTL_RDCHK) == 0);
	CHECK(l1_accesses_inside(one_word, ARRAY_LEN(one_word)));
}

static void test_start_warm_skips_pass(void)
{
	const struct wb_start_config cfg = {
		.l1 = one_word, .n_l1 = 1,
		.sections = NULL, .n_sections = 0,
		.cplb = NULL,
	};

	reset();
	fake_mmr_set(RCU0_MSG, RCU_MSG_C0L1INIT | 0x5u);
	fake_mmr_set(L1IM_ICTL, 0x206u);
	CHECK(wb_start_init(&cfg) == WB_CPLB_OK);
	CHECK(count_writes(0x11800000u) == 0);
	CHECK(count_writes(RCU0_MSG_SET) == 0);
	/* No table: the ROM's CPLBs and cache are left off; read parity
	 * checking on where the pass would have been (S-13a.040). */
	CHECK(fake_mmr_get(L1IM_ICTL) == L1IM_ICTL_RDCHK);
}

static void test_start_bad_table(void)
{
	static const struct wb_cplb bad[] = {
		{ 0x08000004u, CPLB_VALID | WB_CPLB_PSIZE(WB_CPLB_1K) },
	};
	struct wb_cplb_config c = wb_cplb_bf703_default;
	char bss[2] = "a";
	const struct wb_crt_section s[] = { { bss, NULL, sizeof(bss) } };
	const struct wb_start_config cfg = {
		.l1 = one_word, .n_l1 = 1,
		.sections = s, .n_sections = 1,
		.cplb = &c,
	};

	reset();
	fake_mmr_set(RCU0_MSG, RCU_MSG_C0L1INIT);
	c.icplb = bad;
	c.n_icplb = 1;
	CHECK(wb_start_init(&cfg) == WB_CPLB_ERR_ENTRY);
	CHECK(bss[0] == 0);
	CHECK(fake_mmr_get(L1IM_ICTL) == L1IM_ICTL_RDCHK);
}

int main(void)
{
	RUN(test_sections);
	RUN(test_parity_pass);
	RUN(test_bf703_ranges);
	RUN(test_start_cold);
	RUN(test_start_rdchk_off_first);
	RUN(test_start_rdchk_after_pass);
	RUN(test_start_caches_off_first);
	RUN(test_start_warm_skips_pass);
	RUN(test_start_bad_table);
	return check_summary("test_drv_crt");
}

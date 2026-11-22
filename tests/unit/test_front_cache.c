/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's caches (src/front/fr_cache.c, and src/front/fr_hw_cache.c
 * built here for the host since its registers are faked).
 *
 * The table: each side passes the main image's own table rules
 * (wb_cplb_check(): valid, sized, aligned, no two pages over one address,
 * CPROPS defined), and every range the front runs from, reads or writes
 * lies whole in one page of the kind it needs, since the default
 * descriptors are left as found: L1 uncached, L2 and the bulk data
 * write-through, the buffer the panel's DMA reads uncached, the SMC bank
 * I/O device space. The system MMR space is in no page on either side, as
 * in the main image's table, so its accesses take the same defaults as the
 * main image's.
 *
 * The sequence, from the access log, over registers that start as the boot
 * ROM leaves them (HRM, Preboot Operations, Cache Configuration: valid
 * pairs over L1, the ROM, L2 and DDR): at each ENCPLB set, every one of
 * the side's sixteen pairs is the table's or invalid, no two valid pairs
 * meet, CFG was set before the pairs and a CSYNC came after them; no pair
 * is written while its side's CPLBs are on; the defaults are never
 * written; fr_cache_off() leaves both sides off and every pair as found.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "front/fr_hw_cache.c"
#include "front/fr_handoff.h"
#include "waffle/drivers/bf70x_cplb.h"

/* The page of t holding [lo, hi) whole, or -1; -2 when one only meets it. */
static int page_of(const struct wb_cplb *t, unsigned n, uint64_t lo, uint64_t hi)
{
	int got = -1;

	for (unsigned i = 0; i < n; i++) {
		const uint64_t a = t[i].addr, b = a + wb_cplb_page_bytes(t[i].data);

		if (lo >= a && hi <= b)
			got = (int)i;
		else if (lo < b && a < hi)
			return -2;
	}
	return got;
}

static void test_tables(void)
{
	CHECK(wb_cplb_check(fr_icplb, FR_ICPLB_N, false) == WB_CPLB_OK);
	CHECK(wb_cplb_check(fr_dcplb, FR_DCPLB_N, true) == WB_CPLB_OK);
	for (unsigned i = 0; i < FR_DCPLB_N; i++) {
		const uint32_t c = fr_dcplb[i].data & DCPLB_CPROPS_MASK;

		/* never write-back: turning the cache off writes nothing back */
		CHECK(c == DCPLB_WT || c == DCPLB_NOCACHE || c == DCPLB_IO);
		CHECK((fr_dcplb[i].data & DCPLB_SWRITE) == 0u ||
		      (fr_dcplb[i].data & DCPLB_DIRTY) != 0u);
	}
}

enum kind { NONE, UNCACHED, CACHED, WT, IO, RO };

/* What the page holding [lo, hi) makes of it on one side. */
static enum kind kind_of(const struct wb_cplb *t, unsigned n, bool data, uint32_t lo,
			 uint32_t hi)
{
	const int p = page_of(t, n, lo, hi);
	uint32_t c;

	if (p < 0)
		return NONE;
	c = t[p].data & (data ? DCPLB_CPROPS_MASK : ICPLB_CPROPS_MASK);
	if (!data)
		return c == ICPLB_CACHE ? CACHED : UNCACHED;
	if ((t[p].data & DCPLB_SWRITE) == 0u)
		return RO;
	return c == DCPLB_WT ? WT : c == DCPLB_IO ? IO : c == DCPLB_NOCACHE ? UNCACHED : NONE;
}

static void test_ranges(void)
{
	static const struct {
		const char *what;
		uint32_t lo, hi;
		enum kind code, data;	/* NONE: in no page */
	} r[] = {
		{ "front L1 code", FR_L1_CODE_START, FR_L1_CODE_END, UNCACHED, UNCACHED },
		{ "front L1 data and INIT stack", FR_L1_DATA_START, FR_L1_DATA_END, UNCACHED, UNCACHED },
		{ "the data cache's half of L1 data A", 0x11804000u, 0x11808000u, UNCACHED, UNCACHED },
		{ "main L1 code", 0x11A00000u, 0x11A04000u, UNCACHED, UNCACHED },
		{ "main L1 data A", 0x11800000u, 0x11802000u, UNCACHED, UNCACHED },
		{ "L1 data B", 0x11900000u, 0x11908000u, UNCACHED, UNCACHED },
		{ "L1 scratch, the reboot record", 0x11B00000u, 0x11B02000u, UNCACHED, UNCACHED },
		{ "front L2 code, data, bss, stack", FR_L2_START, FR_L2_END, CACHED, WT },
		{ "descriptor and hand-off record", FR_SHARED_START, FR_SHARED_END, CACHED, WT },
		{ "main L2 and hand-over reserve", 0x08031000u, 0x0803E000u, CACHED, WT },
		{ "main DDR image", 0x8803D000u, 0x8833D000u, CACHED, WT },
		{ "recovery's staging, the logo's data", FR_STAGING_START, FR_STAGING_END, CACHED, WT },
		{ "frame buffer and line band (MDMA2 reads)", FR_FRAME_START, FR_FRAME_END, CACHED,
		  UNCACHED },
		{ "SMC bank 0, the panel", 0x70000000u, 0x74000000u, NONE, IO },
		{ "OTP", 0x38000000u, 0x38001000u, NONE, RO },
		{ "system MMRs", 0x20000000u, 0x30000000u, NONE, NONE },
		{ "system MMRs (DMC, SPU, SMPU)", 0x31000000u, 0x32000000u, NONE, NONE },
	};

	for (unsigned i = 0; i < ARRAY_LEN(r); i++) {
		const enum kind ik = kind_of(fr_icplb, FR_ICPLB_N, false, r[i].lo, r[i].hi);
		const enum kind dk = kind_of(fr_dcplb, FR_DCPLB_N, true, r[i].lo, r[i].hi);

		if (ik != r[i].code || dk != r[i].data)
			printf("  %s: code %d, data %d\n", r[i].what, (int)ik, (int)dk);
		CHECK(page_of(fr_icplb, FR_ICPLB_N, r[i].lo, r[i].hi) != -2 &&
		      page_of(fr_dcplb, FR_DCPLB_N, r[i].lo, r[i].hi) != -2);
		CHECK(ik == r[i].code && dk == r[i].data);
	}
}

/* ---- the sequence ---- */

/* As the boot ROM leaves the pairs (HRM Tables 36-13, 36-15, their pages
 * here valid in the PRM's layout), the rest valid too: stale pages over
 * the front's own. */
static const struct wb_cplb rom_i[FR_CPLB_PAIRS] = {
	{ 0x11A00000u, 0x00030001u }, { 0x04000000u, 0x00011001u }, { 0x04004000u, 0x00011001u },
	{ 0x04008000u, 0x00011001u }, { 0x0400C000u, 0x00010001u }, { 0x08002000u, 0x00011001u },
	{ 0x80000000u, 0x00091001u }, { 0x70000000u, 0x00081001u }, { 0x40000000u, 0x00030001u },
	{ 0x40010000u, 0x00030001u }, { 0x08000000u, 0x00001001u }, { 0x88000000u, 0x00041001u },
	{ 0x11800000u, 0x00060001u }, { 0x08010000u, 0x00031001u }, { 0x8C000000u, 0x00081001u },
	{ 0x80400000u, 0x00051001u },
};
static const struct wb_cplb rom_d[FR_CPLB_PAIRS] = {
	{ 0x04000000u, 0x00015001u }, { 0x08000000u, 0x00005091u }, { 0x08001000u, 0x00015091u },
	{ 0x11800000u, 0x00020091u }, { 0x11804000u, 0x00020091u }, { 0x11900000u, 0x00020091u },
	{ 0x11904000u, 0x00020091u }, { 0x11B00000u, 0x00020091u }, { 0x80000000u, 0x00095091u },
	{ 0x70000000u, 0x00084091u }, { 0x40000000u, 0x00080091u }, { 0x40010000u, 0x00080091u },
	{ 0x88000000u, 0x00041091u }, { 0x8FE00000u, 0x00055091u }, { 0x38000000u, 0x00010001u },
	{ 0x08030000u, 0x00031091u },
};
#define ROM_IDFLT 0x00000300u
#define ROM_DDFLT 0x00000F00u

static void preset(void)
{
	fake_mmr_reset();
	for (unsigned i = 0; i < FR_CPLB_PAIRS; i++) {
		fake_mmr_set(L1IM_ICPLB_ADDR(i), rom_i[i].addr);
		fake_mmr_set(L1IM_ICPLB_DATA(i), rom_i[i].data);
		fake_mmr_set(L1DM_DCPLB_ADDR(i), rom_d[i].addr);
		fake_mmr_set(L1DM_DCPLB_DATA(i), rom_d[i].data);
	}
	fake_mmr_set(L1IM_ICPLB_DFLT, ROM_IDFLT);
	fake_mmr_set(L1DM_DCPLB_DFLT, ROM_DDFLT);
	/* as _fr_entry leaves them: CPLBs and caches off, read checking on */
	fake_mmr_set(L1IM_ICTL, L1IM_ICTL_RDCHK);
	fake_mmr_set(L1DM_DCTL, L1DM_DCTL_RDCHK);
	fake_mmr_clear_log();
}

/* One side as the log replays it. */
struct side {
	wb_mmr_addr ctl, addr0, data0, dflt;
	uint32_t encplb, cfg;
	const struct wb_cplb *table;
	unsigned n;
	uint32_t ctl_v, addr[FR_CPLB_PAIRS], data[FR_CPLB_PAIRS];
	bool cfg_before_pairs, csync_after_pairs, wrote_pair, bad_order, dflt_written;
	unsigned enables, pair_writes_on;
	bool all_table, overlap;
};

static void side_init(struct side *s, bool data)
{
	memset(s, 0, sizeof *s);
	s->ctl = data ? L1DM_DCTL : L1IM_ICTL;
	s->addr0 = data ? L1DM_DCPLB_ADDR(0) : L1IM_ICPLB_ADDR(0);
	s->data0 = data ? L1DM_DCPLB_DATA(0) : L1IM_ICPLB_DATA(0);
	s->dflt = data ? L1DM_DCPLB_DFLT : L1IM_ICPLB_DFLT;
	s->encplb = data ? L1DM_DCTL_ENCPLB : L1IM_ICTL_ENCPLB;
	s->cfg = data ? L1DM_DCTL_CFG_A : L1IM_ICTL_CFG;
	s->table = data ? fr_dcplb : fr_icplb;
	s->n = data ? FR_DCPLB_N : FR_ICPLB_N;
	s->ctl_v = fake_mmr_get(s->ctl);
	for (unsigned i = 0; i < FR_CPLB_PAIRS; i++) {
		s->addr[i] = fake_mmr_get(s->addr0 + 4u * i);
		s->data[i] = fake_mmr_get(s->data0 + 4u * i);
	}
	s->all_table = true;
}

/* At an ENCPLB set: the pairs the hardware would search. */
static void at_enable(struct side *s)
{
	s->enables++;
	if (!s->cfg_before_pairs || !s->csync_after_pairs)
		s->bad_order = true;
	for (unsigned i = 0; i < FR_CPLB_PAIRS; i++) {
		const bool want_valid = i < s->n;

		if (want_valid ? s->addr[i] != s->table[i].addr ||
				 s->data[i] != (s->table[i].data | CPLB_LOCK) :
				 (s->data[i] & CPLB_VALID) != 0u)
			s->all_table = false;
		for (unsigned j = i + 1u; j < FR_CPLB_PAIRS; j++) {
			const uint64_t a0 = s->addr[i], a1 = a0 + wb_cplb_page_bytes(s->data[i]);
			const uint64_t b0 = s->addr[j], b1 = b0 + wb_cplb_page_bytes(s->data[j]);

			if ((s->data[i] & CPLB_VALID) != 0u && (s->data[j] & CPLB_VALID) != 0u &&
			    a0 < b1 && b0 < a1)
				s->overlap = true;
		}
	}
}

static void replay(struct side *s, const struct fake_mmr_access *a)
{
	if (a->op == FAKE_MMR_CSYNC && s->wrote_pair)
		s->csync_after_pairs = true;
	if (a->op != FAKE_MMR_WRITE)
		return;
	if (a->addr == s->dflt)
		s->dflt_written = true;
	if (a->addr == s->ctl) {
		const bool was = (s->ctl_v & s->encplb) != 0u;

		s->ctl_v = a->value;
		if ((a->value & s->cfg) != 0u && !s->wrote_pair)
			s->cfg_before_pairs = true;
		if (!was && (a->value & s->encplb) != 0u)
			at_enable(s);
		if (!was || (a->value & s->encplb) == 0u) {
			s->wrote_pair = false;
			s->csync_after_pairs = false;
		}
		return;
	}
	for (unsigned i = 0; i < FR_CPLB_PAIRS; i++)
		if (a->addr == s->addr0 + 4u * i || a->addr == s->data0 + 4u * i) {
			if ((s->ctl_v & s->encplb) != 0u)
				s->pair_writes_on++;
			if (a->addr == s->addr0 + 4u * i)
				s->addr[i] = a->value;
			else
				s->data[i] = a->value;
			s->wrote_pair = true;
			s->csync_after_pairs = false;
		}
}

static void test_sequence(void)
{
	struct side si, sd;
	const struct fake_mmr_access *log;
	size_t n;

	preset();
	side_init(&si, false);
	side_init(&sd, true);
	fr_cache_on();
	log = fake_mmr_log();
	n = fake_mmr_count();
	for (size_t k = 0; k < n; k++) {
		replay(&si, &log[k]);
		replay(&sd, &log[k]);
	}
	CHECK(fake_mmr_errors() == 0u);
	CHECK(si.enables == 1u && sd.enables == 1u);
	CHECK(si.all_table && sd.all_table);	/* all sixteen pairs written */
	CHECK(!si.overlap && !sd.overlap);	/* no multiple hit */
	CHECK(!si.bad_order && !sd.bad_order);
	CHECK(si.pair_writes_on == 0u && sd.pair_writes_on == 0u);
	CHECK(!si.dflt_written && !sd.dflt_written);
	CHECK((fake_mmr_get(L1IM_ICTL) & (L1IM_ICTL_ENCPLB | L1IM_ICTL_CFG)) ==
	      (L1IM_ICTL_ENCPLB | L1IM_ICTL_CFG));
	CHECK((fake_mmr_get(L1DM_DCTL) & (L1DM_DCTL_ENCPLB | L1DM_DCTL_CFG_MASK)) ==
	      (L1DM_DCTL_ENCPLB | L1DM_DCTL_CFG_A));
	CHECK((fake_mmr_get(L1IM_ICTL) & L1IM_ICTL_RDCHK) != 0u &&
	      (fake_mmr_get(L1DM_DCTL) & (L1DM_DCTL_RDCHK | L1DM_DCTL_ENX)) == L1DM_DCTL_RDCHK);

	/* off: both sides off before any pair is put back, every pair as found */
	fake_mmr_clear_log();
	si.ctl_v = fake_mmr_get(L1IM_ICTL);
	sd.ctl_v = fake_mmr_get(L1DM_DCTL);
	si.pair_writes_on = sd.pair_writes_on = 0u;
	fr_cache_off();
	log = fake_mmr_log();
	n = fake_mmr_count();
	for (size_t k = 0; k < n; k++) {
		replay(&si, &log[k]);
		replay(&sd, &log[k]);
	}
	CHECK(si.pair_writes_on == 0u && sd.pair_writes_on == 0u);
	CHECK(!si.dflt_written && !sd.dflt_written);
	CHECK(fake_mmr_get(L1IM_ICTL) == L1IM_ICTL_RDCHK &&
	      fake_mmr_get(L1DM_DCTL) == L1DM_DCTL_RDCHK);
	for (unsigned i = 0; i < FR_CPLB_PAIRS; i++) {
		CHECK(fake_mmr_get(L1IM_ICPLB_ADDR(i)) == rom_i[i].addr &&
		      fake_mmr_get(L1IM_ICPLB_DATA(i)) == rom_i[i].data);
		CHECK(fake_mmr_get(L1DM_DCPLB_ADDR(i)) == rom_d[i].addr &&
		      fake_mmr_get(L1DM_DCPLB_DATA(i)) == rom_d[i].data);
	}
	CHECK(fake_mmr_get(L1IM_ICPLB_DFLT) == ROM_IDFLT &&
	      fake_mmr_get(L1DM_DCPLB_DFLT) == ROM_DDFLT);

	/* a second off does nothing */
	fake_mmr_clear_log();
	fr_cache_off();
	CHECK(fake_mmr_count() == 0u);
}

int main(void)
{
	/* wb_cplb_check()'s object needs the register and core fakes to link */
	fake_mmr_reset();
	wb_core_csync();
	RUN(test_tables);
	RUN(test_ranges);
	RUN(test_sequence);
	return check_summary("test_front_cache");
}

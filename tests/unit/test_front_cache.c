/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's cache pages (src/front/fr_cache.c): each side passes the main
 * image's own table rules (wb_cplb_check(): valid, sized, aligned, no two
 * pages over one address, CPROPS defined); the default descriptors raise
 * no exception on a miss and let supervisor writes through; every range the
 * front reads or writes falls where it should: L1 in no page, the code it
 * runs from L2 cached, the data it reads in bulk (the loaded image, the
 * shared block, recovery's staging) cached write-through, the buffer the
 * panel's DMA reads, the SMC bank and the reboot record in no page.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "front/fr_cache.h"
#include "front/fr_handoff.h"
#include "front/fr_slots.h"
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
		/* writable, write-through: turning the cache off writes nothing back */
		CHECK((fr_dcplb[i].data & (DCPLB_SWRITE | DCPLB_DIRTY)) == (DCPLB_SWRITE | DCPLB_DIRTY));
		CHECK((fr_dcplb[i].data & DCPLB_CPROPS_MASK) == DCPLB_WT);
	}
	for (unsigned i = 0; i < FR_ICPLB_N; i++)
		CHECK((fr_icplb[i].data & ICPLB_CPROPS_MASK) == ICPLB_CACHE);
	/* no exception on a miss, in L1 or system memory; writes allowed; no
	 * reserved bit */
	CHECK(FR_ICPLB_DFLT == ((1u << 8) | (1u << 4)));
	CHECK(FR_DCPLB_DFLT == ((1u << 11) | (1u << 8) | (1u << 7) | (1u << 4) | 4u));
	CHECK((FR_DCPLB_DFLT & 7u) == 4u);	/* SYSCPROPS: I/O device space */
}

static void test_ranges(void)
{
	static const struct {
		const char *what;
		uint32_t lo, hi;
		int code, data;		/* 1: in a cached page; 0: in none */
	} r[] = {
		{ "front L1 code", FR_L1_CODE_START, FR_L1_CODE_END, 0, 0 },
		{ "front L1 data and INIT stack", FR_L1_DATA_START, FR_L1_DATA_END, 0, 0 },
		{ "the data cache's own half of L1 data A", 0x11804000u, 0x11808000u, 0, 0 },
		{ "main L1 code", 0x11A00000u, 0x11A04000u, 0, 0 },
		{ "main L1 data A, B", 0x11800000u, 0x11802000u, 0, 0 },
		{ "L1 data B", 0x11900000u, 0x11908000u, 0, 0 },
		{ "L1 scratch, the reboot record", 0x11B00000u, 0x11B02000u, 0, 0 },
		{ "front L2 code, data, bss, stack", FR_L2_START, FR_L2_END, 1, 1 },
		{ "descriptor and hand-off record", FR_SHARED_START, FR_SHARED_END, 1, 1 },
		{ "main L2 and hand-over reserve", 0x08031000u, 0x0803E000u, 1, 1 },
		{ "main DDR image", 0x8803D000u, 0x8833D000u, -1, 1 },
		{ "recovery's staging, the logo's data", FR_STAGING_START, FR_STAGING_END, -1, 1 },
		{ "frame buffer and line band (MDMA2 reads)", FR_FRAME_START, FR_FRAME_END, 0, 0 },
		{ "SMC bank 0, the panel", 0x70000000u, 0x74000000u, 0, 0 },
		{ "OTP", 0x38000000u, 0x38001000u, 0, 0 },
	};

	for (unsigned i = 0; i < ARRAY_LEN(r); i++) {
		const int ip = page_of(fr_icplb, FR_ICPLB_N, r[i].lo, r[i].hi);
		const int dp = page_of(fr_dcplb, FR_DCPLB_N, r[i].lo, r[i].hi);

		if (r[i].code >= 0 && (ip >= 0) != (r[i].code == 1))
			printf("  %s: code page %d\n", r[i].what, ip);
		if ((dp >= 0) != (r[i].data == 1))
			printf("  %s: data page %d\n", r[i].what, dp);
		CHECK(ip != -2 && dp != -2);
		CHECK(r[i].code < 0 || (ip >= 0) == (r[i].code == 1));
		CHECK((dp >= 0) == (r[i].data == 1));
	}
	/* no page in L1, where the caches are */
	CHECK(page_of(fr_icplb, FR_ICPLB_N, 0x11800000u, 0x11C00000u) == -1);
	CHECK(page_of(fr_dcplb, FR_DCPLB_N, 0x11800000u, 0x11C00000u) == -1);
}

int main(void)
{
	/* wb_cplb_check()'s object needs the register and core fakes to link */
	fake_mmr_reset();
	wb_core_csync();
	RUN(test_tables);
	RUN(test_ranges);
	return check_summary("test_front_cache");
}

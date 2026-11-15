/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tests for the CPLB and L1 cache set-up (src/drivers/bf70x/cplb.c). */
#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x_coremmr.h"
#include "waffle/drivers/bf70x_cplb.h"
#include "waffle/drivers/bf70x_otp.h"

static struct fake_mmr_access want[160];
static size_t nwant;

static void push(enum fake_mmr_op op, wb_mmr_addr addr, uint32_t value)
{
	want[nwant].op = op;
	want[nwant].width = (op == FAKE_MMR_READ || op == FAKE_MMR_WRITE) ?
				    32u : 0u;
	want[nwant].addr = addr;
	want[nwant].value = value;
	nwant++;
}

static void reset(void)
{
	fake_mmr_reset();
	fake_core_reset();
	nwant = 0;
}

static void test_page_bytes(void)
{
	CHECK(wb_cplb_page_bytes(WB_CPLB_PSIZE(WB_CPLB_1K)) == 0x400u);
	CHECK(wb_cplb_page_bytes(WB_CPLB_PSIZE(WB_CPLB_4K)) == 0x1000u);
	CHECK(wb_cplb_page_bytes(WB_CPLB_PSIZE(WB_CPLB_1M)) == 0x100000u);
	CHECK(wb_cplb_page_bytes(WB_CPLB_PSIZE(WB_CPLB_256M)) == 0x10000000u);
	CHECK(wb_cplb_page_bytes(WB_CPLB_PSIZE(WB_CPLB_1G)) == 0x40000000u);
	CHECK(wb_cplb_page_bytes(WB_CPLB_PSIZE(11)) == 0);
	CHECK(wb_cplb_page_bytes(WB_CPLB_PSIZE(15)) == 0);
	/* Other bits do not change the size. */
	CHECK(wb_cplb_page_bytes(0xFFF0FFFFu) == 0x400u);
}

static void test_check(void)
{
	static const struct wb_cplb ok[] = {
		{ 0x08000000u, CPLB_VALID | WB_CPLB_PSIZE(WB_CPLB_1M) },
		{ 0x08100000u, CPLB_VALID | WB_CPLB_PSIZE(WB_CPLB_4K) },
	};
	static const struct wb_cplb invalid[] = {
		{ 0x08000000u, WB_CPLB_PSIZE(WB_CPLB_1M) },
	};
	static const struct wb_cplb misaligned[] = {
		{ 0x08080000u, CPLB_VALID | WB_CPLB_PSIZE(WB_CPLB_1M) },
	};
	static const struct wb_cplb bad_size[] = {
		{ 0x00000000u, CPLB_VALID | WB_CPLB_PSIZE(12) },
	};
	static const struct wb_cplb overlap[] = {
		{ 0x08000000u, CPLB_VALID | WB_CPLB_PSIZE(WB_CPLB_1M) },
		{ 0x080FF000u, CPLB_VALID | WB_CPLB_PSIZE(WB_CPLB_4K) },
	};
	static const struct wb_cplb top[] = {
		{ 0xC0000000u, CPLB_VALID | WB_CPLB_PSIZE(WB_CPLB_1G) },
		{ 0x80000000u, CPLB_VALID | WB_CPLB_PSIZE(WB_CPLB_1G) },
	};
	static const struct wb_cplb reserved[] = {
		{ 0x08000000u, CPLB_VALID | (2u << 12) | WB_CPLB_PSIZE(WB_CPLB_1M) },
	};
	struct wb_cplb many[WB_CPLB_ENTRIES + 1];
	unsigned i;

	for (i = 0; i < ARRAY_LEN(many); i++) {
		many[i].addr = i * 0x400u;
		many[i].data = CPLB_VALID | WB_CPLB_PSIZE(WB_CPLB_1K);
	}

	reset();
	CHECK(wb_cplb_check(ok, ARRAY_LEN(ok), true) == WB_CPLB_OK);
	CHECK(wb_cplb_check(NULL, 0, true) == WB_CPLB_OK);
	CHECK(wb_cplb_check(NULL, 1, true) == WB_CPLB_ERR_ENTRY);
	CHECK(wb_cplb_check(invalid, 1, true) == WB_CPLB_ERR_ENTRY);
	CHECK(wb_cplb_check(misaligned, 1, true) == WB_CPLB_ERR_ENTRY);
	CHECK(wb_cplb_check(bad_size, 1, false) == WB_CPLB_ERR_ENTRY);
	CHECK(wb_cplb_check(overlap, 2, false) == WB_CPLB_ERR_ENTRY);
	CHECK(wb_cplb_check(top, 2, true) == WB_CPLB_OK);
	/* CPROPS 2 is reserved on the data side only. */
	CHECK(wb_cplb_check(reserved, 1, true) == WB_CPLB_ERR_ENTRY);
	CHECK(wb_cplb_check(reserved, 1, false) == WB_CPLB_OK);
	CHECK(wb_cplb_check(many, WB_CPLB_ENTRIES, true) == WB_CPLB_OK);
	CHECK(wb_cplb_check(many, WB_CPLB_ENTRIES + 1, true) ==
	      WB_CPLB_ERR_COUNT);
	CHECK(fake_mmr_count() == 0);
}

static void test_default_tables_are_valid(void)
{
	const struct wb_cplb_config *c = &wb_cplb_bf703_default;
	unsigned i;

	CHECK(wb_cplb_check(c->icplb, c->n_icplb, false) == WB_CPLB_OK);
	CHECK(wb_cplb_check(c->dcplb, c->n_dcplb, true) == WB_CPLB_OK);
	/* Every data page that can be written is marked dirty (PRM). */
	for (i = 0; i < c->n_dcplb; i++)
		if (c->dcplb[i].data & DCPLB_SWRITE)
			CHECK(c->dcplb[i].data & DCPLB_DIRTY);
	/* No write-back pages: turning the cache off needs no flush. */
	for (i = 0; i < c->n_dcplb; i++) {
		uint32_t cp = c->dcplb[i].data & DCPLB_CPROPS_MASK;

		CHECK(cp != DCPLB_WB && cp != (3u << 12));
	}
	/* The OTP memory (data sheet Figure 3) is mapped, uncached, read-only. */
	for (i = 0; i < c->n_dcplb; i++)
		if (c->dcplb[i].addr == WB_OTP_MEM_BASE)
			break;
	CHECK(i < c->n_dcplb);
	if (i < c->n_dcplb) {
		CHECK((c->dcplb[i].data & DCPLB_CPROPS_MASK) == DCPLB_NOCACHE);
		CHECK(!(c->dcplb[i].data & DCPLB_SWRITE));
	}
}

static void push_disable(uint32_t dctl, uint32_t ictl)
{
	/* RDCHK is kept (S-13a.040). */
	uint32_t downed = L1DM_DCTL_ENCPLB | L1DM_DCTL_CFG_MASK | L1DM_DCTL_ENX;
	uint32_t iowned = L1IM_ICTL_ENCPLB | L1IM_ICTL_CFG;

	push(FAKE_MMR_READ, L1DM_DCTL, dctl);
	push(FAKE_MMR_WRITE, L1DM_DCTL, dctl & ~downed);
	push(FAKE_MMR_SYNC, 0, 0);
	push(FAKE_MMR_READ, L1IM_ICTL, ictl);
	push(FAKE_MMR_WRITE, L1IM_ICTL, ictl & ~iowned);
	push(FAKE_MMR_SYNC, 0, 0);
	push(FAKE_MMR_CSYNC, 0, 0);
}

static void push_entries(wb_mmr_addr a0, wb_mmr_addr d0,
			 const struct wb_cplb *t, unsigned n)
{
	uint32_t i;

	for (i = 0; i < WB_CPLB_ENTRIES; i++) {
		push(FAKE_MMR_WRITE, d0 + 4u * i,
		     i < n ? t[i].data | CPLB_LOCK : 0);
		push(FAKE_MMR_WRITE, a0 + 4u * i, i < n ? t[i].addr : 0);
	}
}

static void test_setup_sequence(void)
{
	const struct wb_cplb_config *c = &wb_cplb_bf703_default;
	uint32_t ictl = L1IM_ICTL_CFG;
	uint32_t dctl = L1DM_DCTL_CFG_AB;

	reset();
	push_disable(0, 0);
	push(FAKE_MMR_READ, L1IM_ICTL, 0);
	push(FAKE_MMR_WRITE, L1IM_ICTL, ictl);
	push_entries(L1IM_ICPLB_ADDR(0), L1IM_ICPLB_DATA(0), c->icplb,
		     c->n_icplb);
	push(FAKE_MMR_CSYNC, 0, 0);
	push(FAKE_MMR_READ, L1IM_ICTL, ictl);
	push(FAKE_MMR_WRITE, L1IM_ICTL, ictl | L1IM_ICTL_ENCPLB);
	push(FAKE_MMR_SYNC, 0, 0);
	push(FAKE_MMR_READ, L1DM_DCTL, 0);
	push(FAKE_MMR_WRITE, L1DM_DCTL, dctl);
	push_entries(L1DM_DCPLB_ADDR(0), L1DM_DCPLB_DATA(0), c->dcplb,
		     c->n_dcplb);
	push(FAKE_MMR_CSYNC, 0, 0);
	push(FAKE_MMR_READ, L1DM_DCTL, dctl);
	push(FAKE_MMR_WRITE, L1DM_DCTL, dctl | L1DM_DCTL_ENCPLB);
	push(FAKE_MMR_SYNC, 0, 0);
	push(FAKE_MMR_CSYNC, 0, 0);

	CHECK(wb_cplb_setup(c) == WB_CPLB_OK);
	CHECK(fake_mmr_expect("setup", want, nwant));
	CHECK(fake_mmr_errors() == 0);
}

static void test_setup_keeps_other_bits(void)
{
	struct wb_cplb_config c = wb_cplb_bf703_default;

	reset();
	/* As the boot ROM leaves it (0x206), plus bits not ours; read parity
	 * checking on, as the start-up leaves it (S-13a.040). */
	fake_mmr_set(L1IM_ICTL, 0x206u | L1IM_ICTL_CPRIORST);
	fake_mmr_set(L1DM_DCTL, L1DM_DCTL_ENX | L1DM_DCTL_CBYPASS |
					L1DM_DCTL_ENCPLB | L1DM_DCTL_CFG_A |
					L1DM_DCTL_RDCHK);
	c.dcache = L1DM_DCTL_CFG_A;
	CHECK(wb_cplb_setup(&c) == WB_CPLB_OK);
	/* S-13a.040: RDCHK in every write of the two registers. */
	{
		const struct fake_mmr_access *log = fake_mmr_log();
		size_t i;

		for (i = 0; i < fake_mmr_count(); i++) {
			if (log[i].op != FAKE_MMR_WRITE)
				continue;
			if (log[i].addr == L1IM_ICTL)
				CHECK(log[i].value & L1IM_ICTL_RDCHK);
			if (log[i].addr == L1DM_DCTL)
				CHECK(log[i].value & L1DM_DCTL_RDCHK);
		}
	}
	CHECK(fake_mmr_get(L1IM_ICTL) ==
	      (L1IM_ICTL_CPRIORST | L1IM_ICTL_RDCHK | L1IM_ICTL_CFG |
	       L1IM_ICTL_ENCPLB));
	CHECK(fake_mmr_get(L1DM_DCTL) ==
	      (L1DM_DCTL_CBYPASS | L1DM_DCTL_RDCHK | L1DM_DCTL_CFG_A |
	       L1DM_DCTL_ENCPLB));
	/* Unused entries are invalid. */
	CHECK(fake_mmr_get(L1DM_DCPLB_DATA(15)) == 0);
	CHECK(fake_mmr_get(L1IM_ICPLB_DATA(15)) == 0);
	CHECK((fake_mmr_get(L1DM_DCPLB_DATA(0)) & CPLB_LOCK) != 0);
	CHECK(fake_mmr_errors() == 0);
}

static void test_setup_empty_side_stays_off(void)
{
	struct wb_cplb_config c = wb_cplb_bf703_default;

	reset();
	c.n_icplb = 0;
	c.icache = false;
	c.dcache = 0;
	CHECK(wb_cplb_setup(&c) == WB_CPLB_OK);
	CHECK(fake_mmr_get(L1IM_ICTL) == 0);
	CHECK(fake_mmr_get(L1DM_DCTL) == L1DM_DCTL_ENCPLB);
}

static void test_setup_rejects_before_writing(void)
{
	static const struct wb_cplb bad[] = {
		{ 0x08000001u, CPLB_VALID | WB_CPLB_PSIZE(WB_CPLB_1K) },
	};
	struct wb_cplb_config c = wb_cplb_bf703_default;

	reset();
	c.dcache = L1DM_DCTL_CFG_SHIFT;	/* not a CFG value */
	CHECK(wb_cplb_setup(&c) == WB_CPLB_ERR_CFG);
	c.dcache = 2u << L1DM_DCTL_CFG_SHIFT;	/* bank B without A */
	CHECK(wb_cplb_setup(&c) == WB_CPLB_ERR_CFG);
	c = wb_cplb_bf703_default;
	c.dcplb = bad;
	c.n_dcplb = 1;
	CHECK(wb_cplb_setup(&c) == WB_CPLB_ERR_ENTRY);
	c = wb_cplb_bf703_default;
	c.n_icplb = WB_CPLB_ENTRIES + 1;
	CHECK(wb_cplb_setup(&c) == WB_CPLB_ERR_COUNT);
	CHECK(fake_mmr_count() == 0);
}

static void test_disable(void)
{
	reset();
	fake_mmr_set(L1IM_ICTL, 0x206u);
	fake_mmr_set(L1DM_DCTL, L1DM_DCTL_CFG_AB | L1DM_DCTL_ENCPLB |
					L1DM_DCTL_CBYPASS | L1DM_DCTL_RDCHK);
	push_disable(L1DM_DCTL_CFG_AB | L1DM_DCTL_ENCPLB | L1DM_DCTL_CBYPASS |
		     L1DM_DCTL_RDCHK, 0x206u);
	wb_cplb_disable();
	CHECK(fake_mmr_expect("disable", want, nwant));
	/* Read parity checking stays on (S-13a.040). */
	CHECK(fake_mmr_get(L1DM_DCTL) == (L1DM_DCTL_CBYPASS | L1DM_DCTL_RDCHK));
	CHECK(fake_mmr_get(L1IM_ICTL) == L1IM_ICTL_RDCHK);
}

static void test_dcache_ranges(void)
{
	static const struct fake_mmr_access sync_only[] = { FAKE_SYNC };

	reset();
	wb_dcache_flush_range(0x80000010u, 0x40u);
	CHECK(fake_core_flushes() == 3);
	CHECK(fake_core_last_line() == 0x80000040u);
	CHECK(fake_mmr_expect("flush", sync_only, 1));

	reset();
	wb_dcache_invalidate_range(0x80000000u, 0x20u);
	CHECK(fake_core_flushinvs() == 1);
	CHECK(fake_core_last_line() == 0x80000000u);

	reset();
	wb_dcache_flush_range(0x80000000u, 0);
	CHECK(fake_core_flushes() == 0);
	CHECK(fake_mmr_count() == 0);

	/* The last line of the address space ends the loop. */
	reset();
	wb_dcache_invalidate_range(0xFFFFFFF0u, 0x10u);
	CHECK(fake_core_flushinvs() == 1);
	CHECK(fake_core_last_line() == 0xFFFFFFE0u);
}

int main(void)
{
	RUN(test_page_bytes);
	RUN(test_check);
	RUN(test_default_tables_are_valid);
	RUN(test_setup_sequence);
	RUN(test_setup_keeps_other_bits);
	RUN(test_setup_empty_side_stays_off);
	RUN(test_setup_rejects_before_writing);
	RUN(test_disable);
	RUN(test_dcache_ranges);
	return check_summary("test_drv_cplb");
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tests for the DMA channel helpers (src/drivers/bf70x/dma.c). */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x/dma.h"

static const struct wb_time cclk400 = { 400u };

static void reset(void)
{
	fake_mmr_reset();
	fake_core_reset();
}

/*
 * DMA_STAT's IRQDONE, IRQERR and PIRQ are W1C. A memory DMA destination
 * that is enabled completes at once.
 */
static bool dma_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		     wb_mmr_addr addr, uint32_t *value)
{
	unsigned ch;

	(void)ctx;
	(void)width;
	if (op != FAKE_MMR_WRITE)
		return true;
	for (ch = 0; ch < WB_DMA_CHANNELS; ch++) {
		if (addr == DMA_STAT(ch)) {
			*value = fake_mmr_get(addr) & ~(*value & 7u);
			return true;
		}
		if (addr == DMA_CFG(ch) && (*value & DMA_CFG_EN) &&
		    (ch == 17u || ch == 19u || ch == 21u))
			fake_mmr_set(DMA_STAT(ch), DMA_STAT_IRQDONE);
	}
	return true;
}

static void reset_mdma(void)
{
	reset();
	fake_mmr_set_hook(dma_hook, NULL);
}

/* HRM register list: every channel's base. */
static void test_addresses(void)
{
	static const uint32_t base[WB_DMA_CHANNELS] = {
		0x2007C000u, 0x2007D000u, 0x2007E000u, 0x2007F000u,
		0x20080000u, 0x20081000u, 0x20082000u, 0x20083000u,
		0x20084000u, 0x20085000u, 0x20086000u, 0x20087000u,
		0x20088000u, 0x20089000u, 0x2008A000u, 0x2008B000u,
		0x2008C000u, 0x2008D000u, 0x200A0000u, 0x200A1000u,
		0x200A2000u, 0x200A3000u,
	};
	unsigned ch;

	for (ch = 0; ch < WB_DMA_CHANNELS; ch++) {
		CHECK(DMA_BASE(ch) == base[ch]);
		CHECK(DMA_CFG(ch) == base[ch] + 8u);
		CHECK(DMA_STAT(ch) == base[ch] + 0x30u);
	}
	CHECK(WB_DMA_MDMA_SRC(0) == 16u && WB_DMA_MDMA_DST(0) == 17u);
	CHECK(WB_DMA_MDMA_SRC(2) == 20u && WB_DMA_MDMA_DST(2) == 21u);
}

static void test_start_tx(void)
{
	static const struct fake_mmr_access seq[] = {
		FAKE_R32(DMA_CFG(4), 0),
		FAKE_SYNC,	/* after the cache write-back */
		FAKE_W32(DMA_STAT(4), 7u),
		FAKE_W32(DMA_ADDRSTART(4), 0x08001000u),
		FAKE_W32(DMA_XCNT(4), 64u),
		FAKE_W32(DMA_XMOD(4), 4u),
		/* EN, read, stop flow, PSIZE 1 byte, MSIZE 4 bytes. */
		FAKE_W32(DMA_CFG(4), 0x00000201u),
		FAKE_SYNC,
	};
	const struct wb_dma_xfer x = {
		0x08001000u, 64u, WB_DMA_4B, WB_DMA_1B, false, false, false, false,
	};
	const struct wb_dma_xfer y = {
		0x08001000u, 8u, WB_DMA_4B, WB_DMA_4B, false, true, true, false,
	};

	reset();
	CHECK(wb_dma_start(WB_DMA_SPI0_TX, &x) == WB_DMA_OK);
	CHECK(fake_mmr_expect("tx", seq, ARRAY_LEN(seq)));
	/* 256 bytes written back: eight 32-byte lines. */
	CHECK(fake_core_flushes() == 8u);
	CHECK(fake_core_flushinvs() == 0);

	/* an interrupt when the count expires (IRQDONE) and SYNC */
	reset();
	CHECK(wb_dma_start(WB_DMA_SPI0_TX, &y) == WB_DMA_OK);
	CHECK(fake_mmr_get(DMA_CFG(4)) ==
	      (DMA_CFG_EN | DMA_CFG_SYNC | 0x220u | DMA_CFG_INT_X));
}

static void test_start_rx(void)
{
	const struct wb_dma_xfer x = {
		0x08002000u, 32u, WB_DMA_1B, WB_DMA_1B, true, true, false, false,
	};

	reset();
	CHECK(wb_dma_start(WB_DMA_UART0_RX, &x) == WB_DMA_OK);
	CHECK(fake_mmr_get(DMA_CFG(11)) ==
	      (DMA_CFG_EN | DMA_CFG_WNR | DMA_CFG_INT_X));
	CHECK(fake_mmr_get(DMA_XMOD(11)) == 1u);
	CHECK(fake_core_flushes() == 0);
	CHECK(fake_core_flushinvs() == 1u);
}

/* S-13b.070's ring: autobuffer flow, DMA11_CFG = 0x00001007. */
static void test_start_autobuffer(void)
{
	const struct wb_dma_xfer x = {
		0x08002000u, 0x1000u, WB_DMA_1B, WB_DMA_1B, true, false, true,
		true,
	};

	reset();
	CHECK(wb_dma_start(WB_DMA_UART0_RX, &x) == WB_DMA_OK);
	CHECK(fake_mmr_get(DMA_CFG(11)) == 0x00001007u);
	CHECK(fake_mmr_get(DMA_XCNT(11)) == 0x1000u);
	CHECK(fake_mmr_get(DMA_XMOD(11)) == 1u);
	CHECK(fake_mmr_get(DMA_ADDRSTART(11)) == 0x08002000u);
	CHECK(fake_core_flushinvs() == 0x1000u / 32u);
}

static void test_start_errors(void)
{
	struct wb_dma_xfer x = {
		0x08001002u, 4u, WB_DMA_4B, WB_DMA_4B, false, false, false, false,
	};

	reset();
	/* Misaligned for MSIZE. */
	CHECK(wb_dma_start(4, &x) == WB_DMA_ERR_ARG);
	x.addr = 0x08001000u;
	CHECK(wb_dma_start(WB_DMA_CHANNELS, &x) == WB_DMA_ERR_ARG);
	x.count = 0;
	CHECK(wb_dma_start(4, &x) == WB_DMA_ERR_ARG);
	x.count = 0x40000000u;
	CHECK(wb_dma_start(4, &x) == WB_DMA_ERR_ARG);
	x.count = 4;
	x.psize = 4;
	CHECK(wb_dma_start(4, &x) == WB_DMA_ERR_ARG);
	x.psize = 2;
	x.msize = 6;
	CHECK(wb_dma_start(4, &x) == WB_DMA_ERR_ARG);
	CHECK(fake_mmr_count() == 0);

	/* A running channel is not reprogrammed. */
	x.msize = 2;
	fake_mmr_set(DMA_CFG(4), DMA_CFG_EN);
	CHECK(wb_dma_start(4, &x) == WB_DMA_BUSY);
	CHECK(fake_mmr_count() == 1);
}

/*
 * HRM "Stop Flow Mode": a channel disabled mid-work keeps RUN in its
 * waiting state until what it began has returned. A stop mid-work
 * disables the channel, reads DMA_STAT until RUN is idle, and only then
 * clears the status.
 */
static struct {
	bool on;
	unsigned after_off, reads_after_off;
	bool cleared_while_running;
} mid;

static bool mid_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		     wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_WRITE && addr == DMA_CFG(5))
		mid.on = (*value & DMA_CFG_EN) != 0;
	if (op == FAKE_MMR_READ && addr == DMA_STAT(5)) {
		if (mid.on) {
			*value = 2u << 8;		/* still transferring */
		} else {
			mid.reads_after_off++;
			*value = mid.after_off != 0u ? 4u << 8 : 0u;
			if (mid.after_off != 0u)
				mid.after_off--;
		}
		return true;
	}
	if (op == FAKE_MMR_WRITE && addr == DMA_STAT(5) && !mid.on &&
	    mid.after_off != 0u)
		mid.cleared_while_running = true;
	return true;
}

static void test_stop_idle(void)
{
	reset();
	fake_core_set_cycle_step(400);
	fake_mmr_set_hook(mid_hook, NULL);
	memset(&mid, 0, sizeof(mid));
	fake_mmr_set(DMA_CFG(5), DMA_CFG_EN);
	mid.on = true;
	mid.after_off = 2u;
	wb_dma_stop_idle(5, &cclk400);
	CHECK(mid.reads_after_off == 3u && !mid.cleared_while_running);
	CHECK(fake_mmr_get(DMA_CFG(5)) == 0);
}

/* S-13b.082: a per-channel stop; nothing else is touched (S-13b.081). */
static void test_stop(void)
{
	static const struct fake_mmr_access seq[] = {
		FAKE_W32(DMA_CFG(8), 0),
		FAKE_W32(DMA_STAT(8), 7u),
		FAKE_SYNC,
	};

	reset();
	wb_dma_stop(8);
	CHECK(fake_mmr_expect("stop", seq, ARRAY_LEN(seq)));
	fake_mmr_clear_log();
	wb_dma_stop(WB_DMA_CHANNELS);
	CHECK(!wb_dma_enabled(WB_DMA_CHANNELS));
	CHECK(fake_mmr_count() == 0);
}

/* No helper writes a DMA9 register unless asked for channel 9. */
static void test_dma9_untouched(void)
{
	const struct wb_dma_xfer x = {
		0x08010000u, 16u, WB_DMA_4B, WB_DMA_4B, true, true, false, false,
	};
	size_t i, n;
	const struct fake_mmr_access *log;

	reset_mdma();
	CHECK(wb_dma_start(WB_DMA_MDMA_DST(0), &x) == WB_DMA_OK);
	wb_dma_stop(WB_DMA_MDMA_DST(0));
	wb_dma_stop(WB_DMA_SPI2_TX);
	log = fake_mmr_log();
	n = fake_mmr_count();
	for (i = 0; i < n; i++)
		CHECK(log[i].addr < DMA_BASE(9) ||
		      log[i].addr >= DMA_BASE(10));
}

/*
 * S-13b.083's descriptor arrays: each descriptor the channel value with the
 * memory word size from the buffer's alignment and length, the count in
 * those words, the modify that size (0 for one place); the last one stop
 * flow, no further fetch, the interrupt at its count; the start
 * DMA_DSCPTR_CUR then DMA_CFG and nothing else.
 */
static void test_desc_array(void)
{
	static const struct {
		uint32_t addr;
		uint64_t n;
		unsigned msize;
	} cases[] = {
		{ 0x11900000u, 12u, 2u }, { 0x11900000u, 64u, 5u }, { 0x11900010u, 48u, 4u },
		{ 0x11900008u, 24u, 3u }, { 0x11900002u, 6u, 1u }, { 0x11900001u, 3u, 0u },
		{ 0x8803D020u, 96u, 5u },
	};
	struct wb_dma_desc a[3];
	unsigned i;

	for (i = 0; i < ARRAY_LEN(cases); i++) {
		struct wb_dma_desc d;

		CHECK(wb_dma_desc_set(&d, WB_DMA_ARRAY_RECV, cases[i].addr, cases[i].n, false) ==
		      cases[i].n);
		CHECK(d.addr == cases[i].addr &&
		      d.cfg == (WB_DMA_ARRAY_RECV | cases[i].msize << DMA_CFG_MSIZE_SHIFT) &&
		      d.count == (uint32_t)(cases[i].n >> cases[i].msize) &&
		      d.mod == 1u << cases[i].msize);
	}
	/* one place, a receive's dropped words */
	CHECK(wb_dma_desc_set(&a[0], WB_DMA_ARRAY_RECV, 0x11900040u, 512u, true) == 512u);
	CHECK(a[0].mod == 0u && a[0].count == 16u);
	/* a count too long for one descriptor: the most it takes */
	CHECK(wb_dma_desc_set(&a[1], WB_DMA_ARRAY_RECV, 0x11900040u, (uint64_t)1 << 40, true) ==
	      (uint64_t)WB_DMA_DESC_COUNT_MAX << 5);
	CHECK(a[1].count == WB_DMA_DESC_COUNT_MAX);
	/* the channel values: enabled, 32-bit peripheral words, the array's
	 * flow with four words fetched, SYNC clear; a receive writes memory */
	CHECK(WB_DMA_ARRAY_SEND == (DMA_CFG_EN | 2u << DMA_CFG_PSIZE_SHIFT | DMA_CFG_FLOW_ARRAY |
				    DMA_CFG_NDSIZE_4));
	CHECK(WB_DMA_ARRAY_RECV == (WB_DMA_ARRAY_SEND | DMA_CFG_WNR));
	CHECK(!(WB_DMA_ARRAY_SEND & DMA_CFG_SYNC));
	(void)wb_dma_desc_set(&a[2], WB_DMA_ARRAY_SEND, 0x11900000u, 8u, false);
	wb_dma_desc_last(&a[2]);
	CHECK(a[2].cfg == ((WB_DMA_ARRAY_SEND & ~(DMA_CFG_FLOW_MASK | DMA_CFG_NDSIZE_MASK)) |
			   DMA_CFG_INT_X | 3u << DMA_CFG_MSIZE_SHIFT));
	{
		static const struct fake_mmr_access start[] = {
			FAKE_W32(DMA_DSCPTR_CUR(4), 0x11900100u),
			FAKE_W32(DMA_CFG(4), WB_DMA_ARRAY_SEND),
			FAKE_SYNC,
		};

		reset();
		wb_dma_array_start(4u, (const struct wb_dma_desc *)(uintptr_t)0x11900100u,
				   WB_DMA_ARRAY_SEND);
		CHECK(fake_mmr_expect("array start", start, ARRAY_LEN(start)));
	}
}

/* The cache around a transfer: nothing in L1; elsewhere every line of the
 * range, written back for a send and also invalidated for a receive, with
 * interrupts masked and the mask put back. */
static void test_cache(void)
{
	reset();
	fake_core_set_imask(0xFFE0u);
	wb_dma_cache_send(0x11900000u, 4096u);
	wb_dma_cache_recv(0x11800000u, 4096u);
	CHECK(fake_core_flushes() == 0u && fake_core_flushinvs() == 0u);
	wb_dma_cache_send(0x8803D010u, 64u);
	CHECK(fake_core_flushes() == 3u && fake_core_flushinvs() == 0u);
	wb_dma_cache_recv(0x8803D020u, 96u);
	CHECK(fake_core_flushinvs() == 3u && fake_core_last_line() == 0x8803D060u);
	CHECK(fake_core_imask() == 0xFFE0u);
	CHECK(wb_dma_in_l1(0x11800000u) && wb_dma_in_l1(0x11BFFFFFu) && !wb_dma_in_l1(0x11C00000u) &&
	      !wb_dma_in_l1(0x08000000u));
}

int main(void)
{
	RUN(test_addresses);
	RUN(test_start_tx);
	RUN(test_start_rx);
	RUN(test_start_autobuffer);
	RUN(test_start_errors);
	RUN(test_stop_idle);
	RUN(test_stop);
	RUN(test_dma9_untouched);
	RUN(test_desc_array);
	RUN(test_cache);
	return check_summary("test_drv_dma");
}

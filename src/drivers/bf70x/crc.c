/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/drivers/bf70x_crc.h"

#include <stddef.h>

#include "waffle/drivers/mmr.h"

static void put(wb_mmr_addr reg, uint32_t v)
{
	wb_mmr_write32(reg, v);
	wb_mmr_sync();
}

void wb_crc_init(struct wb_crc *c, const struct wb_time *t)
{
	c->time = t;
	c->src = NULL;
	c->len = 0;
}

/* Disabled, out of line with the stream: DMA18 first (HRM "CRC Programming
 * Model", restriction 2), then the block, then the status. */
static void stop(void)
{
	wb_dma_stop(WB_CRC_DMA);
	put(CRC0_CTL, 0u);
	put(CRC0_STAT, CRC_STAT_W1C);
}

/* Until CRC0_STAT has one of bits set; an error once DMA18 reports one
 * (dma: it runs), or once the time is up. The status is read once more
 * after the deadline. */
static int wait(const struct wb_time *t, uint32_t bits, uint32_t us, bool dma)
{
	const uint64_t end = wb_time_deadline(t, us);

	for (;;) {
		const bool late = wb_time_passed(end);

		if ((wb_mmr_read32(CRC0_STAT) & bits) != 0u)
			return WB_CRC_OK;
		if (dma && (wb_mmr_read32(DMA_STAT(WB_CRC_DMA)) &
			    DMA_STAT_IRQERR) != 0u)
			return WB_CRC_ERR_HW;
		if (late)
			return WB_CRC_TIMEOUT;
	}
}

/* HRM Table 19-10: 4 to 32 bytes */
uint8_t wb_crc_msize(uint32_t addr, uint32_t len)
{
	uint8_t m = WB_DMA_4B;

	while (m < 5u && ((addr | len) & ((2u << m) - 1u)) == 0u)
		m++;
	return m;
}

int wb_crc_run(struct wb_crc *c, uint32_t poly, uint32_t opts,
	       uint32_t *state, const void *data, uint32_t len)
{
	const uint32_t addr = (uint32_t)(uintptr_t)data;
	struct wb_dma_xfer x = { 0 };
	int rc;

	if (((addr | len) & 3u) != 0u || len == 0u || len > WB_CRC_MAX ||
	    (opts & ~CRC_CTL_OPTIONS) != 0u)
		return WB_CRC_ERR_ARG;
	if (wb_dma_enabled(WB_CRC_DMA))
		return WB_CRC_BUSY;
	c->src = data;
	c->len = len;

	/* the options first: POLYMIRR and RSLTMIRR act on the writes below */
	put(CRC0_CTL, opts);
	put(CRC0_STAT, CRC_STAT_W1C);
	put(CRC0_INEN, 0u);
	put(CRC0_POLY, poly);
	rc = wait(c->time, CRC_STAT_LUTDONE, WB_CRC_LUT_US, false);
	if (rc == WB_CRC_OK) {
		put(CRC0_DCNT, len / 4u);
		put(CRC0_DCNTRLD, 0u);
		put(CRC0_COMP, 0u);	/* never checked: CMPERR is cleared */
		put(CRC0_RESULT_CUR, *state);
		/* the block before the channel (restriction 1) */
		put(CRC0_CTL, opts | CRC_CTL_OPMODE_SCAN | CRC_CTL_BLKEN);
		x.addr = addr;
		x.msize = wb_crc_msize(addr, len);
		x.psize = WB_DMA_4B;
		x.count = len >> x.msize;
		rc = wb_dma_start(WB_CRC_DMA, &x) == WB_DMA_OK ?
		     wait(c->time, CRC_STAT_DCNTEXP,
			  WB_CRC_TIMEOUT_US + len / 16u, true) : WB_CRC_ERR_HW;
	}
	if (rc == WB_CRC_OK)
		*state = wb_mmr_read32(CRC0_RESULT_FIN);
	stop();
	return rc;
}

void wb_crc_end(struct wb_crc *c)
{
	stop();
	put(DMA_ADDRSTART(WB_CRC_DMA), 0u);
	put(DMA_XCNT(WB_CRC_DMA), 0u);
	put(DMA_XMOD(WB_CRC_DMA), 0u);
	put(CRC0_INEN, 0u);
	put(CRC0_DCNT, 0u);
	put(CRC0_DCNTRLD, 0u);
	put(CRC0_COMP, 0u);
	put(CRC0_RESULT_CUR, 0u);
	put(CRC0_RESULT_FIN, 0u);
	put(CRC0_POLY, 0u);
	c->src = NULL;
	c->len = 0;
}

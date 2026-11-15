/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DMA channel helpers (see waffle/drivers/bf70x_dma.h). */
#include "waffle/drivers/bf70x_dma.h"

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_cplb.h"
#include "waffle/drivers/mmr.h"

#define STAT_W1C (DMA_STAT_IRQDONE | DMA_STAT_IRQERR | DMA_STAT_PIRQ)

int wb_dma_start(unsigned ch, const struct wb_dma_xfer *x)
{
	uint32_t cfg, bytes;

	if (ch >= WB_DMA_CHANNELS || x->count == 0 || x->msize > 5u ||
	    x->psize > 3u || (x->addr & ((1u << x->msize) - 1u)) != 0 ||
	    x->count > (UINT32_MAX >> x->msize))
		return WB_DMA_ERR_ARG;
	if (wb_mmr_read32(DMA_CFG(ch)) & DMA_CFG_EN)
		return WB_DMA_BUSY;

	bytes = x->count << x->msize;
	if (x->to_mem)
		wb_dcache_invalidate_range(x->addr, bytes);
	else
		wb_dcache_flush_range(x->addr, bytes);

	cfg = DMA_CFG_EN |
	      (x->autobuffer ? DMA_CFG_FLOW_AUTO : DMA_CFG_FLOW_STOP) |
	      ((uint32_t)x->psize << DMA_CFG_PSIZE_SHIFT) |
	      ((uint32_t)x->msize << DMA_CFG_MSIZE_SHIFT);
	if (x->to_mem)
		cfg |= DMA_CFG_WNR;
	if (x->irq)
		cfg |= DMA_CFG_INT_X;
	if (x->sync)
		cfg |= DMA_CFG_SYNC;
	wb_mmr_write32(DMA_STAT(ch), STAT_W1C);
	wb_mmr_write32(DMA_ADDRSTART(ch), x->addr);
	wb_mmr_write32(DMA_XCNT(ch), x->count);
	wb_mmr_write32(DMA_XMOD(ch), 1u << x->msize);
	wb_mmr_write32(DMA_CFG(ch), cfg);
	wb_mmr_sync();
	return WB_DMA_OK;
}

void wb_dma_stop_idle(unsigned ch, const struct wb_time *t)
{
	uint64_t end;

	if (ch >= WB_DMA_CHANNELS)
		return;
	wb_mmr_write32(DMA_CFG(ch), 0);
	wb_mmr_sync();
	end = wb_time_deadline(t, 100u);
	while ((wb_mmr_read32(DMA_STAT(ch)) & DMA_STAT_RUN_MASK) != 0u &&
	       !wb_time_passed(end))
		;
	wb_mmr_write32(DMA_STAT(ch), STAT_W1C);
	wb_mmr_sync();
}

void wb_dma_stop(unsigned ch)
{
	if (ch >= WB_DMA_CHANNELS)
		return;
	wb_mmr_write32(DMA_CFG(ch), 0);
	wb_mmr_write32(DMA_STAT(ch), STAT_W1C);
	wb_mmr_sync();
}

bool wb_dma_enabled(unsigned ch)
{
	return ch < WB_DMA_CHANNELS &&
	       (wb_mmr_read32(DMA_CFG(ch)) & DMA_CFG_EN) != 0;
}

/* Memory word sizes (DMA_CFG.MSIZE) 1 to 32 bytes, HRM Table 19-10. */
#define MSIZE_MAX 5u

uint64_t wb_dma_desc_set(struct wb_dma_desc *d, uint32_t cfg, uint32_t addr,
			 uint64_t n, bool same)
{
	unsigned m = MSIZE_MAX;
	uint64_t count;

	while (m > 0u && ((addr | (uint32_t)n) & ((1u << m) - 1u)) != 0u)
		m--;
	count = n >> m;
	if (count > WB_DMA_DESC_COUNT_MAX)
		count = WB_DMA_DESC_COUNT_MAX;
	d->addr = addr;
	d->cfg = (cfg & ~DMA_CFG_MSIZE_MASK) | (uint32_t)m << DMA_CFG_MSIZE_SHIFT;
	d->count = (uint32_t)count;
	d->mod = same ? 0u : 1u << m;
	return count << m;
}

void wb_dma_desc_last(struct wb_dma_desc *d)
{
	d->cfg = (d->cfg & ~(DMA_CFG_FLOW_MASK | DMA_CFG_NDSIZE_MASK)) |
		 DMA_CFG_FLOW_STOP | DMA_CFG_INT_X;
}

void wb_dma_array_start(unsigned ch, const struct wb_dma_desc *a, uint32_t cfg)
{
	if (ch >= WB_DMA_CHANNELS)
		return;
	wb_mmr_write32(DMA_DSCPTR_CUR(ch), (uint32_t)(uintptr_t)a);
	wb_mmr_write32(DMA_CFG(ch), cfg);
	wb_mmr_sync();
}

bool wb_dma_in_l1(uint32_t addr)
{
	return addr >= 0x11800000u && addr < 0x11C00000u;
}

void wb_dma_cache_send(uint32_t addr, uint32_t n)
{
	uint32_t mask;

	if (n == 0u || wb_dma_in_l1(addr))
		return;
	mask = wb_core_cli();
	wb_dcache_flush_range(addr, n);		/* ends with SSYNC */
	wb_core_sti(mask);
}

void wb_dma_cache_recv(uint32_t addr, uint32_t n)
{
	uint32_t mask;

	if (n == 0u || wb_dma_in_l1(addr))
		return;
	mask = wb_core_cli();
	wb_dcache_invalidate_range(addr, n);	/* FLUSHINV, then SSYNC */
	wb_core_sti(mask);
}

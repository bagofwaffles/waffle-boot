/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_DMA_H
#define WAFFLE_DRIVERS_BF70X_DMA_H

/*
 * DMA channels, one-dimensional transfers in stop or autobuffer flow
 * (ADSP-BF70x Hardware Reference chapter "Direct Memory Access (DMA)").
 *
 * Channels are fixed by the hardware (HRM Table 19-7, S-13b.080). Nothing
 * here touches a channel it was not asked to: there is no reset or stop of
 * all channels, since DMA9's XMOD and YMOD carry the display timestamp on
 * the master until the display set-up has read them (S-13b.081).
 *
 * Data cache: wb_dma_start() writes back the buffer for a transmit and
 * writes back and invalidates it for a receive; after a receive the
 * caller invalidates the buffer again (wb_dcache_invalidate_range) before
 * reading it.
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/drivers/bf70x/time.h"

#define WB_DMA_CHANNELS   22u

/* Channel numbers (HRM Table 19-7). */
#define WB_DMA_SPI0_TX    4u
#define WB_DMA_SPI0_RX    5u
#define WB_DMA_SPI2_TX    8u
#define WB_DMA_SPI2_RX    9u
#define WB_DMA_UART0_TX   10u
#define WB_DMA_UART0_RX   11u
#define WB_DMA_UART1_TX   12u
#define WB_DMA_UART1_RX   13u
#define WB_DMA_MDMA_SRC(s) (16u + 2u * (s))
#define WB_DMA_MDMA_DST(s) (17u + 2u * (s))

/* Channel registers. */
#define DMA_BASE(ch)  ((ch) < 18u ? 0x2007C000u + 0x1000u * (ch) : \
				   0x200A0000u + 0x1000u * ((ch) - 18u))
#define DMA_DSCPTR_NXT(ch) (DMA_BASE(ch) + 0x00u)
#define DMA_ADDRSTART(ch)  (DMA_BASE(ch) + 0x04u)
#define DMA_CFG(ch)        (DMA_BASE(ch) + 0x08u)
#define DMA_XCNT(ch)       (DMA_BASE(ch) + 0x0Cu)
#define DMA_XMOD(ch)       (DMA_BASE(ch) + 0x10u)
#define DMA_YCNT(ch)       (DMA_BASE(ch) + 0x14u)
#define DMA_YMOD(ch)       (DMA_BASE(ch) + 0x18u)
#define DMA_DSCPTR_CUR(ch) (DMA_BASE(ch) + 0x24u)
#define DMA_ADDR_CUR(ch)   (DMA_BASE(ch) + 0x2Cu)
#define DMA_STAT(ch)       (DMA_BASE(ch) + 0x30u)
#define DMA_XCNT_CUR(ch)   (DMA_BASE(ch) + 0x34u)

/* DMA_CFG. */
/* CRC1 sits in line with MDMA2 (HRM, the CRC chapter): enabled, the stream's
 * destination gets nothing. */
#define CRC1_CTL            0x200B1000u

#define DMA_CFG_EN          (1u << 0)
#define DMA_CFG_WNR         (1u << 1)	/* write to memory (receive) */
#define DMA_CFG_SYNC        (1u << 2)
#define DMA_CFG_PSIZE_SHIFT 4u
#define DMA_CFG_MSIZE_SHIFT 8u
#define DMA_CFG_FLOW_STOP   (0u << 12)
#define DMA_CFG_FLOW_AUTO   (1u << 12)	/* autobuffer: restarts at the end */
#define DMA_CFG_FLOW_ARRAY  (5u << 12)	/* descriptor array */
#define DMA_CFG_FLOW_MASK   (7u << 12)
#define DMA_CFG_MSIZE_MASK  (7u << 8)
#define DMA_CFG_NDSIZE_4    (3u << 16)	/* four descriptor elements fetched */
#define DMA_CFG_NDSIZE_MASK (7u << 16)
#define DMA_CFG_INT_X       (1u << 20)	/* interrupt when XCNT expires */

/* DMA_STAT. IRQDONE, IRQERR and PIRQ are W1C. */
#define DMA_STAT_IRQDONE    (1u << 0)
#define DMA_STAT_IRQERR     (1u << 1)
#define DMA_STAT_PIRQ       (1u << 2)
#define DMA_STAT_ERRC_SHIFT 4u
#define DMA_STAT_ERRC_MASK  (7u << 4)
#define DMA_STAT_RUN_MASK   (7u << 8)
#define DMA_STAT_FIFOFILL_MASK (7u << 16)	/* 0: the channel's FIFO empty */

/* Element sizes, as log2 of the bytes. */
#define WB_DMA_1B 0u
#define WB_DMA_2B 1u
#define WB_DMA_4B 2u
#define WB_DMA_8B 3u

struct wb_dma_xfer {
	uint32_t addr;		/* memory address, aligned to msize */
	uint32_t count;		/* elements of msize */
	uint8_t msize;		/* memory element, log2 bytes, 0 to 5 */
	uint8_t psize;		/* peripheral element, log2 bytes, 0 to 3 */
	bool to_mem;		/* receive (peripheral or MDMA destination) */
	/*
	 * Interrupt when the count expires (DMA_CFG.INT 1). DMA_STAT.IRQDONE
	 * is set only when the channel issues that interrupt (HRM DMA_STAT),
	 * so any wait on IRQDONE needs it.
	 */
	bool irq;
	/* DMA_CFG.SYNC: the DMA FIFO cleared before the work unit and, for a
	 * transmit, the interrupt only once the data has gone to the
	 * peripheral (HRM DMA_CFG.SYNC) */
	bool sync;
	/* Autobuffer flow (HRM "Autobuffer Mode"): the channel starts over at
	 * addr when the count expires, until it is disabled. */
	bool autobuffer;
};

#define WB_DMA_OK         0
#define WB_DMA_ERR_ARG   (-1)
#define WB_DMA_BUSY      (-2)	/* the channel is enabled */
#define WB_DMA_TIMEOUT   (-3)
#define WB_DMA_ERR_HW    (-4)	/* DMA_STAT.IRQERR */

int wb_dma_start(unsigned ch, const struct wb_dma_xfer *x);

/* Disable one channel and clear its status. Other channels are untouched. */
void wb_dma_stop(unsigned ch);
/*
 * The same for a channel stopped in the middle of its work: HRM "Stop Flow
 * Mode" keeps DMA_STAT.RUN in its waiting state until every transaction
 * the channel began has returned, and the channel must not be set up again
 * before; so the status is cleared once RUN reads idle, or after 100 us.
 */
void wb_dma_stop_idle(unsigned ch, const struct wb_time *t);
bool wb_dma_enabled(unsigned ch);

/*
 * Descriptor-array flow (HRM "Descriptor-Array Flow Mode", Table 19-14):
 * the channel fetches four words per work unit, start address, configuration,
 * count and modify, from DMA_DSCPTR_CUR on, until a descriptor whose flow is
 * stop. S-13b.083 gives the bridge's channel values: enabled, 32-bit
 * peripheral words, the array's flow with four words fetched, SYNC clear.
 */
struct wb_dma_desc {
	uint32_t addr;
	uint32_t cfg;
	uint32_t count;		/* in memory words (the descriptor's MSIZE) */
	uint32_t mod;		/* bytes */
};

#define WB_DMA_ARRAY_SEND 0x00035021u
#define WB_DMA_ARRAY_RECV 0x00035023u

/* A descriptor's most memory words (DMA_XCNT is 32 bits): the count of a
 * longer buffer is split. */
#define WB_DMA_DESC_COUNT_MAX 0x80000000u

/*
 * One descriptor for n bytes at addr under the channel value cfg: the memory
 * word size the largest of 1 to 32 bytes that divides both addr and n, and the
 * modify that size, or 0 with `same` (every word to the same place: a
 * receive's discarded bytes). The count is n in those words, at most
 * WB_DMA_DESC_COUNT_MAX (the caller splits). Returns the bytes it covers.
 */
uint64_t wb_dma_desc_set(struct wb_dma_desc *d, uint32_t cfg, uint32_t addr,
			 uint64_t n, bool same);
/*
 * The array's last descriptor: stop flow and no further fetch (the HRM
 * flags a stop flow with a descriptor size other than 0), the interrupt at
 * the end of its count, which sets the channel's done status.
 */
void wb_dma_desc_last(struct wb_dma_desc *d);
/*
 * Start a descriptor array: DMA_DSCPTR_CUR, then DMA_CFG = cfg, nothing else
 * (S-13b.083). The array is in memory the channel reads directly: L1, or
 * written back by the caller.
 */
void wb_dma_array_start(unsigned ch, const struct wb_dma_desc *a, uint32_t cfg);

/*
 * The cache around a DMA transfer (S-13b.083): the lines of [addr, addr + n)
 * written back, for a send, or written back and invalidated, for a receive,
 * one line at a time with interrupts masked, then a system synchronization.
 * Nothing for memory in L1, which is never cached.
 */
bool wb_dma_in_l1(uint32_t addr);
void wb_dma_cache_send(uint32_t addr, uint32_t n);
void wb_dma_cache_recv(uint32_t addr, uint32_t n);

#endif /* WAFFLE_DRIVERS_BF70X_DMA_H */

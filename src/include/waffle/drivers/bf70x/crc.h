/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_CRC_H
#define WAFFLE_DRIVERS_BF70X_CRC_H

/*
 * The CRC peripheral CRC0 (ADSP-BF70x Hardware Reference, chapter 14) in
 * memory scan compute-and-compare mode, one operation at a time, polled:
 * the 32-bit CRC of whole words that memory DMA stream 1's source channel
 * (DMA18) reads from memory. CRC0 sits in line with MDMA1 (HRM Table 8-2,
 * "Memory DMA Stream 1 Source / CRC0 Input Channel"); CRC1 sits in line
 * with MDMA2, the display's stream (S-05.016), and is never used here.
 * Scan mode needs no destination channel, so DMA19 is never enabled.
 *
 * Every operation sets the peripheral up, waits for it and leaves it
 * disabled (CRC0_CTL 0) with DMA18 disabled and both status registers
 * cleared, on success and on failure: nothing stays in line with the
 * stream between operations.
 *
 * Data cache: DMA18 reads memory. wb_dma_start() writes the range back
 * first; data pages are write-through anyway and L1 is not cached. The
 * peripheral writes no memory in scan mode.
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/drivers/bf70x/dma.h"
#include "waffle/drivers/bf70x/time.h"

/* HRM Table A-5. */
#define CRC0_BASE        0x200B0000u
#define CRC0_CTL         (CRC0_BASE + 0x00u)
#define CRC0_DCNT        (CRC0_BASE + 0x04u)
#define CRC0_DCNTRLD     (CRC0_BASE + 0x08u)
#define CRC0_COMP        (CRC0_BASE + 0x14u)
#define CRC0_INEN        (CRC0_BASE + 0x20u)
#define CRC0_POLY        (CRC0_BASE + 0x2Cu)
#define CRC0_STAT        (CRC0_BASE + 0x40u)
#define CRC0_RESULT_FIN  (CRC0_BASE + 0x4Cu)
#define CRC0_RESULT_CUR  (CRC0_BASE + 0x50u)

/* CRC_CTL (Table 14-6). */
#define CRC_CTL_BLKEN    (1u << 0)
#define CRC_CTL_OPMODE_MASK (15u << 4)
#define CRC_CTL_OPMODE_SCAN (3u << 4)	/* CRC compute/compare memory scan */
#define CRC_CTL_BITMIRR  (1u << 16)	/* the bits of each input byte */
#define CRC_CTL_BYTMIRR  (1u << 17)	/* the bytes of each input word */
#define CRC_CTL_W16SWP   (1u << 18)	/* the 16-bit halves of each word */
#define CRC_CTL_RSLTMIRR (1u << 20)	/* CRC_RESULT_CUR and _FIN reversed */
#define CRC_CTL_POLYMIRR (1u << 21)	/* CRC_POLY reversed */
/* The options an operation may set. */
#define CRC_CTL_OPTIONS  (CRC_CTL_BITMIRR | CRC_CTL_BYTMIRR | CRC_CTL_W16SWP | \
			  CRC_CTL_RSLTMIRR | CRC_CTL_POLYMIRR)

/* CRC_STAT (Table 14-18). CMPERR and DCNTEXP are W1C. */
#define CRC_STAT_CMPERR  (1u << 1)
#define CRC_STAT_DCNTEXP (1u << 4)
#define CRC_STAT_LUTDONE (1u << 19)
#define CRC_STAT_W1C     (CRC_STAT_CMPERR | CRC_STAT_DCNTEXP)

/* The stream CRC0 reads through. */
#define WB_CRC_DMA       WB_DMA_MDMA_SRC(1u)

/* An operation's most bytes, and its waits: the look-up table (16 cycles,
 * HRM "Look-up Table"), then the data, WB_CRC_TIMEOUT_US and a microsecond
 * for every 16 bytes. */
#define WB_CRC_MAX       0x100000u
#define WB_CRC_LUT_US    100u
#define WB_CRC_TIMEOUT_US 1000u

#define WB_CRC_OK        0
#define WB_CRC_ERR_ARG  (-1)
#define WB_CRC_BUSY     (-2)	/* DMA18 is enabled: nothing was touched */
#define WB_CRC_TIMEOUT  (-3)
#define WB_CRC_ERR_HW   (-4)	/* DMA18 reported an error */

struct wb_crc {
	const struct wb_time *time;
	/* The last operation's bytes: len at src. */
	const void *src;
	uint32_t len;
};

void wb_crc_init(struct wb_crc *c, const struct wb_time *t);

/*
 * The CRC of len bytes at data (4-byte aligned, a whole number of words,
 * at most WB_CRC_MAX) taken on from *state, with polynomial poly and the
 * options opts (CRC_CTL_OPTIONS bits): *state is written to CRC_RESULT_CUR
 * and replaced by CRC_RESULT_FIN on success, both as the registers hold
 * them. *state is unchanged on any failure.
 */
int wb_crc_run(struct wb_crc *c, uint32_t poly, uint32_t opts,
	       uint32_t *state, const void *data, uint32_t len);

/* The memory transfer size wb_crc_run() gives DMA18 for len bytes at addr:
 * the largest that both allow, WB_DMA_4B (4 bytes) to 5 (32 bytes). */
uint8_t wb_crc_msize(uint32_t addr, uint32_t len);

/*
 * CRC0 and DMA18 as reset leaves them: every register the driver writes
 * back to its reset value, the status cleared. CRC_STAT.LUTDONE, read-only,
 * reads 1 afterwards: only a reset clears it.
 */
void wb_crc_end(struct wb_crc *c);

#endif /* WAFFLE_DRIVERS_BF70X_CRC_H */

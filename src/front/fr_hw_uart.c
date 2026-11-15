/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The PSoC's UART (S-13b.070: UART0, 1 Mbaud, 8N1, no flow control), set
 * up as the board's driver does with opxy_uart_psoc, and run polled: each
 * read or write moves what the controller holds into a small ring, a write
 * fills the transmit FIFO and no more. Target only.
 */
#include "fr_hw.h"

#include "waffle/drivers/bf70x_uart.h"
#include "waffle/drivers/mmr.h"

#define PSOC_UART 0u				/* S-13b.070 */
#define PSOC_CTL  UART_CTL_WLS8			/* 0x301 with EN */
#define PSOC_CLK  (UART_CLK_EDBO | 100u)	/* SCLK0 100 MHz / 100 */
#define HW_FIFO   16u				/* bytes one call moves, at most */
#define RING      256u				/* a power of two */

static uint8_t ring[RING];
static uint32_t head, tail;

/* RX FIFO to the ring; line errors cleared. A full ring leaves the bytes
 * in the controller (which then overruns: the frame's check drops it). */
static void take(void)
{
	uint32_t stat = wb_mmr_read32(UART_STAT(PSOC_UART));

	if (stat & UART_STAT_ERRORS)
		wb_mmr_write32(UART_STAT(PSOC_UART), stat & UART_STAT_ERRORS);
	for (unsigned i = 0; i < HW_FIFO && head - tail < RING; i++) {
		if (!(stat & UART_STAT_DR))
			break;
		ring[head & (RING - 1u)] = (uint8_t)wb_mmr_read32(UART_RBR(PSOC_UART));
		head++;
		stat = wb_mmr_read32(UART_STAT(PSOC_UART));
	}
}

void fr_hw_uart_take(void)
{
	take();
}

void fr_hw_uart_start(void)
{
	head = tail = 0;
	wb_mmr_write32(UART_CTL(PSOC_UART), 0);
	wb_mmr_write32(UART_IMSK_CLR(PSOC_UART), 0xFFu);	/* polled */
	wb_mmr_write32(UART_CLK(PSOC_UART), PSOC_CLK);
	wb_mmr_write32(UART_CTL(PSOC_UART), PSOC_CTL | UART_CTL_EN);
	wb_mmr_write32(UART_STAT(PSOC_UART), UART_STAT_ERRORS);
	wb_mmr_sync();
}

static size_t uart_read(void *ctx, uint8_t *buf, size_t max)
{
	size_t n = 0;

	(void)ctx;
	take();
	while (n < max && head != tail) {
		buf[n++] = ring[tail & (RING - 1u)];
		tail++;
	}
	if (n != 0)
		take();
	return n;
}

static size_t uart_write(void *ctx, const uint8_t *buf, size_t n)
{
	size_t i;

	(void)ctx;
	take();
	for (i = 0; i < n && i < HW_FIFO; i++) {
		if (!(wb_mmr_read32(UART_STAT(PSOC_UART)) & UART_STAT_THRE))
			break;
		wb_mmr_write32(UART_THR(PSOC_UART), buf[i]);
	}
	return i;
}

static bool uart_idle(void *ctx)
{
	(void)ctx;
	take();
	return (wb_mmr_read32(UART_STAT(PSOC_UART)) & UART_STAT_TEMT) != 0u;
}

const struct fr_bytes fr_hw_psoc = { uart_read, uart_write, uart_idle, NULL };

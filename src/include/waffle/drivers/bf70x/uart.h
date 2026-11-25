/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_UART_H
#define WAFFLE_DRIVERS_BF70X_UART_H

/*
 * UARTs with software receive and transmit rings (ADSP-BF70x Hardware
 * Reference chapter "UART").
 *
 * The board passes whole UART_CTL and UART_CLK values (S-13b.070, .071):
 * the line format and, through UART_CTL.ARTS/ACTS/FCPOL, the hardware
 * flow control. The driver adds UART_CTL.EN.
 *
 * wb_uart_service() moves bytes between the controller and the rings. It
 * runs from wb_uart_isr() (attach it with wb_sec_attach() to the UART's
 * status source, SEC 48 for UART0 and 51 for UART1, and call
 * wb_uart_use_irq()), or from a poll loop. When the receive ring is full,
 * received bytes stay in the controller's FIFO; with ARTS set that holds
 * RTS off, so nothing is lost; without it the controller overruns.
 *
 * wb_uart_init_dma() instead receives through the UART's receive DMA
 * channel (HRM Table 19-7: DMA11 for UART0, DMA13 for UART1) in autobuffer
 * mode over the rx ring, which is then the DMA's: the channel fills it
 * whether or not anything services the UART (S-13b.070). The write
 * position comes from the channel's current count at each service. The
 * ring must hold more bytes than the line can carry in the longest pause
 * the caller allows; how a ring overwritten before it was read shows is
 * said at struct wb_uart. Transmit is the same in both modes.
 *
 * read, room and write have struct s06_port_ops's shapes, with the
 * struct wb_uart as ctx: { wb_uart_read, wb_uart_room, wb_uart_write }.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/drivers/bf70x/time.h"

#define WB_UART_COUNT 2u

/* The UARTs' status sources on the SEC. */
#define WB_SID_UART0_STAT 48u
#define WB_SID_UART1_STAT 51u

#define UART_BASE(n)       (0x2005F000u + 0x400u * (n))
#define UART_CTL(n)        (UART_BASE(n) + 0x04u)
#define UART_STAT(n)       (UART_BASE(n) + 0x08u)
#define UART_CLK(n)        (UART_BASE(n) + 0x10u)
#define UART_IMSK(n)       (UART_BASE(n) + 0x14u)
#define UART_IMSK_SET(n)   (UART_BASE(n) + 0x18u)
#define UART_IMSK_CLR(n)   (UART_BASE(n) + 0x1Cu)
#define UART_RBR(n)        (UART_BASE(n) + 0x20u)
#define UART_THR(n)        (UART_BASE(n) + 0x24u)

/* UART_CTL. */
#define UART_CTL_EN        (1u << 0)
#define UART_CTL_LOOP_EN   (1u << 1)
#define UART_CTL_MOD_MASK  (3u << 4)
#define UART_CTL_WLS8      (3u << 8)
#define UART_CTL_STB       (1u << 12)
#define UART_CTL_PEN       (1u << 14)
#define UART_CTL_FCPOL     (1u << 22)
#define UART_CTL_ARTS      (1u << 27)
#define UART_CTL_ACTS      (1u << 28)

/* UART_STAT. */
#define UART_STAT_DR       (1u << 0)
#define UART_STAT_OE       (1u << 1)
#define UART_STAT_PE       (1u << 2)
#define UART_STAT_FE       (1u << 3)
#define UART_STAT_BI       (1u << 4)
#define UART_STAT_THRE     (1u << 5)
#define UART_STAT_TEMT     (1u << 7)
#define UART_STAT_ERRORS   (UART_STAT_OE | UART_STAT_PE | UART_STAT_FE | \
			    UART_STAT_BI)

/* UART_CLK: baud = SCLK0 / (16 * DIV), or SCLK0 / DIV with EDBO. */
#define UART_CLK_EDBO      (1u << 31)

/* UART_IMSK. */
#define UART_IMSK_ERBFI    (1u << 0)
#define UART_IMSK_ETBEI    (1u << 1)
#define UART_IMSK_ELSI     (1u << 2)
#define UART_IMSK_ERXS     (1u << 8)	/* RX requests to the status output */
#define UART_IMSK_ETXS     (1u << 9)	/* TX requests to the status output */
#define UART_IMSK_ALL      0x3FFu

struct wb_uart_config {
	uint32_t ctl;		/* UART_CTL without EN */
	uint32_t clk;
};

/*
 * Ring sizes are powers of two; the rings belong to the caller.
 *
 * With DMA receive, rx_head counts the bytes the channel has written, from
 * the change of its position between services. A reader more than the ring
 * behind has lost bytes: rx_lost counts each time, and the unread bytes are
 * dropped. A pause between services in which the line could have carried a
 * whole ring makes the position's change ambiguous: if bytes came, rx_gaps
 * counts it and the bytes are kept as if no lap had passed (S-06's CRC
 * rejects a frame torn by one).
 */
struct wb_uart {
	unsigned n;
	bool irq;
	bool dma;			/* receive through the DMA ring */
	uint8_t *rx;
	uint8_t *tx;
	uint32_t rx_size, tx_size;
	volatile uint32_t rx_head, rx_tail;	/* head: next write */
	volatile uint32_t tx_head, tx_tail;
	volatile uint32_t line_errors;	/* OE, PE, FE or BI seen */
	volatile uint32_t overruns;	/* of those, OE */
	const struct wb_time *time;
	/* DMA receive only. */
	uint32_t byte_cycles;		/* the shortest byte on the line */
	uint32_t rx_pos;		/* the ring offset at the last service */
	uint64_t rx_seen;		/* the cycle count then */
	volatile uint32_t rx_lost;
	volatile uint32_t rx_gaps;
	volatile uint32_t dma_errors;	/* DMA_STAT.IRQERR; channel restarted */
};

#define WB_UART_OK        0
#define WB_UART_ERR_ARG  (-1)
#define WB_UART_TIMEOUT  (-2)

/*
 * S-13b.072: the longest wait for a UART's transmitter to empty before it
 * is disabled. A full 2 KiB queue takes 20.5 ms at 1 Mbaud, the board's
 * rings and rate (S-13b.071).
 */
#define WB_UART_CLOSE_US 25000u

int wb_uart_check(unsigned n, const struct wb_uart_config *c);

/*
 * A UART already enabled, by an earlier stage for one, is closed first as
 * wb_uart_disable() closes it, with no ring: the bytes the controller
 * holds leave the pin, for at most WB_UART_CLOSE_US by t. Bytes still
 * queued in u's own earlier ring are dropped; flush or disable first.
 */
int wb_uart_init(struct wb_uart *u, unsigned n,
		 const struct wb_uart_config *c, const struct wb_time *t,
		 uint8_t *rx, uint32_t rx_size, uint8_t *tx, uint32_t tx_size);

/*
 * As wb_uart_init(), receiving through the DMA ring (S-13b.070): rx is
 * aligned to WB_DCACHE_LINE and rx_size at least that. byte_ns is the
 * shortest time one byte takes on the line (bits per byte at the fastest
 * rate the line may run); t serves the gap check and a channel restart.
 */
int wb_uart_init_dma(struct wb_uart *u, unsigned n,
		     const struct wb_uart_config *c, const struct wb_time *t,
		     uint32_t byte_ns, uint8_t *rx, uint32_t rx_size,
		     uint8_t *tx, uint32_t tx_size);

/*
 * Close UART n (S-13b.072, S-13b.082). While it is enabled, every byte
 * given to it leaves the pin first: u's queue, if u is this UART's ring
 * (NULL, or a ring never opened for n, is none), and the controller's,
 * until UART_STAT.TEMT reads 1, for at most WB_UART_CLOSE_US by t. Then,
 * with interrupts off since that read, the receive DMA channel stopped,
 * the interrupts masked and UART_CTL = 0. WB_UART_TIMEOUT when the wait
 * ran out (CTS held off, for one); the UART is closed all the same.
 */
int wb_uart_disable(unsigned n, struct wb_uart *u, const struct wb_time *t);

/*
 * S-13b.072's wait alone, for a path that stops the transmitter without
 * writing UART_CTL (a hard reset, the end of a power-off's shutdown): while
 * UART n is enabled, as wb_uart_disable() waits, and the UART is left as
 * it is. A UART that is not enabled is not waited for. WB_UART_TIMEOUT
 * when the wait ran out.
 */
int wb_uart_drain(unsigned n, struct wb_uart *u, const struct wb_time *t);

/* Drive the UART from its status interrupt (true) or by polling. */
void wb_uart_use_irq(struct wb_uart *u, bool on);

void wb_uart_service(struct wb_uart *u);
void wb_uart_isr(void *ctx, uint32_t sid);

size_t wb_uart_read(void *ctx, uint8_t *buf, size_t max);
/* Received bytes not yet read, after a service. */
size_t wb_uart_rx_ready(struct wb_uart *u);
size_t wb_uart_room(void *ctx);
void wb_uart_write(void *ctx, const uint8_t *data, size_t n);

/* Wait until the transmit ring and the controller are empty. */
int wb_uart_flush(struct wb_uart *u, const struct wb_time *t,
		  uint32_t timeout_us);

#endif /* WAFFLE_DRIVERS_BF70X_UART_H */

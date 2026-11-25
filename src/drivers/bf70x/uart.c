/* SPDX-License-Identifier: GPL-2.0-or-later */
/* UARTs (see waffle/drivers/bf70x/uart.h). */
#include "waffle/drivers/bf70x/uart.h"

#include "waffle/drivers/bf70x/core.h"
#include "waffle/drivers/bf70x/cplb.h"
#include "waffle/drivers/bf70x/dma.h"
#include "waffle/drivers/mmr.h"
#include "waffle/util/div64.h"

/* Bytes the controller may hold; one service call drains at most this. */
#define HW_FIFO 16u

/* UART n's receive channel (HRM Table 19-7, S-13b.080). */
#define RX_DMA(n) (WB_DMA_UART0_RX + 2u * (n))

/*
 * DMA receive: the unread bytes kept clear of the channel's write position.
 * Past that a byte could be overwritten while a read copies it, so the
 * reader counts as lost. 32 bytes at 1 Mbaud are 320 us, far longer than
 * any copy out of the ring.
 */
#define RX_MARGIN 32u

static uint32_t tx_used(const struct wb_uart *u)
{
	return u->tx_head - u->tx_tail;
}

/*
 * S-13b.072: wait until every byte given to UART n has left the pin:
 * nothing queued in u (NULL: no ring of the driver's) and TEMT read 1 after
 * the last byte went to THR, for at most WB_UART_CLOSE_US by t (one check
 * with t NULL). With CTS held off the transmitter never empties and the
 * wait ends at the bound. Returns with interrupts off and their mask in
 * *mask, so that no byte reaches THR between the last TEMT read and the
 * caller's disable.
 */
static int drain(unsigned n, struct wb_uart *u, const struct wb_time *t,
		 uint32_t *mask)
{
	const uint64_t end = t != NULL ?
			     wb_time_deadline(t, WB_UART_CLOSE_US) : 0u;

	for (;;) {
		const bool late = t == NULL || wb_time_passed(end);

		*mask = wb_core_cli();
		if (u != NULL)
			wb_uart_service(u);
		if ((u == NULL || tx_used(u) == 0) &&
		    (wb_mmr_read32(UART_STAT(n)) & UART_STAT_TEMT))
			return WB_UART_OK;
		if (late)
			return WB_UART_TIMEOUT;
		wb_core_sti(*mask);
	}
}

static bool pow2(uint32_t v)
{
	return v != 0 && (v & (v - 1u)) == 0;
}

int wb_uart_check(unsigned n, const struct wb_uart_config *c)
{
	if (n >= WB_UART_COUNT || (c->ctl & UART_CTL_EN) ||
	    (c->clk & 0xFFFFu) == 0)
		return WB_UART_ERR_ARG;
	return WB_UART_OK;
}

static int prepare(struct wb_uart *u, unsigned n,
		   const struct wb_uart_config *c, uint8_t *rx,
		   uint32_t rx_size, uint8_t *tx, uint32_t tx_size)
{
	if (wb_uart_check(n, c) != WB_UART_OK || !pow2(rx_size) ||
	    !pow2(tx_size))
		return WB_UART_ERR_ARG;
	*u = (struct wb_uart){ 0 };
	u->n = n;
	u->rx = rx;
	u->tx = tx;
	u->rx_size = rx_size;
	u->tx_size = tx_size;
	return WB_UART_OK;
}

/* Off and masked, then the divisor. A UART already enabled, by an earlier
 * stage for one, first lets the bytes it holds leave the pin (S-13b.072). */
static void quiesce(unsigned n, const struct wb_uart_config *c,
		    const struct wb_time *t)
{
	if (wb_mmr_read32(UART_CTL(n)) & UART_CTL_EN) {
		uint32_t mask;

		(void)drain(n, NULL, t, &mask);
		wb_mmr_write32(UART_CTL(n), 0);
		wb_core_sti(mask);
	} else {
		wb_mmr_write32(UART_CTL(n), 0);
	}
	wb_mmr_write32(UART_IMSK_CLR(n), UART_IMSK_ALL);
	wb_mmr_write32(UART_CLK(n), c->clk);
}

static void enable(unsigned n, const struct wb_uart_config *c)
{
	wb_mmr_write32(UART_CTL(n), c->ctl | UART_CTL_EN);
	wb_mmr_write32(UART_STAT(n), UART_STAT_ERRORS);
	wb_mmr_sync();
}

int wb_uart_init(struct wb_uart *u, unsigned n,
		 const struct wb_uart_config *c, const struct wb_time *t,
		 uint8_t *rx, uint32_t rx_size, uint8_t *tx, uint32_t tx_size)
{
	if (prepare(u, n, c, rx, rx_size, tx, tx_size) != WB_UART_OK ||
	    t == NULL)
		return WB_UART_ERR_ARG;
	u->time = t;
	quiesce(n, c, t);
	enable(n, c);
	return WB_UART_OK;
}

/*
 * The channel over the whole ring from its start, autobuffer:
 * DMAn_CFG = 0x00001007 (EN, WNR, SYNC, FLOW 1, bytes), XCNT the ring's
 * size, XMOD 1 (S-13b.070). wb_dma_start() invalidates the ring's lines.
 * Unread bytes are dropped.
 */
static int rx_dma_start(struct wb_uart *u)
{
	const struct wb_dma_xfer x = {
		.addr = (uint32_t)(uintptr_t)u->rx,
		.count = u->rx_size,
		.msize = WB_DMA_1B,
		.psize = WB_DMA_1B,
		.to_mem = true,
		.sync = true,
		.autobuffer = true,
	};

	u->rx_pos = 0;
	u->rx_tail = u->rx_head;
	u->rx_seen = wb_core_cycles();
	return wb_dma_start(RX_DMA(u->n), &x);
}

int wb_uart_init_dma(struct wb_uart *u, unsigned n,
		     const struct wb_uart_config *c, const struct wb_time *t,
		     uint32_t byte_ns, uint8_t *rx, uint32_t rx_size,
		     uint8_t *tx, uint32_t tx_size)
{
	uint64_t byte_cycles;

	if (prepare(u, n, c, rx, rx_size, tx, tx_size) != WB_UART_OK ||
	    t == NULL || rx_size < 2u * RX_MARGIN ||
	    ((uintptr_t)rx & (WB_DCACHE_LINE - 1u)) != 0)
		return WB_UART_ERR_ARG;
	byte_cycles = wb_udiv64((uint64_t)t->cycles_per_us * byte_ns, 1000u);
	if (byte_cycles == 0 || byte_cycles > UINT32_MAX)
		return WB_UART_ERR_ARG;
	u->dma = true;
	u->time = t;
	u->byte_cycles = (uint32_t)byte_cycles;

	quiesce(n, c, t);
	/* HRM "Using DMA Transfers": the request masked while the channel is
	 * set up, then unmasked; the channel runs before UART_CTL.EN is set
	 * (S-13b.070). ERBFI is the channel's request: the core never reads
	 * RBR in this mode. */
	wb_dma_stop_idle(RX_DMA(n), t);
	if (rx_dma_start(u) != WB_DMA_OK)
		return WB_UART_ERR_ARG;
	wb_mmr_write32(UART_IMSK_SET(n), UART_IMSK_ERBFI);
	enable(n, c);
	return WB_UART_OK;
}

int wb_uart_disable(unsigned n, struct wb_uart *u, const struct wb_time *t)
{
	uint32_t mask;
	int rc = WB_UART_OK;

	if (n >= WB_UART_COUNT)
		return WB_UART_ERR_ARG;
	if (u != NULL && (u->tx == NULL || u->n != n))
		u = NULL;		/* not this UART's ring */
	/* S-13b.072: the transmitter empty first. The receive channel goes
	 * on filling the ring through the wait (S-13b.070). */
	if (wb_mmr_read32(UART_CTL(n)) & UART_CTL_EN)
		rc = drain(n, u, t, &mask);
	else
		mask = wb_core_cli();
	/* The channel before the UART, so it is off at the hand-off and on
	 * every path that closes the UART (S-13b.072, S-13b.082). */
	wb_dma_stop(RX_DMA(n));
	wb_mmr_write32(UART_IMSK_CLR(n), UART_IMSK_ALL);
	wb_mmr_write32(UART_CTL(n), 0);
	wb_mmr_sync();
	wb_core_sti(mask);
	return rc;
}

int wb_uart_drain(unsigned n, struct wb_uart *u, const struct wb_time *t)
{
	uint32_t mask;
	int rc;

	if (n >= WB_UART_COUNT)
		return WB_UART_ERR_ARG;
	if (!(wb_mmr_read32(UART_CTL(n)) & UART_CTL_EN))
		return WB_UART_OK;
	if (u != NULL && (u->tx == NULL || u->n != n))
		u = NULL;		/* not this UART's ring */
	rc = drain(n, u, t, &mask);
	wb_core_sti(mask);
	return rc;
}

static uint32_t rx_used(const struct wb_uart *u)
{
	return u->rx_head - u->rx_tail;
}

/*
 * DMA receive: account for what the channel wrote since the last service.
 * The position is the ring's size less the current count, taken modulo the
 * size (the count reads 0 for an instant as it wraps), and the change is
 * taken modulo the size too, so a wrap between two services is counted
 * right whenever fewer than a ring's bytes came between them.
 */
static void rx_sample(struct wb_uart *u)
{
	const unsigned ch = RX_DMA(u->n);
	const uint32_t mask = u->rx_size - 1u;
	uint32_t pos, delta;
	uint64_t now;

	if (wb_mmr_read32(DMA_STAT(ch)) & DMA_STAT_IRQERR) {
		/* The channel stopped on an error: count it and start over. */
		u->dma_errors++;
		wb_mmr_write32(UART_IMSK_CLR(u->n), UART_IMSK_ERBFI);
		wb_dma_stop_idle(ch, u->time);
		(void)rx_dma_start(u);
		wb_mmr_write32(UART_IMSK_SET(u->n), UART_IMSK_ERBFI);
		wb_mmr_sync();
		return;
	}
	pos = (u->rx_size - wb_mmr_read32(DMA_XCNT_CUR(ch))) & mask;
	now = wb_core_cycles();
	delta = (pos - u->rx_pos) & mask;
	/* The line could have carried a whole ring: a lap cannot be ruled
	 * out (struct wb_uart). */
	if (delta != 0 &&
	    now - u->rx_seen >= (uint64_t)(u->rx_size - 1u) * u->byte_cycles)
		u->rx_gaps++;
	u->rx_pos = pos;
	u->rx_seen = now;
	u->rx_head += delta;
	if (u->rx_head - u->rx_tail > u->rx_size - RX_MARGIN) {
		u->rx_lost++;
		u->rx_tail = u->rx_head;
	}
}

/* Interrupt sources that have work to do now. */
static void update_irq(const struct wb_uart *u)
{
	uint32_t on = 0, off = 0;

	if (!u->irq)
		return;
	if (tx_used(u) != 0)
		on |= UART_IMSK_ETBEI;
	else
		off |= UART_IMSK_ETBEI;
	if (u->dma) {
		/* ERBFI stays the channel's request; the transmit request goes
		 * to the status interrupt. ELSI would take the receive request
		 * from the channel (HRM UART_IMSK.ELSI). */
		on |= UART_IMSK_ETXS;
	} else {
		if (rx_used(u) < u->rx_size)
			on |= UART_IMSK_ERBFI;
		else
			off |= UART_IMSK_ERBFI;
		on |= UART_IMSK_ELSI;
	}
	if (off != 0)
		wb_mmr_write32(UART_IMSK_CLR(u->n), off);
	wb_mmr_write32(UART_IMSK_SET(u->n), on);
}

void wb_uart_service(struct wb_uart *u)
{
	unsigned i;
	uint32_t stat = wb_mmr_read32(UART_STAT(u->n));

	if (stat & UART_STAT_ERRORS) {
		u->line_errors++;
		if (stat & UART_STAT_OE)
			u->overruns++;
		wb_mmr_write32(UART_STAT(u->n), stat & UART_STAT_ERRORS);
	}
	if (u->dma)
		rx_sample(u);
	for (i = 0; !u->dma && i < HW_FIFO && rx_used(u) < u->rx_size; i++) {
		if (!(stat & UART_STAT_DR))
			break;
		u->rx[u->rx_head & (u->rx_size - 1u)] =
			(uint8_t)wb_mmr_read32(UART_RBR(u->n));
		u->rx_head++;
		stat = wb_mmr_read32(UART_STAT(u->n));
	}
	for (i = 0; i < HW_FIFO && tx_used(u) != 0; i++) {
		if (!(stat & UART_STAT_THRE))
			break;
		wb_mmr_write32(UART_THR(u->n),
			       u->tx[u->tx_tail & (u->tx_size - 1u)]);
		u->tx_tail++;
		stat = wb_mmr_read32(UART_STAT(u->n));
	}
	update_irq(u);
}

void wb_uart_isr(void *ctx, uint32_t sid)
{
	(void)sid;
	wb_uart_service(ctx);
}

void wb_uart_use_irq(struct wb_uart *u, bool on)
{
	uint32_t mask = wb_core_cli();

	u->irq = on;
	if (on)
		update_irq(u);
	else if (u->dma)
		wb_mmr_write32(UART_IMSK_CLR(u->n),
			       UART_IMSK_ETBEI | UART_IMSK_ETXS);
	else
		wb_mmr_write32(UART_IMSK_CLR(u->n), UART_IMSK_ALL);
	wb_mmr_sync();
	wb_core_sti(mask);
}

/* Thread-side service: interrupts off while the rings and FIFO move. */
static void service_locked(struct wb_uart *u)
{
	uint32_t mask = wb_core_cli();

	wb_uart_service(u);
	wb_core_sti(mask);
}

/*
 * DMA receive, with interrupts off so a service from the interrupt cannot
 * move the tail under the copy. The ring may be in memory the data cache
 * holds (the board's is in DDR, cached write-through; in L1 SRAM, never
 * cached, FLUSHINV does nothing): each line is invalidated after the
 * channel's position showed its bytes written and just before they are
 * read, so a line cached by an earlier read is not read stale. The core
 * never writes the ring, so an invalidate drops nothing of its own.
 */
static size_t dma_read(struct wb_uart *u, uint8_t *buf, size_t max)
{
	uint32_t mask = wb_core_cli();
	size_t n = 0;

	wb_uart_service(u);
	while (n < max && rx_used(u) != 0) {
		uint32_t at = u->rx_tail & (u->rx_size - 1u);
		uint32_t len = u->rx_size - at, i;

		if (len > rx_used(u))
			len = rx_used(u);
		if (len > max - n)
			len = (uint32_t)(max - n);
		wb_dcache_invalidate_range((uint32_t)(uintptr_t)(u->rx + at),
					   len);
		for (i = 0; i < len; i++)
			buf[n++] = u->rx[at + i];
		u->rx_tail += len;
	}
	wb_core_sti(mask);
	return n;
}

size_t wb_uart_read(void *ctx, uint8_t *buf, size_t max)
{
	struct wb_uart *u = ctx;
	size_t n = 0;

	if (u->dma)
		return dma_read(u, buf, max);
	service_locked(u);
	while (n < max && rx_used(u) != 0) {
		buf[n++] = u->rx[u->rx_tail & (u->rx_size - 1u)];
		u->rx_tail++;
	}
	/* Room again: take what waited in the controller. */
	if (n != 0)
		service_locked(u);
	return n;
}

size_t wb_uart_rx_ready(struct wb_uart *u)
{
	service_locked(u);
	return rx_used(u);
}

size_t wb_uart_room(void *ctx)
{
	struct wb_uart *u = ctx;

	service_locked(u);
	return u->tx_size - tx_used(u);
}

void wb_uart_write(void *ctx, const uint8_t *data, size_t n)
{
	struct wb_uart *u = ctx;
	size_t i;

	for (i = 0; i < n && tx_used(u) < u->tx_size; i++) {
		u->tx[u->tx_head & (u->tx_size - 1u)] = data[i];
		u->tx_head++;
	}
	service_locked(u);
}

int wb_uart_flush(struct wb_uart *u, const struct wb_time *t,
		  uint32_t timeout_us)
{
	uint64_t end = wb_time_deadline(t, timeout_us);
	bool late;

	do {
		late = wb_time_passed(end);
		service_locked(u);
		if (tx_used(u) == 0 &&
		    (wb_mmr_read32(UART_STAT(u->n)) & UART_STAT_TEMT))
			return WB_UART_OK;
	} while (!late);
	return WB_UART_TIMEOUT;
}

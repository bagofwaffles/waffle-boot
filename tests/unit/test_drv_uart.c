/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tests for the UART driver (src/drivers/bf70x/uart.c). */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x/dma.h"
#include "waffle/drivers/bf70x/uart.h"
#include "waffle/link/link.h"

static const struct wb_time cclk400 = { 400u };

/* S-13b.070, .071 without EN. */
static const struct wb_uart_config uart0_cfg = { 0x00000300u, 0x80000064u };
static const struct wb_uart_config uart1_cfg = { 0x18000300u, 0x80000064u };

/*
 * The line: bytes queued in in[] arrive in the receive FIFO (DR while any
 * is left); THR writes go to out[] unless CTS holds the transmitter. With
 * byte_reads set, a byte written to THR stays in the transmitter for that
 * many reads of STAT (THRE and TEMT clear meanwhile), and out[] gets it
 * whole when it has left.
 */
static struct line {
	unsigned n;
	uint8_t in[64];
	size_t in_head, in_len;
	uint8_t out[256];
	size_t out_len;
	bool cts_held;
	uint32_t errors;
	unsigned byte_reads;
	unsigned busy;		/* reads left for the byte in the transmitter */
	uint8_t shifting;
} line;

static bool line_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		      wb_mmr_addr addr, uint32_t *value)
{
	struct line *l = ctx;

	(void)width;
	if (op == FAKE_MMR_READ && addr == UART_STAT(l->n)) {
		*value = l->errors;
		if (l->in_head < l->in_len)
			*value |= UART_STAT_DR;
		if (l->busy != 0 && !l->cts_held && --l->busy == 0 &&
		    l->out_len < sizeof(l->out))
			l->out[l->out_len++] = l->shifting;
		if (!l->cts_held && l->busy == 0)
			*value |= UART_STAT_THRE | UART_STAT_TEMT;
	} else if (op == FAKE_MMR_READ && addr == UART_RBR(l->n)) {
		*value = l->in_head < l->in_len ? l->in[l->in_head++] : 0;
	} else if (op == FAKE_MMR_WRITE && addr == UART_THR(l->n)) {
		if (l->cts_held || l->busy != 0 ||
		    l->out_len == sizeof(l->out))
			l->errors |= 1u << 31;	/* written while full */
		else if (l->byte_reads != 0) {
			l->busy = l->byte_reads;
			l->shifting = (uint8_t)*value;
		} else
			l->out[l->out_len++] = (uint8_t)*value;
	} else if (op == FAKE_MMR_WRITE && addr == UART_STAT(l->n)) {
		l->errors &= ~*value;
	}
	return true;
}

static uint8_t rxbuf[8], txbuf[8];

static void reset(unsigned n)
{
	fake_mmr_reset();
	fake_core_reset();
	memset(&line, 0, sizeof(line));
	line.n = n;
	fake_mmr_set_hook(line_hook, &line);
}

static void feed(const char *s)
{
	size_t len = strlen(s);

	memcpy(line.in + line.in_len, s, len);
	line.in_len += len;
}

static void test_init(void)
{
	static const struct fake_mmr_access seq[] = {
		FAKE_R32(UART_CTL(1), 0),
		FAKE_W32(UART_CTL(1), 0),
		FAKE_W32(UART_IMSK_CLR(1), UART_IMSK_ALL),
		FAKE_W32(UART_CLK(1), 0x80000064u),
		FAKE_W32(UART_CTL(1), 0x18000301u),
		FAKE_W32(UART_STAT(1), UART_STAT_ERRORS),
		FAKE_SYNC,
	};
	/* The receive channel first (S-13b.072, S-13b.082). */
	static const struct fake_mmr_access off[] = {
		FAKE_R32(UART_CTL(0), 0),
		FAKE_W32(DMA_CFG(11), 0),
		FAKE_W32(DMA_STAT(11), 7u),
		FAKE_SYNC,
		FAKE_W32(UART_IMSK_CLR(0), UART_IMSK_ALL),
		FAKE_W32(UART_CTL(0), 0),
		FAKE_SYNC,
	};
	struct wb_uart u;

	reset(1);
	CHECK(wb_uart_init(&u, 1, &uart1_cfg, &cclk400, rxbuf,
			   sizeof(rxbuf), txbuf, sizeof(txbuf)) == WB_UART_OK);
	CHECK(fake_mmr_expect("init", seq, ARRAY_LEN(seq)));
	CHECK(UART_CTL(0) == 0x2005F004u && UART_THR(1) == 0x2005F424u);

	CHECK(wb_uart_init(&u, 2, &uart0_cfg, &cclk400, rxbuf, 8, txbuf, 8) ==
	      WB_UART_ERR_ARG);
	CHECK(wb_uart_init(&u, 0, &uart0_cfg, &cclk400, rxbuf, 6, txbuf, 8) ==
	      WB_UART_ERR_ARG);
	CHECK(wb_uart_init(&u, 0, &(struct wb_uart_config){ 0x301u, 100u },
			   &cclk400, rxbuf, 8, txbuf, 8) == WB_UART_ERR_ARG);
	CHECK(wb_uart_init(&u, 0, &(struct wb_uart_config){ 0x300u, 0 },
			   &cclk400, rxbuf, 8, txbuf, 8) == WB_UART_ERR_ARG);
	CHECK(wb_uart_init(&u, 0, &uart0_cfg, NULL, rxbuf, 8, txbuf, 8) ==
	      WB_UART_ERR_ARG);

	/* S-13b.072 */
	fake_mmr_clear_log();
	CHECK(wb_uart_disable(0, NULL, &cclk400) == WB_UART_OK);
	CHECK(fake_mmr_expect("disable", off, ARRAY_LEN(off)));
	CHECK(wb_uart_disable(2, NULL, &cclk400) == WB_UART_ERR_ARG);
}

/* The s06 port adapter is an initializer; polled operation. */
static void test_polled(void)
{
	const struct s06_port_ops ops = {
		wb_uart_read, wb_uart_room, wb_uart_write,
	};
	struct wb_uart u;
	uint8_t buf[16];

	reset(0);
	CHECK(wb_uart_init(&u, 0, &uart0_cfg, &cclk400, rxbuf,
			   sizeof(rxbuf), txbuf, sizeof(txbuf)) == WB_UART_OK);
	CHECK(ops.read(&u, buf, sizeof(buf)) == 0);
	feed("hello");
	CHECK(ops.read(&u, buf, sizeof(buf)) == 5);
	CHECK(memcmp(buf, "hello", 5) == 0);

	CHECK(ops.room(&u) == 8);
	ops.write(&u, (const uint8_t *)"abc", 3);
	CHECK(line.out_len == 3 && memcmp(line.out, "abc", 3) == 0);
	CHECK(wb_uart_flush(&u, &cclk400, 10) == WB_UART_OK);
	CHECK(line.errors == 0);
}

/* A full receive ring leaves bytes in the controller (RTS held off). */
static void test_rx_backpressure(void)
{
	struct wb_uart u;
	uint8_t buf[32];

	reset(1);
	CHECK(wb_uart_init(&u, 1, &uart1_cfg, &cclk400, rxbuf,
			   sizeof(rxbuf), txbuf, sizeof(txbuf)) == WB_UART_OK);
	feed("0123456789AB");
	wb_uart_service(&u);
	CHECK(line.in_head == 8);	/* the ring took 8, 4 still wait */
	CHECK(wb_uart_read(&u, buf, 3) == 3);
	CHECK(memcmp(buf, "012", 3) == 0);
	/* Reading made room and pulled the waiting bytes in. */
	CHECK(line.in_head == 11);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 8);
	CHECK(memcmp(buf, "3456789A", 8) == 0);
	CHECK(line.in_head == 12);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 1);
	CHECK(buf[0] == 'B');
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 0);
}

/* CTS held: nothing is written to THR until it is released. */
static void test_tx_held(void)
{
	struct wb_uart u;

	reset(1);
	CHECK(wb_uart_init(&u, 1, &uart1_cfg, &cclk400, rxbuf,
			   sizeof(rxbuf), txbuf, sizeof(txbuf)) == WB_UART_OK);
	line.cts_held = true;
	wb_uart_write(&u, (const uint8_t *)"0123456789", 10);
	CHECK(wb_uart_room(&u) == 0);	/* 8 queued, 2 refused */
	CHECK(line.out_len == 0);
	fake_core_set_cycle_step(400);
	CHECK(wb_uart_flush(&u, &cclk400, 20) == WB_UART_TIMEOUT);
	line.cts_held = false;
	CHECK(wb_uart_flush(&u, &cclk400, 20) == WB_UART_OK);
	CHECK(line.out_len == 8 && memcmp(line.out, "01234567", 8) == 0);
	CHECK(line.errors == 0);
}

static void test_irq(void)
{
	struct wb_uart u;
	uint8_t buf[8];

	reset(0);
	CHECK(wb_uart_init(&u, 0, &uart0_cfg, &cclk400, rxbuf,
			   sizeof(rxbuf), txbuf, sizeof(txbuf)) == WB_UART_OK);
	fake_core_set_imask(0xFFFFu);
	wb_uart_use_irq(&u, true);
	CHECK(fake_core_imask() == 0xFFFFu);	/* restored */
	CHECK(fake_mmr_get(UART_IMSK_SET(0)) ==
	      (UART_IMSK_ERBFI | UART_IMSK_ELSI));

	line.cts_held = true;
	wb_uart_write(&u, (const uint8_t *)"xy", 2);
	CHECK(fake_mmr_get(UART_IMSK_SET(0)) ==
	      (UART_IMSK_ERBFI | UART_IMSK_ETBEI | UART_IMSK_ELSI));
	line.cts_held = false;
	wb_uart_isr(&u, 48);
	CHECK(line.out_len == 2);
	CHECK(fake_mmr_get(UART_IMSK_CLR(0)) == UART_IMSK_ETBEI);

	/* A full ring masks the receive interrupt. */
	feed("ABCDEFGHIJ");
	wb_uart_isr(&u, 48);
	CHECK(fake_mmr_get(UART_IMSK_CLR(0)) ==
	      (UART_IMSK_ERBFI | UART_IMSK_ETBEI));
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 8);
	CHECK(fake_mmr_get(UART_IMSK_SET(0)) & UART_IMSK_ERBFI);

	wb_uart_use_irq(&u, false);
	CHECK(fake_mmr_get(UART_IMSK_CLR(0)) == UART_IMSK_ALL);
}

static void test_line_errors(void)
{
	struct wb_uart u;

	reset(0);
	CHECK(wb_uart_init(&u, 0, &uart0_cfg, &cclk400, rxbuf,
			   sizeof(rxbuf), txbuf, sizeof(txbuf)) == WB_UART_OK);
	line.errors = UART_STAT_FE | UART_STAT_OE;
	wb_uart_service(&u);
	CHECK(u.line_errors == 1 && u.overruns == 1);
	CHECK(line.errors == 0);
	wb_uart_service(&u);
	CHECK(u.line_errors == 1);
	line.errors = UART_STAT_PE;
	wb_uart_service(&u);
	CHECK(u.line_errors == 2 && u.overruns == 1);
}

/* --- Closing (S-13b.072) --- */

/*
 * check_uart_close (tests/oracle/S-13b-buses-and-pins/check.py) on the register
 * log: UART n, enabled (a CTL write with EN, or on before the log when
 * on), is disabled only after a STAT read with TEMT set that no THR write
 * follows, or after its wait ran out (expired: the test saw the bound).
 */
static bool close_ok(unsigned n, bool on, bool expired)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	bool empty = false;
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++) {
		const struct fake_mmr_access *a = &log[i];

		if (a->op == FAKE_MMR_READ && a->addr == UART_STAT(n)) {
			empty = (a->value & UART_STAT_TEMT) != 0;
		} else if (a->op == FAKE_MMR_WRITE && a->addr == UART_THR(n)) {
			empty = false;
		} else if (a->op == FAKE_MMR_WRITE && a->addr == UART_CTL(n)) {
			if (a->value & UART_CTL_EN) {
				if (!on)
					empty = false;
				on = true;
			} else {
				if (on && !empty && !expired)
					return false;
				on = false;
			}
		}
	}
	return true;
}

/* The log index of the last access to addr of op, or -1. */
static int last_at(enum fake_mmr_op op, wb_mmr_addr addr)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	int at = -1;
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == op && log[i].addr == addr)
			at = (int)i;
	return at;
}

/*
 * Drained: bytes in the ring and in the transmitter all leave the pin,
 * whole, before UART_CTL = 0, and the TEMT read comes after the last THR
 * write. Nothing sent, never enabled and disabled twice pass too.
 */
static void test_close_drained(void)
{
	static const uint8_t msg[8] = { 0x7E, 1, 2, 3, 4, 5, 6, 0x7E };
	struct wb_uart u;
	uint32_t imask;

	reset(0);
	line.byte_reads = 3;
	CHECK(wb_uart_init(&u, 0, &uart0_cfg, &cclk400, rxbuf,
			   sizeof(rxbuf), txbuf, sizeof(txbuf)) == WB_UART_OK);
	wb_uart_write(&u, msg, sizeof(msg));
	CHECK(line.out_len == 0 && u.tx_head - u.tx_tail == 7u);
	imask = fake_core_imask();
	CHECK(wb_uart_disable(0, &u, &cclk400) == WB_UART_OK);
	CHECK(line.out_len == sizeof(msg) && memcmp(line.out, msg, sizeof(msg)) == 0);
	CHECK(fake_mmr_get(UART_CTL(0)) == 0);
	CHECK(last_at(FAKE_MMR_READ, UART_STAT(0)) >
	      last_at(FAKE_MMR_WRITE, UART_THR(0)));
	CHECK(close_ok(0, false, false));
	CHECK(line.errors == 0);
	CHECK(fake_core_imask() == imask);	/* interrupts as they were */

	/* Nothing sent; then a second disable of the closed UART. */
	reset(0);
	CHECK(wb_uart_init(&u, 0, &uart0_cfg, &cclk400, rxbuf,
			   sizeof(rxbuf), txbuf, sizeof(txbuf)) == WB_UART_OK);
	CHECK(wb_uart_disable(0, &u, &cclk400) == WB_UART_OK);
	CHECK(wb_uart_disable(0, &u, &cclk400) == WB_UART_OK);
	CHECK(close_ok(0, false, false));

	/* Never enabled: closed at once, no status read. */
	reset(1);
	CHECK(wb_uart_disable(1, NULL, &cclk400) == WB_UART_OK);
	CHECK(last_at(FAKE_MMR_READ, UART_STAT(1)) < 0);
	CHECK(fake_mmr_get(UART_CTL(1)) == 0);

	/* A ring opened for the other UART is not this one's queue. */
	reset(0);
	CHECK(wb_uart_init(&u, 0, &uart0_cfg, &cclk400, rxbuf,
			   sizeof(rxbuf), txbuf, sizeof(txbuf)) == WB_UART_OK);
	fake_mmr_set(UART_CTL(1), 0x18000301u);
	fake_mmr_set(UART_STAT(1), UART_STAT_TEMT);
	CHECK(wb_uart_disable(1, &u, &cclk400) == WB_UART_OK);
	CHECK(last_at(FAKE_MMR_WRITE, UART_THR(0)) < 0);
	CHECK(fake_mmr_get(UART_CTL(1)) == 0 && fake_mmr_get(UART_CTL(0)) != 0);
}

/*
 * UART1 with CTS held off by the Nordic: the transmitter never empties, the
 * wait ends at WB_UART_CLOSE_US and the UART is closed all the same.
 */
static void test_close_cts_held(void)
{
	static const uint8_t msg[4] = { 0x7E, 9, 8, 0x7E };
	struct wb_uart u;
	uint64_t t0, took;

	reset(1);
	line.byte_reads = 3;
	CHECK(wb_uart_init(&u, 1, &uart1_cfg, &cclk400, rxbuf,
			   sizeof(rxbuf), txbuf, sizeof(txbuf)) == WB_UART_OK);
	wb_uart_write(&u, msg, sizeof(msg));
	line.cts_held = true;
	fake_core_set_cycle_step(400u * 250u);		/* 250 us a read */
	t0 = fake_core_peek_cycles();
	CHECK(wb_uart_disable(1, &u, &cclk400) == WB_UART_TIMEOUT);
	took = fake_core_peek_cycles() - t0;
	CHECK(took >= 400u * (uint64_t)WB_UART_CLOSE_US);
	CHECK(took <= 400u * ((uint64_t)WB_UART_CLOSE_US + 2000u));
	CHECK(fake_mmr_get(UART_CTL(1)) == 0);
	CHECK(!close_ok(1, false, false));	/* cut: only the bound allows it */
	CHECK(close_ok(1, false, true));
	CHECK(fake_mmr_errors() == 0);
	fake_core_set_cycle_step(1u);
}

/*
 * A UART an earlier stage left enabled with a byte in flight: the new
 * start lets it leave the pin before UART_CTL = 0, for either init.
 */
static void test_close_reinit(void)
{
	struct wb_uart u;

	reset(0);
	line.byte_reads = 5;
	fake_mmr_set(UART_CTL(0), 0x00000301u);
	wb_mmr_write32(UART_THR(0), 0x42u);	/* the earlier stage's last byte */
	fake_mmr_clear_log();
	CHECK(wb_uart_init(&u, 0, &uart0_cfg, &cclk400, rxbuf,
			   sizeof(rxbuf), txbuf, sizeof(txbuf)) == WB_UART_OK);
	CHECK(line.out_len == 1 && line.out[0] == 0x42u);
	CHECK(close_ok(0, true, false));
	CHECK(fake_mmr_get(UART_CTL(0)) == 0x00000301u);
}

/*
 * The wait alone (a hard reset, a power-off's end): the queue and the
 * transmitter empty, the TEMT read after the last THR write, and the UART
 * left enabled; with CTS held, the bound; a UART not enabled is not waited
 * for.
 */
static void test_drain(void)
{
	static const uint8_t msg[8] = { 0x7E, 1, 2, 3, 4, 5, 6, 0x7E };
	struct wb_uart u;
	uint32_t imask;
	uint64_t t0, took;

	reset(0);
	line.byte_reads = 3;
	CHECK(wb_uart_init(&u, 0, &uart0_cfg, &cclk400, rxbuf,
			   sizeof(rxbuf), txbuf, sizeof(txbuf)) == WB_UART_OK);
	wb_uart_write(&u, msg, sizeof(msg));
	CHECK(line.out_len == 0 && u.tx_head - u.tx_tail == 7u);
	imask = fake_core_imask();
	CHECK(wb_uart_drain(0, &u, &cclk400) == WB_UART_OK);
	CHECK(line.out_len == sizeof(msg) && memcmp(line.out, msg, sizeof(msg)) == 0);
	CHECK(u.tx_head == u.tx_tail);
	CHECK(last_at(FAKE_MMR_READ, UART_STAT(0)) >
	      last_at(FAKE_MMR_WRITE, UART_THR(0)));
	CHECK(fake_mmr_get(UART_CTL(0)) == 0x00000301u);
	CHECK(line.errors == 0);
	CHECK(fake_core_imask() == imask);

	/* CTS held: the bound, then the UART as it was. */
	reset(1);
	line.byte_reads = 3;
	CHECK(wb_uart_init(&u, 1, &uart1_cfg, &cclk400, rxbuf,
			   sizeof(rxbuf), txbuf, sizeof(txbuf)) == WB_UART_OK);
	wb_uart_write(&u, msg, 4);
	line.cts_held = true;
	fake_core_set_cycle_step(400u * 250u);		/* 250 us a read */
	t0 = fake_core_peek_cycles();
	CHECK(wb_uart_drain(1, &u, &cclk400) == WB_UART_TIMEOUT);
	took = fake_core_peek_cycles() - t0;
	CHECK(took >= 400u * (uint64_t)WB_UART_CLOSE_US);
	CHECK(took <= 400u * ((uint64_t)WB_UART_CLOSE_US + 2000u));
	CHECK(fake_mmr_get(UART_CTL(1)) == 0x18000301u);
	CHECK(fake_mmr_errors() == 0);
	fake_core_set_cycle_step(1u);

	/* Not enabled: no status read. */
	reset(1);
	CHECK(wb_uart_drain(1, NULL, &cclk400) == WB_UART_OK);
	CHECK(last_at(FAKE_MMR_READ, UART_STAT(1)) < 0);
	CHECK(wb_uart_drain(2, NULL, &cclk400) == WB_UART_ERR_ARG);
}

/* --- DMA receive (S-13b.070) --- */

/*
 * UART0 with DMA11 over a ring: a byte that arrives goes to the ring while
 * the channel is enabled, ERBFI (its request) unmasked and the UART on;
 * DMA11_XCNT_CUR counts down and reads 0 after the ring's last byte until
 * the next one reloads it. RBR is never to be read in this mode.
 */
#define RING 128u
static uint8_t ring[RING] __attribute__((aligned(32)));

static struct dmaline {
	uint32_t imsk;
	uint32_t errors;
	unsigned rbr_reads;
	unsigned dropped;		/* arrived with no request taken */
	uint8_t out[64];
	size_t out_len;
	bool imsk_elsi_seen;
	unsigned busy;			/* STAT reads before TEMT */
	unsigned arrived;		/* bytes received meanwhile */
} dl;

static void arrive_seq(unsigned from, size_t n);

static bool dma_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		     wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_READ && addr == UART_STAT(0)) {
		*value = dl.errors | UART_STAT_THRE;
		if (dl.busy != 0) {
			/* the PSoC goes on sending while the last byte leaves */
			dl.busy--;
			arrive_seq(0x40u + dl.arrived++, 1);
		} else {
			*value |= UART_STAT_TEMT;
		}
	} else if (op == FAKE_MMR_READ && addr == UART_RBR(0)) {
		dl.rbr_reads++;
	} else if (op == FAKE_MMR_WRITE && addr == UART_STAT(0)) {
		dl.errors &= ~*value;
	} else if (op == FAKE_MMR_WRITE && addr == UART_IMSK_SET(0)) {
		dl.imsk |= *value;
		if (dl.imsk & UART_IMSK_ELSI)
			dl.imsk_elsi_seen = true;
	} else if (op == FAKE_MMR_WRITE && addr == UART_IMSK_CLR(0)) {
		dl.imsk &= ~*value;
	} else if (op == FAKE_MMR_WRITE && addr == UART_THR(0)) {
		if (dl.out_len < sizeof(dl.out))
			dl.out[dl.out_len++] = (uint8_t)*value;
	} else if (op == FAKE_MMR_WRITE && addr == DMA_STAT(11)) {
		*value = fake_mmr_get(addr) & ~(*value & 7u);
	} else if (op == FAKE_MMR_WRITE && addr == DMA_CFG(11) &&
		   (*value & DMA_CFG_EN)) {
		fake_mmr_set(DMA_XCNT_CUR(11), fake_mmr_get(DMA_XCNT(11)));
	}
	return true;
}

static void arrive(const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		uint32_t cur = fake_mmr_get(DMA_XCNT_CUR(11));

		if (!(fake_mmr_get(DMA_CFG(11)) & DMA_CFG_EN) ||
		    !(dl.imsk & UART_IMSK_ERBFI) || (dl.imsk & UART_IMSK_ELSI) ||
		    !(fake_mmr_get(UART_CTL(0)) & UART_CTL_EN)) {
			dl.dropped++;
			continue;
		}
		if (cur == 0)
			cur = RING;
		ring[RING - cur] = b[i];
		fake_mmr_set(DMA_XCNT_CUR(11), cur - 1u);
	}
}

static void arrive_seq(unsigned from, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		uint8_t b = (uint8_t)(from + i);

		arrive(&b, 1);
	}
}

static bool is_seq(const uint8_t *b, unsigned from, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (b[i] != (uint8_t)(from + i))
			return false;
	return true;
}

static void dma_reset(void)
{
	fake_mmr_reset();
	fake_core_reset();
	memset(&dl, 0, sizeof(dl));
	memset(ring, 0xEE, sizeof(ring));
	fake_mmr_set_hook(dma_hook, NULL);
}

static void dma_open(struct wb_uart *u)
{
	dma_reset();
	CHECK(wb_uart_init_dma(u, 0, &uart0_cfg, &cclk400, 9900u, ring, RING,
			       txbuf, sizeof(txbuf)) == WB_UART_OK);
}

/* The index of the first write of value to addr in the log, or -1. */
static int write_at(wb_mmr_addr addr, uint32_t value)
{
	const struct fake_mmr_access *log = fake_mmr_log();

	for (size_t i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == FAKE_MMR_WRITE && log[i].addr == addr &&
		    log[i].value == value)
			return (int)i;
	return -1;
}

/* S-13b.070's set-up: the channel runs before UART_CTL.EN; its request
 * (ERBFI) is unmasked after the channel is set up. */
static void test_dma_start(void)
{
	struct wb_uart u;
	int cfg, en, req;

	dma_open(&u);
	CHECK(u.dma);
	CHECK(fake_mmr_get(DMA_CFG(11)) == 0x00001007u);
	CHECK(fake_mmr_get(DMA_XCNT(11)) == RING);
	CHECK(fake_mmr_get(DMA_XMOD(11)) == 1u);
	CHECK(fake_mmr_get(DMA_ADDRSTART(11)) == (uint32_t)(uintptr_t)ring);
	CHECK(fake_mmr_get(UART_CTL(0)) == 0x00000301u);
	CHECK(fake_mmr_get(UART_CLK(0)) == 0x80000064u);
	cfg = write_at(DMA_CFG(11), 0x00001007u);
	en = write_at(UART_CTL(0), 0x00000301u);
	req = write_at(UART_IMSK_SET(0), UART_IMSK_ERBFI);
	CHECK(cfg >= 0 && req > cfg && en > req);
	/* The channel was stopped before it was set up; the UART off first. */
	CHECK(write_at(DMA_CFG(11), 0) >= 0 && write_at(DMA_CFG(11), 0) < cfg);
	CHECK(write_at(UART_CTL(0), 0) >= 0 && write_at(UART_CTL(0), 0) < cfg);
	CHECK(dl.imsk == UART_IMSK_ERBFI);
	/* The ring's lines invalidated before the channel writes them. */
	CHECK(fake_core_flushinvs() == RING / 32u);
	CHECK(fake_mmr_errors() == 0);

	/* A ring the channel cannot serve, or the caches would share. */
	CHECK(wb_uart_init_dma(&u, 0, &uart0_cfg, &cclk400, 9900u, ring + 4,
			       64, txbuf, 8) == WB_UART_ERR_ARG);
	CHECK(wb_uart_init_dma(&u, 0, &uart0_cfg, &cclk400, 9900u, ring, 32,
			       txbuf, 8) == WB_UART_ERR_ARG);
	CHECK(wb_uart_init_dma(&u, 0, &uart0_cfg, &cclk400, 9900u, ring, 96,
			       txbuf, 8) == WB_UART_ERR_ARG);
	CHECK(wb_uart_init_dma(&u, 0, &uart0_cfg, &cclk400, 0, ring, RING,
			       txbuf, 8) == WB_UART_ERR_ARG);
	CHECK(wb_uart_init_dma(&u, 0, &uart0_cfg, NULL, 9900u, ring, RING,
			       txbuf, 8) == WB_UART_ERR_ARG);
}

/* Bytes the channel wrote are read in order across the ring's end, the
 * write position from the current count, RBR never read. */
static void test_dma_ring_wrap(void)
{
	struct wb_uart u;
	uint8_t buf[RING];
	unsigned at = 0;

	dma_open(&u);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 0);
	for (int round = 0; round < 5; round++) {
		size_t got = 0;

		arrive_seq(at, 90);
		while (got < 90) {
			size_t n = wb_uart_read(&u, buf + got, 7);

			CHECK(n != 0);
			if (n == 0)
				break;
			got += n;
		}
		CHECK(is_seq(buf, at, 90));
		at += 90;
	}
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 0);
	CHECK(u.rx_head == 450u && u.rx_tail == 450u);
	/* 62 more end the ring's fourth pass: the count reads 0 there. */
	arrive_seq(at, 62);
	CHECK(fake_mmr_get(DMA_XCNT_CUR(11)) == 0);
	CHECK(wb_uart_rx_ready(&u) == 62u);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 62u && is_seq(buf, at, 62));
	at += 62;
	arrive_seq(at, 3);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 3u && is_seq(buf, at, 3));
	CHECK(dl.rbr_reads == 0 && dl.dropped == 0);
	CHECK(u.rx_lost == 0 && u.rx_gaps == 0 && u.overruns == 0);
	CHECK(fake_mmr_errors() == 0);
}

/* The position is the ring's size less the current count, so a count
 * that wrapped between two services still gives the bytes that came. */
static void test_dma_position(void)
{
	struct wb_uart u;
	uint8_t buf[RING];
	unsigned before;

	dma_open(&u);
	fake_mmr_set(DMA_XCNT_CUR(11), RING - 90u);
	memcpy(ring, "abcdefghij", 10);
	CHECK(wb_uart_read(&u, buf, 10) == 10 && memcmp(buf, "abcdefghij", 10) == 0);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 80);
	/* 60 more: 38 to the end, 22 from the start. */
	fake_mmr_set(DMA_XCNT_CUR(11), RING - 22u);
	ring[90] = 'x';
	ring[21] = 'y';
	CHECK(wb_uart_rx_ready(&u) == 60u);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 60u);
	CHECK(buf[0] == 'x' && buf[59] == 'y');
	/* Each read invalidates the lines it copies, last the one it ends in. */
	before = fake_core_flushinvs();
	fake_mmr_set(DMA_XCNT_CUR(11), RING - 30u);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 8u);
	CHECK(fake_core_flushinvs() == before + 1u);
	CHECK(fake_core_last_line() == ((uint32_t)(uintptr_t)ring & ~31u));
	CHECK(u.rx_lost == 0);
}

/* A reader more than the ring (less the margin) behind loses the unread
 * bytes; it is counted and reading goes on with what comes next. */
static void test_dma_ring_lost(void)
{
	struct wb_uart u;
	uint8_t buf[RING];

	dma_open(&u);
	arrive_seq(0, 50);
	wb_uart_service(&u);
	arrive_seq(50, 47);	/* 97 unread: past 128 - 32 */
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 0);
	CHECK(u.rx_lost == 1);
	arrive_seq(97, 20);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 20 && is_seq(buf, 97, 20));
	CHECK(u.rx_lost == 1 && u.rx_gaps == 0);

	/* Within the margin, nothing is lost. */
	dma_open(&u);
	arrive_seq(0, 96);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 96 && is_seq(buf, 0, 96));
	CHECK(u.rx_lost == 0);
}

/* A pause in which the line could have carried a whole ring: bytes that
 * came are kept, and the possible lap counted. An idle line is not. */
static void test_dma_gap(void)
{
	struct wb_uart u;
	uint8_t buf[RING];
	/* 128 bytes of 9.9 us at 400 cycles/us. */
	const uint64_t lap = 128u * 3960u;

	dma_open(&u);
	wb_uart_service(&u);
	fake_core_set_cycles(fake_core_peek_cycles() + lap);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 0);
	CHECK(u.rx_gaps == 0);

	fake_core_set_cycles(fake_core_peek_cycles() + lap / 2u);
	arrive_seq(0, 10);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 10);
	CHECK(u.rx_gaps == 0);

	fake_core_set_cycles(fake_core_peek_cycles() + lap);
	arrive_seq(10, 10);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 10 && is_seq(buf, 10, 10));
	CHECK(u.rx_gaps == 1 && u.rx_lost == 0);

	/* The edge: a lap is possible from ring - 1 shortest bytes after the
	 * last sample on, the counter held still for the reads. */
	fake_core_set_cycle_step(0);
	fake_core_set_cycles(u.rx_seen + (lap - 3960u) - 1u);
	arrive_seq(20, 10);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 10 && is_seq(buf, 20, 10));
	CHECK(u.rx_gaps == 1);
	fake_core_set_cycles(u.rx_seen + (lap - 3960u));
	arrive_seq(30, 10);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 10 && is_seq(buf, 30, 10));
	CHECK(u.rx_gaps == 2 && u.rx_lost == 0);
	fake_core_set_cycle_step(1);
}

/* UART_STAT.OE is counted with DMA receive too (S-13b.070). */
static void test_dma_overrun(void)
{
	struct wb_uart u;

	dma_open(&u);
	dl.errors = UART_STAT_OE;
	wb_uart_service(&u);
	CHECK(u.overruns == 1 && u.line_errors == 1 && dl.errors == 0);
	wb_uart_service(&u);
	CHECK(u.overruns == 1);
}

/* A channel error: counted, the channel started over with its request
 * masked meanwhile, the unread bytes dropped. */
static void test_dma_error(void)
{
	struct wb_uart u;
	uint8_t buf[RING];
	int clr, cfg, set;

	dma_open(&u);
	arrive_seq(0, 10);
	fake_mmr_set(DMA_STAT(11), DMA_STAT_IRQERR | (3u << DMA_STAT_ERRC_SHIFT));
	fake_mmr_clear_log();
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 0);
	CHECK(u.dma_errors == 1);
	clr = write_at(UART_IMSK_CLR(0), UART_IMSK_ERBFI);
	cfg = write_at(DMA_CFG(11), 0x00001007u);
	set = write_at(UART_IMSK_SET(0), UART_IMSK_ERBFI);
	CHECK(clr >= 0 && cfg > clr && set > cfg);
	CHECK(write_at(DMA_CFG(11), 0) > clr && write_at(DMA_CFG(11), 0) < cfg);
	CHECK(!(fake_mmr_get(DMA_STAT(11)) & DMA_STAT_IRQERR));
	arrive_seq(10, 5);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 5 && is_seq(buf, 10, 5));
	CHECK(u.dma_errors == 1);
}

/* Held mode's interrupt: transmit through the status output; ERBFI stays
 * the channel's request and ELSI, which would take it, is never set. */
static void test_dma_irq(void)
{
	struct wb_uart u;
	uint8_t buf[8];

	dma_open(&u);
	wb_uart_use_irq(&u, true);
	CHECK(dl.imsk == (UART_IMSK_ERBFI | UART_IMSK_ETXS));
	wb_uart_write(&u, (const uint8_t *)"hi", 2);
	CHECK(dl.out_len == 2 && memcmp(dl.out, "hi", 2) == 0);
	arrive_seq(0, 4);
	wb_uart_isr(&u, WB_SID_UART0_STAT);
	CHECK(wb_uart_read(&u, buf, sizeof(buf)) == 4 && is_seq(buf, 0, 4));
	CHECK(dl.imsk & UART_IMSK_ERBFI);
	wb_uart_use_irq(&u, false);
	CHECK(dl.imsk == UART_IMSK_ERBFI);
	CHECK(!dl.imsk_elsi_seen && dl.rbr_reads == 0 && dl.dropped == 0);
}

/*
 * Closing: the transmitter empty first, with DMA11 filling the ring through
 * the wait; then DMA11 off before UART0, both off after (S-13b.072, .082).
 */
static void test_dma_disable(void)
{
	struct wb_uart u;
	int dma, ctl, temt;

	dma_open(&u);
	dl.busy = 4;
	fake_mmr_clear_log();
	CHECK(wb_uart_disable(0, &u, &cclk400) == WB_UART_OK);
	dma = write_at(DMA_CFG(11), 0);
	ctl = write_at(UART_CTL(0), 0);
	temt = last_at(FAKE_MMR_READ, UART_STAT(0));
	CHECK(temt >= 0 && dma > temt && ctl > dma);
	CHECK(dl.arrived == 4u && dl.dropped == 0);	/* none lost to the wait */
	CHECK(ring[0] == 0x40u && ring[3] == 0x43u);
	CHECK(close_ok(0, true, false));
	CHECK(!(fake_mmr_get(DMA_CFG(11)) & DMA_CFG_EN));
	CHECK(fake_mmr_get(UART_CTL(0)) == 0);
	CHECK(dl.imsk == 0);
	arrive_seq(0, 1);
	CHECK(dl.dropped == 1);
}

/* The DMA init after an earlier stage left UART0 on with a byte in flight
 * (S-13b.072): the byte leaves before UART_CTL = 0. */
static void test_dma_reinit(void)
{
	struct wb_uart u;

	dma_reset();
	fake_mmr_set(UART_CTL(0), 0x00000301u);
	dl.busy = 3;
	CHECK(wb_uart_init_dma(&u, 0, &uart0_cfg, &cclk400, 9900u, ring, RING,
			       txbuf, sizeof(txbuf)) == WB_UART_OK);
	CHECK(dl.busy == 0 && close_ok(0, true, false));
	CHECK(fake_mmr_get(UART_CTL(0)) == 0x00000301u);
}

int main(void)
{
	RUN(test_init);
	RUN(test_polled);
	RUN(test_rx_backpressure);
	RUN(test_tx_held);
	RUN(test_irq);
	RUN(test_line_errors);
	RUN(test_close_drained);
	RUN(test_close_cts_held);
	RUN(test_close_reinit);
	RUN(test_drain);
	RUN(test_dma_start);
	RUN(test_dma_reinit);
	RUN(test_dma_ring_wrap);
	RUN(test_dma_position);
	RUN(test_dma_ring_lost);
	RUN(test_dma_gap);
	RUN(test_dma_overrun);
	RUN(test_dma_error);
	RUN(test_dma_irq);
	RUN(test_dma_disable);
	return check_summary("test_drv_uart");
}

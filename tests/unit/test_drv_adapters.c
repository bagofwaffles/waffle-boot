/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The board adapters the S-09 code needs, written against the drivers as
 * a board would write them: the bridge (s09_bus_ops: S-13b.040-.041 and
 * S-13b.083 on SPI0 and its DMA, PC11 select, PA6 ready, PC5 attention),
 * the slave boot link (s09_boot_ops: S-13b.020-.021, SEL3) and the S-06
 * clock.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/bf70x_spi.h"
#include "waffle/s09_master.h"

static const struct wb_time cclk400 = { 400u };

#define PA6  WB_PIN(WB_PORT_A, 6)
#define PC5  WB_PIN(WB_PORT_C, 5)
#define PC11 WB_PIN(WB_PORT_C, 11)

static struct wb_spi spi0;

static bool bus_ready(void *ctx)
{
	(void)ctx;
	return !wb_gpio_get(PA6);
}

static bool bus_attention(void *ctx)
{
	(void)ctx;
	return !wb_gpio_get(PC5);
}

static void bus_select(void *ctx, bool on)
{
	(void)ctx;
	wb_gpio_set(PC11, !on);
}

/* S-13b.083: every session by SPI0's DMA. */
/* cppcheck-suppress constParameterCallback ; s09_bus_ops fixes the type */
static bool bus_send(void *ctx, const uint8_t *d, size_t n)
{
	return wb_spi_dma_send(ctx, NULL, d, n) == WB_SPI_OK;
}

/* cppcheck-suppress constParameterCallback ; s09_bus_ops fixes the type */
static bool bus_recv(void *ctx, uint8_t *d, size_t n)
{
	return wb_spi_dma_recv(ctx, d, n) == WB_SPI_OK;
}

/* cppcheck-suppress constParameterCallback ; s09_bus_ops fixes the type */
static void bus_end(void *ctx)
{
	wb_spi_dma_close(ctx);
}

static void bus_configure(void *ctx, int phase)
{
	if (phase == 0)
		(void)wb_spi_reconfigure(ctx, 0x00002592u, 3u);
	else
		(void)wb_spi_reconfigure(ctx, 0x00242592u, 1u);
}

/* A byte inside a session: the FIFOs and the status alone (S-09.005); then
 * the link's settle time by the cycle counter, when the board has one. */
static uint64_t boot_settle_cycles;

/* cppcheck-suppress constParameterCallback ; s09_boot_ops fixes the type */
static uint8_t boot_xfer(void *ctx, uint8_t tx)
{
	const uint8_t rx = (uint8_t)wb_spi_word(ctx, tx);

	if (boot_settle_cycles != 0u) {
		const uint64_t from = wb_core_cycles();

		while (wb_core_cycles() - from < boot_settle_cycles)
			;
	}
	return rx;
}

/* The chip select, and the session's set-up, once for all its bytes. */
/* cppcheck-suppress constParameterCallback ; s09_boot_ops fixes the type */
static void boot_select(void *ctx, bool on)
{
	if (on) {
		wb_spi_select(ctx, true);
		(void)wb_spi_words_begin(ctx);
	} else {
		(void)wb_spi_words_end(ctx);
		wb_spi_select(ctx, false);
	}
}

/*
 * BOOT_LINK_FC=1: the opening byte and S-09.007's one at a time, without
 * flow control; from the second session on SPI0's flow control on PA6,
 * set with the chip select high, and a session's bytes through the
 * transmit FIFO under a word count.
 */
static unsigned fc_sessions;

/* cppcheck-suppress constParameterCallback ; s09_boot_ops fixes the type */
static uint8_t fc_xfer(void *ctx, uint8_t tx)
{
	uint8_t rx = 0xFF;

	(void)wb_spi_xfer(ctx, &tx, &rx, 1);
	return rx;
}

static void fc_select(void *ctx, bool on)
{
	if (on && fc_sessions++ == 1u)
		(void)wb_spi_reconfigure(ctx, SPI_CTL_MSTR | SPI_CTL_CPHA | SPI_CTL_FCEN, 2u);
	wb_spi_select(ctx, on);
}

/* cppcheck-suppress constParameterCallback ; s09_boot_ops fixes the type */
static bool fc_send(void *ctx, const uint8_t *d, size_t n)
{
	const struct wb_spi *s = ctx;

	return (s->ctl & SPI_CTL_FCEN) != 0u && wb_spi_send(s, d, n) == WB_SPI_OK;
}

/* Loopback: what goes into TFIFO comes back inverted in RFIFO. */
static bool loop_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		      wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_WRITE && addr == SPI_TFIFO(0))
		fake_mmr_set(SPI_RFIFO(0), ~*value & 0xFFu);
	if (op == FAKE_MMR_READ && addr == SPI_STAT(0))
		*value = SPI_STAT_TF;	/* RFE clear, transfers finish */
	/* the bridge's DMA done at once, its send's TF taken */
	if (op == FAKE_MMR_READ && (addr == DMA_STAT(4) || addr == DMA_STAT(5)))
		*value = DMA_STAT_IRQDONE;
	if (op == FAKE_MMR_READ && addr == SPI_ILAT(0))
		*value = SPI_STAT_TF;
	return true;
}

static void test_bus_ops(void)
{
	const struct s09_bus_ops ops = {
		&spi0, bus_ready, bus_attention, bus_select, bus_send,
		bus_recv, bus_configure, bus_end,
	};
	static const struct wb_spi_config pre = { 0x00002592u, 3u, 0u, 0u };
	uint8_t w[4] = { 1, 2, 3, 4 };

	fake_mmr_reset();
	fake_core_reset();
	fake_mmr_set_hook(loop_hook, NULL);
	/* T_REPLY bounds one word (S-13b.040's flow control). */
	CHECK(wb_spi_init(&spi0, 0, &pre, &cclk400, S09_T_REPLY_US) ==
	      WB_SPI_OK);

	fake_mmr_set(PORT_DATA(WB_PORT_A), 0);
	fake_mmr_set(PORT_DATA(WB_PORT_C), 1u << 5);
	CHECK(ops.ready(ops.ctx));
	CHECK(!ops.attention(ops.ctx));
	ops.select(ops.ctx, true);
	CHECK(fake_mmr_get(PORT_DATA_CLR(WB_PORT_C)) == 1u << 11);
	CHECK(ops.send(ops.ctx, w, 4));
	CHECK(fake_mmr_get(DMA_CFG(4)) == 0u && !(fake_mmr_get(SPI_CTL(0)) & SPI_CTL_EN));
	CHECK(ops.recv(ops.ctx, w, 4));
	CHECK(fake_mmr_get(SPI_CTL(0)) == 0x00002593u);	/* EN set last, the part done */
	ops.end(ops.ctx);
	CHECK(fake_mmr_get(SPI_CTL(0)) == 0x00002592u && fake_mmr_get(DMA_CFG(5)) == 0u);
	CHECK(!ops.send(ops.ctx, w, 3));	/* not a whole word */
	ops.configure(ops.ctx, 1);
	CHECK(fake_mmr_get(SPI_CTL(0)) == 0x00242593u);
	CHECK(fake_mmr_get(SPI_CLK(0)) == 1u);
	ops.configure(ops.ctx, 0);
	CHECK(fake_mmr_get(SPI_CTL(0)) == 0x00002593u);
}

static void test_boot_ops(void)
{
	const struct s09_boot_ops ops = {
		&spi0, bus_ready, boot_select, boot_xfer, NULL, NULL,
	};
	/* S-13b.020/.021: mode 1, 8-bit, SEL3, SCLK0 / 3. */
	static const struct wb_spi_config link = {
		SPI_CTL_MSTR | SPI_CTL_CPHA, 2u, 0u, 3u,
	};

	fake_mmr_reset();
	fake_core_reset();
	fake_mmr_set_hook(loop_hook, NULL);
	CHECK(wb_spi_init(&spi0, 0, &link, &cclk400, 1000u) == WB_SPI_OK);
	ops.select(ops.ctx, true);
	CHECK(fake_mmr_get(SPI_SLVSEL(0)) ==
	      ((0xFE00u & ~(1u << 11)) | 8u));
	CHECK(fake_mmr_get(SPI_RXCTL(0)) == SPI_XCTL_EN);
	CHECK(fake_mmr_get(SPI_TXCTL(0)) == (SPI_XCTL_EN | SPI_XCTL_TI));
	CHECK(ops.xfer(ops.ctx, 0x5A) == 0xA5);
	/* a session's later bytes write nothing but the transmit FIFO */
	fake_mmr_clear_log();
	fake_mmr_set(PORT_DATA(WB_PORT_A), 0);
	for (unsigned i = 0; i < 64u; i++) {
		CHECK(ops.ready(ops.ctx));
		CHECK(ops.xfer(ops.ctx, (uint8_t)i) == (uint8_t)~i);
	}
	{
		const struct fake_mmr_access *l = fake_mmr_log();

		for (size_t i = 0; i < fake_mmr_count(); i++)
			CHECK(l[i].op == FAKE_MMR_READ ||
			      (l[i].op == FAKE_MMR_WRITE && l[i].addr == SPI_TFIFO(0)));
		CHECK(fake_mmr_count() == 64u * 4u);	/* PA6, TFIFO, STAT, RFIFO */
	}
	/* with no settle time the clock is never read; with one, a byte takes
	 * that long and touches the same registers */
	{
		uint64_t t0;

		fake_core_set_cycle_step(1);
		t0 = fake_core_peek_cycles();
		CHECK(ops.xfer(ops.ctx, 0x11) == 0xEE);
		CHECK(fake_core_peek_cycles() == t0);
		boot_settle_cycles = 800u;
		fake_mmr_clear_log();
		CHECK(ops.xfer(ops.ctx, 0x22) == 0xDD);
		CHECK(fake_core_peek_cycles() - t0 >= 800u);
		CHECK(fake_core_peek_cycles() - t0 <= 810u);
		CHECK(fake_mmr_count() == 3u);
		boot_settle_cycles = 0u;
	}
	/* PA6 high: not ready, and the adapter starts nothing by itself */
	fake_mmr_set(PORT_DATA(WB_PORT_A), 1u << 6);
	CHECK(!ops.ready(ops.ctx));
	ops.select(ops.ctx, false);
	CHECK(fake_mmr_get(SPI_RXCTL(0)) == 0u && fake_mmr_get(SPI_TXCTL(0)) == 0u);
	CHECK(fake_mmr_get(SPI_SLVSEL(0)) == (0xFE00u | 8u));
}

/* The controller as BOOT_LINK_FC's sessions see it: a session's bytes go
 * into the transmit FIFO, counted by TWC, with the select low and FCEN set.
 * The ready line holds it: no word leaves, and TF is not set. */
static struct {
	uint32_t twc;
	unsigned sent, bad;
	bool held;
	uint8_t log[64];
} fcm;

static bool fc_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		    wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_WRITE && addr == SPI_TWC(0))
		fcm.twc = *value;
	if (op == FAKE_MMR_WRITE && addr == SPI_TFIFO(0)) {
		const uint32_t txctl = fake_mmr_get(SPI_TXCTL(0));

		if (txctl & SPI_XCTL_WCEN) {
			/* a stream session: flow control on, the select low */
			if (!(fake_mmr_get(SPI_CTL(0)) & SPI_CTL_FCEN) ||
			    (fake_mmr_get(SPI_SLVSEL(0)) & (1u << 11)))
				fcm.bad++;
			if (fcm.sent < sizeof fcm.log)
				fcm.log[fcm.sent] = (uint8_t)*value;
			fcm.sent++;
			if (!fcm.held && fcm.twc > 0)
				fcm.twc--;
		} else {
			if (fake_mmr_get(SPI_CTL(0)) & SPI_CTL_FCEN)
				fcm.bad++;	/* a single byte goes without it */
			fake_mmr_set(SPI_RFIFO(0), ~*value & 0xFFu);
		}
	}
	if (op == FAKE_MMR_WRITE && addr == SPI_CTL(0) && !(*value & SPI_CTL_EN) &&
	    !(fake_mmr_get(SPI_SLVSEL(0)) & (1u << 11)))
		fcm.bad++;		/* disabled with the select low */
	if (op == FAKE_MMR_READ && addr == SPI_STAT(0)) {
		const bool counted = (fake_mmr_get(SPI_TXCTL(0)) & SPI_XCTL_WCEN) != 0u;

		*value = SPI_STAT_TFS_EMPTY;
		if (fcm.held)
			*value = SPI_STAT_FCS | SPI_STAT_TFF;	/* full, nothing leaves */
		else if (!counted || fcm.twc == 0)
			*value |= SPI_STAT_TF;
	}
	return true;
}

static void test_boot_ops_fc(void)
{
	const struct s09_boot_ops ops = {
		&spi0, bus_ready, fc_select, fc_xfer, fc_send, NULL,
	};
	static const struct wb_spi_config link = {
		SPI_CTL_MSTR | SPI_CTL_CPHA, 2u, 0u, 3u,
	};
	static const uint8_t block[20] = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
	};

	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(1);
	fc_sessions = 0;
	fake_mmr_set_hook(fc_hook, NULL);
	/* T_STREAM bounds one byte the slave holds (S-09.060) */
	CHECK(wb_spi_init(&spi0, 0, &link, &cclk400, S09_T_STREAM_US) == WB_SPI_OK);
	memset(&fcm, 0, sizeof fcm);

	/* the opening session: one byte, read back, no flow control */
	ops.select(ops.ctx, true);
	CHECK(fake_mmr_get(SPI_CTL(0)) == (link.ctl | SPI_CTL_EN));
	CHECK(ops.xfer(ops.ctx, 0x03) == 0xFC);
	CHECK(!ops.send(ops.ctx, block, sizeof block));	/* not before the stream */
	ops.select(ops.ctx, false);
	CHECK(fcm.sent == 0);

	/* the stream's sessions: S-13b.020, .021 with FCEN, RDY active low */
	ops.select(ops.ctx, true);
	CHECK(fake_mmr_get(SPI_CTL(0)) ==
	      (SPI_CTL_MSTR | SPI_CTL_CPHA | SPI_CTL_FCEN | SPI_CTL_EN));
	CHECK(fake_mmr_get(SPI_CLK(0)) == 2u);
	CHECK(fake_mmr_get(SPI_SLVSEL(0)) == ((0xFE00u & ~(1u << 11)) | 8u));
	CHECK(ops.send(ops.ctx, block, sizeof block));
	ops.select(ops.ctx, false);
	CHECK(fcm.sent == sizeof block);
	CHECK(memcmp(fcm.log, block, sizeof block) == 0);
	ops.select(ops.ctx, true);
	CHECK(ops.send(ops.ctx, block + 4, 16));
	ops.select(ops.ctx, false);
	CHECK(fcm.sent == sizeof block + 16u);
	CHECK(fcm.bad == 0);

	/* PA6 held high for good: the session fails after T_STREAM, not before
	 * and not much later, and nothing is left running */
	ops.select(ops.ctx, true);
	fcm.held = true;
	fake_core_set_cycle_step(400u * 1000u);		/* a millisecond a look */
	fake_core_set_cycles(0);
	CHECK(!ops.send(ops.ctx, block, sizeof block));
	CHECK(fake_core_peek_cycles() >= (uint64_t)S09_T_STREAM_US * 400u);
	CHECK(fake_core_peek_cycles() <= ((uint64_t)S09_T_STREAM_US + 50000u) * 400u);
	ops.select(ops.ctx, false);
	CHECK(fake_mmr_get(SPI_TXCTL(0)) == 0u && fake_mmr_get(SPI_RXCTL(0)) == 0u);
	CHECK(fcm.bad == 0);
}

static void idle_none(void *ctx, uint32_t until_us)
{
	(void)ctx;
	(void)until_us;
}

static void test_clock(void)
{
	struct wb_time t = cclk400;
	const struct s06_clock clk = { wb_time_now_us, idle_none, &t };

	fake_core_reset();
	fake_core_set_cycle_step(0);
	fake_core_set_cycles(400u * 1234u);
	CHECK(s06_now(&clk) == 1234u);
	s06_idle(&clk, 2000u);
}

int main(void)
{
	RUN(test_bus_ops);
	RUN(test_boot_ops);
	RUN(test_boot_ops_fc);
	RUN(test_clock);
	return check_summary("test_drv_adapters");
}

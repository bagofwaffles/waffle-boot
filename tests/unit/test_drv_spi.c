/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tests for the SPI controllers and the SPI flash commands
 * (src/drivers/bf70x/spi.c, spiflash.c), and the master's bridge sessions by
 * DMA (S-13b.083), judged by the checkers of s13b_check.h.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_spiflash.h"
#include "waffle/s02_flash.h"

#include "s13b_check.h"
#include "s13b_trace.h"
#include "vectors/s13b_vectors.h"

static const struct wb_time cclk400 = { 400u };

#define FLASH_SIZE 0x100000u

/*
 * A device on one controller. Words written to TFIFO are answered by
 * reply() and queued for RFIFO while the receive channel is on; RWC words
 * are clocked in when the receive channel is started with RTI and RWCEN;
 * TF is set when TWC words have gone. The select line is SPI_SELn of
 * SLVSEL; as silicon anomaly 19000038 describes, a SLVSEL value takes
 * effect only when it is written twice back to back. As a flash it checks
 * S-13b.013-.016 at the end of every frame.
 */
struct dev {
	unsigned n, sel;
	uint32_t (*reply)(struct dev *d, uint32_t tx);
	uint32_t q[600];
	size_t qh, qt;
	uint32_t twc, rwc, txctl, rxctl, stat;
	uint32_t rx_due;	/* words of a long receive not yet clocked in */
	uint32_t clk, ctl;	/* as written */
	unsigned too_fast;	/* enabled with a divider below `slowest` */
	uint32_t slowest;
	unsigned frames_at[4];	/* READ frames by the divider they ran at */
	uint32_t read_addr[32];	/* each READ frame's address */
	unsigned nreads;
	unsigned tfifo;		/* words written to the transmit FIFO */
	unsigned fast_frames;	/* READ frames with SPI_CTL.FMODE set */
	unsigned reloads;	/* counted transfers started with a reload not 0 */
	bool selected, mute, raise_ror;
	/* S-13b.017: a receive overrun in the last transfer of the next
	 * fail_left frames with opcode fail_op (its first word: the opcode
	 * for a WREN, A0 for an SE, the first data byte for a READ or PP) */
	uint8_t fail_op;
	unsigned fail_left;
	bool slvsel_armed;	/* the last write was SLVSEL = slvsel */
	uint32_t slvsel;
	/* flash */
	uint8_t frame[300];
	size_t flen;
	uint8_t mem[FLASH_SIZE];
	bool wel, wip, stuck;
	unsigned busy_polls, busy_set;
	uint8_t ops[64];
	uint8_t last_op;
	unsigned nops, violations;
};

static struct dev dev;

static uint8_t flash_status(const struct dev *d)
{
	return d->wip ? WB_SPIFLASH_WIP : 0;
}

static uint32_t flash_reply(struct dev *d, uint32_t tx)
{
	size_t pos = d->flen;
	uint32_t a;

	if (!d->selected) {
		d->violations++;
		return 0xFF;
	}
	if (pos < sizeof(d->frame))
		d->frame[pos] = (uint8_t)tx;
	d->flen++;
	if (pos == 0)
		return 0xFF;
	switch (d->frame[0]) {
	case WB_SPIFLASH_RDSR:
		return flash_status(d);
	case WB_SPIFLASH_RDID:
		return pos <= 3 ? (const uint8_t[]){ 0, 0xEF, 0x40, 0x14 }[pos]
				: 0xFF;
	case WB_SPIFLASH_REMS:
		return pos == 4 ? 0xEF : pos == 5 ? 0x13 : 0xFF;
	case WB_SPIFLASH_READ:
		if (pos < 4)
			return 0xFF;
		a = (uint32_t)d->frame[1] << 16 | (uint32_t)d->frame[2] << 8 |
		    d->frame[3];
		return d->mem[(a + pos - 4u) % FLASH_SIZE];
	default:
		return 0xFF;
	}
}

static void flash_end_frame(struct dev *d)
{
	uint8_t op = d->frame[0];
	uint32_t a = (uint32_t)d->frame[1] << 16 | (uint32_t)d->frame[2] << 8 |
		     d->frame[3];

	if (d->flen == 0)
		return;
	if (d->nops < ARRAY_LEN(d->ops))
		d->ops[d->nops] = op;
	d->nops++;
	d->last_op = op;
	if (d->wip && op != WB_SPIFLASH_RDSR)
		d->violations++;		/* S-13b.015 */
	switch (op) {
	case WB_SPIFLASH_RDSR:
		if (d->flen < 2)
			d->violations++;
		if (d->wip && !d->stuck && --d->busy_polls == 0)
			d->wip = false;
		break;
	case WB_SPIFLASH_WREN:
		if (d->flen != 1)
			d->violations++;
		d->wel = true;
		break;
	case WB_SPIFLASH_SE:
	case WB_SPIFLASH_PP:
		if (!d->wel || d->flen < 4 || a >= FLASH_SIZE)
			d->violations++;	/* S-13b.013, .014 */
		if (op == WB_SPIFLASH_SE) {
			if (d->flen != 4 || (a & 0xFFFu))
				d->violations++;	/* S-13b.016 */
			memset(d->mem + (a & ~0xFFFu), 0xFF, 4096);
		} else {
			size_t i;

			if (d->flen < 5 || a % 256u + (d->flen - 4u) > 256u)
				d->violations++;
			for (i = 4; i < d->flen && i < sizeof(d->frame); i++)
				d->mem[(a + i - 4u) % FLASH_SIZE] &=
					d->frame[i];
		}
		d->wel = false;
		d->wip = true;
		d->busy_polls = d->busy_set;
		break;
	case WB_SPIFLASH_READ:
		if (d->flen < 5 || a >= FLASH_SIZE)
			d->violations++;
		d->wel = false;
		if (d->clk < ARRAY_LEN(d->frames_at))
			d->frames_at[d->clk]++;
		if (d->ctl & SPI_CTL_FMODE)
			d->fast_frames++;
		if (d->nreads < ARRAY_LEN(d->read_addr))
			d->read_addr[d->nreads] = a;
		d->nreads++;
		break;
	case WB_SPIFLASH_RDID:
	case WB_SPIFLASH_REMS:
		d->wel = false;
		break;
	default:
		d->violations++;		/* S-13b.013 */
		break;
	}
	d->flen = 0;
}

static uint32_t echo_reply(struct dev *d, uint32_t tx)
{
	(void)d;
	return ~tx;
}

static bool dev_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		     wb_mmr_addr addr, uint32_t *value)
{
	struct dev *d = ctx;
	unsigned n = d->n;

	(void)width;
	if (op == FAKE_MMR_READ) {
		if (addr == SPI_STAT(n)) {
			/* a long receive's words come as they are asked for */
			if (d->qh == d->qt && d->rx_due > 0 && !d->mute) {
				d->q[d->qt++ % ARRAY_LEN(d->q)] = d->reply(d, 0);
				if (--d->rx_due == 0)
					d->stat |= SPI_STAT_RF;
			}
			*value = d->stat | (4u << 16);
			if (d->qh == d->qt)
				*value |= SPI_STAT_RFE;
		} else if (addr == SPI_RFIFO(n) && d->qh != d->qt) {
			*value = d->q[d->qh++ % ARRAY_LEN(d->q)];
		}
		return true;
	}
	if (op != FAKE_MMR_WRITE)
		return true;
	if (addr != SPI_SLVSEL(n))
		d->slvsel_armed = false;
	if (addr == SPI_STAT(n)) {
		d->stat &= ~(*value & SPI_STAT_W1C);
	} else if (addr == SPI_CLK(n)) {
		d->clk = *value;
	} else if (addr == SPI_CTL(n)) {
		d->ctl = *value;
		if ((*value & SPI_CTL_EN) && d->clk < d->slowest)
			d->too_fast++;		/* S-13b.011 */
		if (*value & SPI_CTL_MMSE)
			d->violations++;	/* S-13b.019 */
	} else if (addr == SPI_MMRDH(n) || addr == SPI_MMTOP(n)) {
		d->violations++;		/* S-13b.019 */
	} else if (addr == SPI_TWC(n)) {
		d->twc = *value;
	} else if (addr == SPI_RWC(n)) {
		d->rwc = *value;
	} else if (addr == SPI_TXCTL(n)) {
		d->txctl = *value;
		/* a count that runs out must not be loaded again (HRM, SPI_TWCR) */
		if ((*value & SPI_XCTL_WCEN) && fake_mmr_get(SPI_TWCR(n)) != 0u)
			d->reloads++;
	} else if (addr == SPI_RXCTL(n)) {
		uint32_t start = SPI_XCTL_EN | SPI_XCTL_TI | SPI_XCTL_WCEN;

		d->rxctl = *value;
		if ((*value & SPI_XCTL_WCEN) && fake_mmr_get(SPI_RWCR(n)) != 0u)
			d->reloads++;
		if ((*value & start) == start && d->rwc > ARRAY_LEN(d->q) / 2u) {
			d->rx_due = d->rwc;
			d->rwc = 0;
		} else if ((*value & start) == start && !d->mute) {
			for (; d->rwc > 0; d->rwc--)
				d->q[d->qt++ % ARRAY_LEN(d->q)] =
					d->reply(d, 0);
			d->stat |= SPI_STAT_RF;
		}
		if (!(*value & SPI_XCTL_EN))
			d->rx_due = 0;
	} else if (addr == SPI_TFIFO(n)) {
		uint32_t r;

		if (!(d->txctl & SPI_XCTL_EN))
			d->violations++;
		d->tfifo++;
		if (d->mute)
			return true;
		if (d->raise_ror)
			d->stat |= SPI_STAT_ROR;
		r = d->reply(d, *value);
		if (d->fail_left > 0u && d->flen >= 1u &&
		    d->frame[0] == d->fail_op &&
		    d->flen - 1u == (d->fail_op == WB_SPIFLASH_WREN ? 0u :
				     d->fail_op == WB_SPIFLASH_SE ? 3u : 4u)) {
			d->stat |= SPI_STAT_ROR;
			d->fail_left--;
		}
		if (d->rxctl & SPI_XCTL_EN)
			d->q[d->qt++ % ARRAY_LEN(d->q)] = r;
		if ((d->txctl & SPI_XCTL_WCEN) && d->twc > 0 && --d->twc == 0)
			d->stat |= SPI_STAT_TF;
	} else if (addr == SPI_SLVSEL(n)) {
		bool on = (*value & SPI_SLVSEL_SSE(d->sel)) &&
			  !(*value & SPI_SLVSEL_SSEL(d->sel));

		if (!d->slvsel_armed || d->slvsel != *value) {
			d->slvsel_armed = true;	/* the first of the pair */
			d->slvsel = *value;
			return true;
		}
		d->slvsel_armed = false;
		if (d->selected && !on)
			flash_end_frame(d);
		d->selected = on;
	}
	return true;
}

static void reset_dev(unsigned n, unsigned sel,
		      uint32_t (*reply)(struct dev *, uint32_t))
{
	fake_mmr_reset();
	fake_core_reset();
	memset(&dev, 0, sizeof(dev));
	memset(dev.mem, 0xFF, sizeof(dev.mem));
	dev.n = n;
	dev.sel = sel;
	dev.reply = reply;
	dev.busy_set = 2;
	fake_mmr_set_hook(dev_hook, &dev);
}

/* S-13b.010/.011: SPI2 master, mode 0, 8-bit, SEL1, 25 MHz. */
static const struct wb_spi_config flash_cfg = { SPI_CTL_MSTR, 3, 0, 1 };

static void flash_setup(struct wb_spi *s, struct wb_spiflash *f)
{
	reset_dev(2, 1, flash_reply);
	/* as a boot ROM that read the flash by counts may have left them */
	fake_mmr_set(SPI_RWCR(2), 0x1234u);
	fake_mmr_set(SPI_TWCR(2), 0x40u);
	CHECK(wb_spi_init(s, 2, &flash_cfg, &cclk400, 100) == WB_SPI_OK);
	CHECK(fake_mmr_get(SPI_RWCR(2)) == 0u && fake_mmr_get(SPI_TWCR(2)) == 0u);
	*f = (struct wb_spiflash){ s, FLASH_SIZE, 1000000u, false, NULL, 0 };
}

static void test_init(void)
{
	static const struct fake_mmr_access seq[] = {
		FAKE_W32(SPI_CTL(2), 0),
		FAKE_W32(SPI_SLVSEL(2), 0xFE02u),
		FAKE_W32(SPI_SLVSEL(2), 0xFE02u),	/* 19000038 */
		FAKE_W32(SPI_CLK(2), 3u),
		FAKE_W32(SPI_DLY(2), 0),
		FAKE_W32(SPI_RXCTL(2), 0),
		FAKE_W32(SPI_TXCTL(2), 0),
		FAKE_W32(SPI_RWCR(2), 0),	/* no reload of a count that runs out */
		FAKE_W32(SPI_TWCR(2), 0),
		FAKE_W32(SPI_IMSK_CLR(2), 0xFFFFu),
		FAKE_W32(SPI_STAT(2), SPI_STAT_W1C),
		FAKE_W32(SPI_CTL(2), 3u),
		FAKE_SYNC,
	};
	/* S-13b.040-.042's controller values, without EN. */
	static const struct wb_spi_config ok[] = {
		{ 0x00002592u, 3, 0, 0 },
		{ 0x00242592u, 1, 0, 0 },
		{ 0x00002510u, 0, 0, 0 },
		{ 0x00202510u, 0, 0, 0 },
	};
	static const struct wb_spi_config bad[] = {
		{ SPI_CTL_EN, 3, 0, 0 },
		{ SPI_CTL_SIZE_MASK, 3, 0, 0 },
		{ SPI_CTL_MIOM_MASK, 3, 0, 0 },
		{ SPI_CTL_MMSE, 3, 0, 0 },	/* S-13b.019 */
		{ 0, 0x10000u, 0, 0 },
		{ 0, 0, 0x400u, 0 },
		{ 0, 0, 0, 8 },
	};
	struct wb_spi s;
	unsigned i;

	reset_dev(2, 1, flash_reply);
	CHECK(wb_spi_init(&s, 2, &flash_cfg, &cclk400, 100) == WB_SPI_OK);
	CHECK(fake_mmr_expect("init", seq, ARRAY_LEN(seq)));
	CHECK(wb_spi_word_bytes(&s) == 1u);

	for (i = 0; i < ARRAY_LEN(ok); i++)
		CHECK(wb_spi_check(0, &ok[i]) == WB_SPI_OK);
	for (i = 0; i < ARRAY_LEN(bad); i++)
		CHECK(wb_spi_check(0, &bad[i]) == WB_SPI_ERR_ARG);
	CHECK(wb_spi_check(3, &ok[0]) == WB_SPI_ERR_ARG);
}

static void test_flash_read_and_id(void)
{
	struct wb_spi s;
	struct wb_spiflash f;
	uint8_t buf[16];
	unsigned i;

	flash_setup(&s, &f);
	for (i = 0; i < 16; i++)
		dev.mem[0x1230u + i] = (uint8_t)(i * 7u);
	CHECK(wb_spiflash_read(&f, 0x1230u, buf, 16) == WB_SPIFLASH_OK);
	for (i = 0; i < 16; i++)
		CHECK(buf[i] == (uint8_t)(i * 7u));
	CHECK(dev.nops == 1 && dev.ops[0] == WB_SPIFLASH_READ);
	CHECK(dev.frame[1] == 0x00 && dev.frame[2] == 0x12 &&
	      dev.frame[3] == 0x30);
	/* Don't-care bytes are 0x00. */
	CHECK(dev.frame[4] == 0 && dev.frame[19] == 0);
	CHECK(dev.violations == 0);
	CHECK(!dev.selected);
}

static void test_flash_write(void)
{
	static const uint8_t data[4] = { 0x12, 0x34, 0x56, 0x78 };
	static const uint8_t want[] = {
		/* WIP reads 1 twice, then 0. */
		WB_SPIFLASH_WREN, WB_SPIFLASH_SE, WB_SPIFLASH_RDSR,
		WB_SPIFLASH_RDSR, WB_SPIFLASH_RDSR,
		WB_SPIFLASH_WREN, WB_SPIFLASH_PP, WB_SPIFLASH_RDSR,
		WB_SPIFLASH_RDSR, WB_SPIFLASH_RDSR,
	};
	struct wb_spi s;
	struct wb_spiflash f;

	flash_setup(&s, &f);
	dev.mem[0x2000] = 0;
	CHECK(wb_spiflash_erase(&f, 0x2000u) == WB_SPIFLASH_OK);
	CHECK(dev.mem[0x2000] == 0xFF);
	CHECK(wb_spiflash_program(&f, 0x20FCu, data, 4) == WB_SPIFLASH_OK);
	CHECK(memcmp(dev.mem + 0x20FC, data, 4) == 0);
	CHECK(dev.nops == ARRAY_LEN(want));
	CHECK(memcmp(dev.ops, want, sizeof(want)) == 0);
	CHECK(dev.violations == 0);
}

static void test_flash_args(void)
{
	static const uint8_t page[257];
	struct wb_spi s;
	struct wb_spiflash f;
	uint8_t b;

	flash_setup(&s, &f);
	fake_mmr_clear_log();
	CHECK(wb_spiflash_erase(&f, 0x2100u) == WB_SPIFLASH_ERR_ARG);
	CHECK(wb_spiflash_erase(&f, FLASH_SIZE) == WB_SPIFLASH_ERR_ARG);
	CHECK(wb_spiflash_program(&f, 0x20FDu, page, 4) ==
	      WB_SPIFLASH_ERR_ARG);
	CHECK(wb_spiflash_program(&f, 0x2000u, page, 257) ==
	      WB_SPIFLASH_ERR_ARG);
	CHECK(wb_spiflash_program(&f, 0x2000u, page, 0) ==
	      WB_SPIFLASH_ERR_ARG);
	CHECK(wb_spiflash_read(&f, FLASH_SIZE - 1u, &b, 2) ==
	      WB_SPIFLASH_ERR_ARG);
	CHECK(wb_spiflash_read(&f, 0, &b, 0) == WB_SPIFLASH_ERR_ARG);
	CHECK(fake_mmr_count() == 0);
	CHECK(wb_spiflash_program(&f, 0x2000u, page, 256) ==
	      WB_SPIFLASH_OK);
	CHECK(dev.violations == 0);
}

/* S-13b.017: WIP never clears; later calls send nothing but RDSR. */
static void test_flash_stuck(void)
{
	static const uint8_t data[1] = { 0 };
	struct wb_spi s;
	struct wb_spiflash f;
	uint8_t b;
	unsigned before;

	flash_setup(&s, &f);
	fake_core_set_cycle_step(4000000);	/* 10 ms per cycle read */
	dev.stuck = true;
	CHECK(wb_spiflash_program(&f, 0, data, 1) == WB_SPIFLASH_BUSY);
	CHECK(f.busy);
	/* At least 1 s passed (400 cycles per µs). */
	CHECK(fake_core_peek_cycles() >= 400000000ull);
	before = dev.nops;
	CHECK(wb_spiflash_read(&f, 0, &b, 1) == WB_SPIFLASH_BUSY);
	CHECK(dev.nops > before);
	CHECK(dev.last_op == WB_SPIFLASH_RDSR);
	CHECK(dev.violations == 0);

	dev.stuck = false;
	dev.busy_polls = 1;
	CHECK(wb_spiflash_read(&f, 0, &b, 1) == WB_SPIFLASH_OK);
	CHECK(!f.busy);
	CHECK(dev.last_op == WB_SPIFLASH_READ);
	CHECK(dev.violations == 0);
}

/* The ops sent, as a string of letters, for the retry tests: R READ,
 * S RDSR, W WREN, E SE, P PP. */
static const char *ops_str(void)
{
	static char s[ARRAY_LEN(dev.ops) + 1u];
	unsigned i, n = dev.nops < ARRAY_LEN(dev.ops) ? dev.nops : ARRAY_LEN(dev.ops);

	for (i = 0; i < n; i++)
		s[i] = dev.ops[i] == WB_SPIFLASH_READ ? 'R' :
		       dev.ops[i] == WB_SPIFLASH_RDSR ? 'S' :
		       dev.ops[i] == WB_SPIFLASH_WREN ? 'W' :
		       dev.ops[i] == WB_SPIFLASH_SE ? 'E' :
		       dev.ops[i] == WB_SPIFLASH_PP ? 'P' : '?';
	s[n] = '\0';
	return s;
}

static bool ops_are(const char *want)
{
	const char *got = ops_str();

	if (strcmp(got, want) == 0)
		return true;
	printf("  ops %s, want %s\n", got, want);
	return false;
}

/*
 * S-13b.017, S-13b.015, S-13b.014: in a writer (three tries), a READ, an
 * SE, a PP or a WREN that ends with a controller error is sent again from
 * its first frame, the WREN included, after an RDSR has read WIP = 0, and
 * the operation completes; failing on every try, three tries are made and
 * the operation fails. Outside a writer, one try.
 */
static void test_flash_retry(void)
{
	static const uint8_t data[2] = { 0x12, 0x34 };
	struct wb_spi s;
	struct wb_spiflash f;
	uint8_t buf[4];

	/* A READ failing once, in a writer: RDSR, then the READ again. */
	flash_setup(&s, &f);
	f.tries = 3;
	dev.mem[0x100] = 0xA5;
	dev.fail_op = WB_SPIFLASH_READ;
	dev.fail_left = 1;
	CHECK(wb_spiflash_read(&f, 0x100u, buf, 4) == WB_SPIFLASH_OK);
	CHECK(buf[0] == 0xA5);
	CHECK(ops_are("RSR"));
	CHECK(dev.violations == 0);

	/* An SE failing once: the part may have started; WIP = 0 read, then
	 * WREN and SE again. The model holds WIP for two status reads. */
	flash_setup(&s, &f);
	f.tries = 3;
	dev.mem[0x3000] = 0;
	dev.fail_op = WB_SPIFLASH_SE;
	dev.fail_left = 1;
	CHECK(wb_spiflash_erase(&f, 0x3000u) == WB_SPIFLASH_OK);
	CHECK(dev.mem[0x3000] == 0xFF);
	CHECK(ops_are("WESSSWESSS"));
	CHECK(dev.violations == 0 && !f.busy);

	/* A PP failing once. */
	flash_setup(&s, &f);
	f.tries = 3;
	dev.fail_op = WB_SPIFLASH_PP;
	dev.fail_left = 1;
	CHECK(wb_spiflash_program(&f, 0x3010u, data, 2) == WB_SPIFLASH_OK);
	CHECK(dev.mem[0x3010] == 0x12 && dev.mem[0x3011] == 0x34);
	CHECK(ops_are("WPSSSWPSSS"));
	CHECK(dev.violations == 0);

	/* A WREN failing once: RDSR, then the WREN and its SE. */
	flash_setup(&s, &f);
	f.tries = 3;
	dev.fail_op = WB_SPIFLASH_WREN;
	dev.fail_left = 1;
	CHECK(wb_spiflash_erase(&f, 0x3000u) == WB_SPIFLASH_OK);
	CHECK(ops_are("WSWESSS"));
	CHECK(dev.violations == 0);

	/* An SE failing on every try: three, each after WIP = 0 but the
	 * first, then the failure, the part left marked busy. */
	flash_setup(&s, &f);
	f.tries = 3;
	dev.fail_op = WB_SPIFLASH_SE;
	dev.fail_left = 100;
	CHECK(wb_spiflash_erase(&f, 0x3000u) == WB_SPIFLASH_ERR_HW);
	CHECK(ops_are("WESSSWESSSWE"));
	CHECK(dev.violations == 0 && f.busy);
	/* The next operation waits for WIP = 0 first (S-13b.015). */
	dev.fail_left = 0;
	CHECK(wb_spiflash_read(&f, 0, buf, 1) == WB_SPIFLASH_OK);
	CHECK(ops_are("WESSSWESSSWESSSR"));
	CHECK(dev.violations == 0);

	/* A READ failing on every try. */
	flash_setup(&s, &f);
	f.tries = 3;
	dev.fail_op = WB_SPIFLASH_READ;
	dev.fail_left = 100;
	CHECK(wb_spiflash_read(&f, 0, buf, 1) == WB_SPIFLASH_ERR_HW);
	CHECK(ops_are("RSRSR"));

	/* Outside a writer: one try, the failure at once. */
	flash_setup(&s, &f);
	f.tries = 1;
	dev.fail_op = WB_SPIFLASH_READ;
	dev.fail_left = 1;
	CHECK(wb_spiflash_read(&f, 0, buf, 1) == WB_SPIFLASH_ERR_HW);
	CHECK(ops_are("R"));
	flash_setup(&s, &f);	/* tries 0 is one try too */
	dev.fail_op = WB_SPIFLASH_PP;
	dev.fail_left = 1;
	CHECK(wb_spiflash_program(&f, 0, data, 1) == WB_SPIFLASH_ERR_HW);
	CHECK(ops_are("WP"));
	CHECK(f.busy);

	/* A timeout is not a controller error: no second try. */
	flash_setup(&s, &f);
	f.tries = 3;
	dev.mute = true;
	CHECK(wb_spiflash_read(&f, 0, buf, 1) == WB_SPIFLASH_ERR_IO);
	CHECK(dev.tfifo == 1u);
}

/* S-13b.015: a flash marked busy at the start sends an RDSR that reads
 * WIP = 0 before its first other frame. */
static void test_flash_first_frame(void)
{
	struct wb_spi s;
	struct wb_spiflash f;
	uint8_t b;

	flash_setup(&s, &f);
	f.busy = true;
	dev.wip = true;		/* a reset cut an erase short */
	dev.busy_polls = 2;
	CHECK(wb_spiflash_read(&f, 0, &b, 1) == WB_SPIFLASH_OK);
	CHECK(ops_are("SSSR"));
	CHECK(dev.violations == 0);
}

/* S-13b.011, .013, .019: a slot's stream read at SCLK0 / 2. READ frames
 * only, each with its address, the data by the receive channel's count;
 * the bus back at SCLK0 / 4 afterwards, for every other command. */
static void test_flash_read_fast(void)
{
	static uint8_t buf[0x12345u];
	const uint32_t at = 0x10000u, n = sizeof(buf);
	struct wb_spi s;
	struct wb_spiflash f;
	uint8_t sr = 0xFF, b[4];
	uint32_t i;

	flash_setup(&s, &f);
	dev.slowest = 1u;
	for (i = 0; i < FLASH_SIZE; i++)
		dev.mem[i] = (uint8_t)(i * 31u + (i >> 8) * 7u + (i >> 16));
	CHECK(wb_spiflash_read_at(&f, 1u, at, buf, n) == WB_SPIFLASH_OK);
	CHECK(memcmp(buf, dev.mem + at, n) == 0);
	/* three frames, at 50 MHz, each READ with its own address */
	CHECK(dev.nops == 3u && dev.nreads == 3u);
	CHECK(dev.ops[0] == WB_SPIFLASH_READ && dev.ops[1] == WB_SPIFLASH_READ &&
	      dev.ops[2] == WB_SPIFLASH_READ);
	CHECK(dev.frames_at[1] == 3u && dev.frames_at[3] == 0u);
	/* in fast mode, all three: full-cycle receive timing at SCLK0 / 2 */
	CHECK(dev.fast_frames == 3u);
	CHECK(dev.reloads == 0u);
	CHECK(dev.read_addr[0] == at && dev.read_addr[1] == at + WB_SPIFLASH_FAST_FRAME &&
	      dev.read_addr[2] == at + 2u * WB_SPIFLASH_FAST_FRAME);
	CHECK(dev.violations == 0 && dev.too_fast == 0);
	CHECK(!dev.selected);
	/* the data by the receive channel: no transmit FIFO write but the heads' */
	CHECK(dev.tfifo == 3u * 4u);
	/* the usual rate again, the configuration kept, both channels off */
	CHECK(fake_mmr_get(SPI_CLK(2)) == 3u);
	CHECK(fake_mmr_get(SPI_CTL(2)) == (flash_cfg.ctl | SPI_CTL_EN));
	CHECK(s.clk == 3u);
	CHECK(dev.rxctl == 0 && dev.txctl == 0);
	/* and the commands after it run there */
	CHECK(wb_spiflash_status(&f, &sr) == WB_SPIFLASH_OK && sr == 0);
	CHECK(wb_spiflash_read(&f, at + 5u, b, 4) == WB_SPIFLASH_OK);
	CHECK(memcmp(b, dev.mem + at + 5u, 4) == 0);
	CHECK(dev.frames_at[3] == 1u && dev.frames_at[1] == 3u);
	CHECK(dev.fast_frames == 3u);		/* not the frames at the usual rate */
	CHECK(dev.too_fast == 0u);

	/* a frame's worth, less and one more: one frame, one, two */
	for (i = 0; i < 3u; i++) {
		const uint32_t len = WB_SPIFLASH_FAST_FRAME - 1u + i;

		flash_setup(&s, &f);
		dev.mem[0x20000u + len - 1u] = 0xC3u;
		CHECK(wb_spiflash_read_at(&f, 1u, 0x20000u, buf, len) == WB_SPIFLASH_OK);
		CHECK(buf[len - 1u] == 0xC3u);
		CHECK(dev.nreads == (i == 2u ? 2u : 1u));
	}

	/* refused: nothing, past the part, a busy part (S-13b.015) */
	flash_setup(&s, &f);
	CHECK(wb_spiflash_read_at(&f, 1u, 0, buf, 0) == WB_SPIFLASH_ERR_ARG);
	CHECK(wb_spiflash_read_at(&f, 1u, FLASH_SIZE - 1u, buf, 2) == WB_SPIFLASH_ERR_ARG);
	CHECK(wb_spiflash_read_at(&f, 0x10000u, 0, buf, 1) == WB_SPIFLASH_ERR_ARG);
	CHECK(dev.nops == 0);
	CHECK(fake_mmr_get(SPI_CLK(2)) == 3u);
	CHECK(fake_mmr_get(SPI_CTL(2)) == (flash_cfg.ctl | SPI_CTL_EN));
	/* the control value in a fast frame is the configured one and the bit,
	 * written with the controller disabled just before; a slave has none */
	{
		static const struct fake_mmr_access fast[] = {
			FAKE_W32(SPI_CTL(2), 0),
			FAKE_W32(SPI_CLK(2), 1u),
			FAKE_W32(SPI_CTL(2), SPI_CTL_MSTR | SPI_CTL_FMODE | SPI_CTL_EN),
			FAKE_SYNC,
		};
		static const struct wb_spi_config slave = { 0x00002510u, 0, 0, 0 };
		struct wb_spi sl;

		fake_mmr_clear_log();
		CHECK(wb_spi_clock(&s, 1u, true) == WB_SPI_OK);
		CHECK(fake_mmr_expect("fast", fast, ARRAY_LEN(fast)));
		CHECK(wb_spi_clock(&s, s.clk, false) == WB_SPI_OK);
		CHECK(fake_mmr_get(SPI_CTL(2)) == (flash_cfg.ctl | SPI_CTL_EN));
		CHECK(wb_spi_init(&sl, 0, &slave, &cclk400, 100) == WB_SPI_OK);
		CHECK(wb_spi_clock(&sl, 0u, true) == WB_SPI_ERR_ARG);
	}

	/* the words stop: an error within a word's bound, the bus put back */
	flash_setup(&s, &f);
	dev.mute = true;
	fake_core_set_cycle_step(1);
	fake_core_set_cycles(0);
	CHECK(wb_spiflash_read_at(&f, 1u, at, buf, n) == WB_SPIFLASH_ERR_IO);
	CHECK(fake_core_peek_cycles() <= 400u * 400u);
	CHECK(fake_mmr_get(SPI_CLK(2)) == 3u);
	CHECK(fake_mmr_get(SPI_CTL(2)) == (flash_cfg.ctl | SPI_CTL_EN));	/* no fast mode left */
	CHECK(!dev.selected);
	CHECK(dev.rxctl == 0 && dev.txctl == 0);
	dev.mute = false;
	CHECK(wb_spiflash_read(&f, at, b, 4) == WB_SPIFLASH_OK);
	CHECK(memcmp(b, dev.mem + at, 4) == 0);
}

/*
 * DMA9, SPI2's receive channel, in front of the device: started by its
 * DMA_CFG write, it takes every word the device has for the receive FIFO
 * when its status is read (none while rxd.stall), into the buffer
 * registered in rxd.base (the register holds the low 32 bits of the host's
 * address), and shows done once
 * its count is moved; its current address and SPI_RWC follow the words
 * moved. It notes a start that is not a byte-wide receive with the done
 * interrupt, one made with the receive channel already on, and a receive
 * channel on with no data request while it runs. With no_move it shows done
 * at once with nothing moved, its address 0, while the device fills the
 * receive FIFO; with `extra` it leaves that many more words in the FIFO at
 * its done. The device keeps its receive FIFO when the controller is
 * disabled, so a word left there leads the next read unless it is read off.
 */
#define RXD_CH 9u

static struct {
	uint8_t *base;
	bool on, error, running, stall, no_move;
	uint32_t addr, count, done, stat, extra;
	unsigned starts, bad, core_reads;
	uint32_t first_addr[4];
} rxd;

static bool rxd_hook(void *ctx, enum fake_mmr_op op, unsigned width, wb_mmr_addr addr,
		     uint32_t *value)
{
	if (op == FAKE_MMR_WRITE && addr == DMA_CFG(RXD_CH)) {
		rxd.on = (*value & DMA_CFG_EN) != 0u;
		if (rxd.on) {
			if ((*value & DMA_CFG_WNR) == 0u ||
			    (*value & (7u << DMA_CFG_PSIZE_SHIFT)) != 0u ||
			    (*value & DMA_CFG_MSIZE_MASK) != 0u || (*value & DMA_CFG_INT_X) == 0u ||
			    (dev.rxctl & SPI_XCTL_EN) != 0u)
				rxd.bad++;
			rxd.addr = fake_mmr_get(DMA_ADDRSTART(RXD_CH));
			rxd.count = fake_mmr_get(DMA_XCNT(RXD_CH));
			if (rxd.starts < ARRAY_LEN(rxd.first_addr))
				rxd.first_addr[rxd.starts] = rxd.addr;
			rxd.starts++;
			rxd.done = 0;
			rxd.stat = 0;
		}
		return true;
	}
	if (op == FAKE_MMR_WRITE && addr == DMA_STAT(RXD_CH)) {
		rxd.stat &= ~(*value & (DMA_STAT_IRQDONE | DMA_STAT_IRQERR | DMA_STAT_PIRQ));
		return false;
	}
	if (op == FAKE_MMR_READ && addr == DMA_STAT(RXD_CH)) {
		if (rxd.running) {
			*value = 1u << 8;
			return false;
		}
		if (rxd.on && rxd.error) {
			rxd.stat |= DMA_STAT_IRQERR;
		} else if (rxd.on && rxd.no_move && (dev.rxctl & SPI_XCTL_EN)) {
			/* the FIFO full, no word taken, and done */
			for (; dev.qt - dev.qh < 8u && dev.rx_due > 0u; dev.rx_due--)
				dev.q[dev.qt++ % ARRAY_LEN(dev.q)] = dev.reply(&dev, 0);
			rxd.stat |= DMA_STAT_IRQDONE;
		} else if (rxd.on && !rxd.stall && (dev.rxctl & SPI_XCTL_EN)) {
			if ((dev.rxctl & SPI_XCTL_DR_NE) == 0u)
				rxd.bad++;
			while (rxd.done < rxd.count) {
				uint32_t st = 0, w = 0;

				(void)dev_hook(&dev, FAKE_MMR_READ, 32u, SPI_STAT(2), &st);
				if (st & SPI_STAT_RFE)
					break;
				(void)dev_hook(&dev, FAKE_MMR_READ, 32u, SPI_RFIFO(2), &w);
				rxd.base[rxd.addr - (uint32_t)(uintptr_t)rxd.base + rxd.done] = (uint8_t)w;
				rxd.done++;
			}
			if (rxd.done == rxd.count && !(rxd.stat & DMA_STAT_IRQDONE)) {
				for (; rxd.extra > 0u; rxd.extra--)
					dev.q[dev.qt++ % ARRAY_LEN(dev.q)] = 0xEEu;
				rxd.stat |= DMA_STAT_IRQDONE;
			}
		}
		*value = rxd.stat;
		return false;
	}
	if (op == FAKE_MMR_READ && addr == DMA_XCNT_CUR(RXD_CH)) {
		*value = rxd.no_move ? 0u : rxd.count - rxd.done;
		return false;
	}
	if (op == FAKE_MMR_READ && addr == DMA_ADDR_CUR(RXD_CH)) {
		*value = rxd.no_move ? 0u : rxd.addr + rxd.done;
		return false;
	}
	if (op == FAKE_MMR_READ && addr == SPI_RWC(2) && rxd.starts != 0u) {
		*value = dev.rx_due;
		return false;
	}
	if (op == FAKE_MMR_READ && addr == SPI_RFIFO(2) && rxd.on)
		rxd.core_reads++;
	return dev_hook(ctx, op, width, addr, value);
}

static void rxd_setup(struct wb_spi *s, struct wb_spiflash *f, uint8_t *base)
{
	flash_setup(s, f);
	memset(&rxd, 0, sizeof(rxd));
	rxd.base = base;
	fake_mmr_set_hook(rxd_hook, &dev);
}

/*
 * S-13b.011, S-13b.080: the slot's stream read at 50 MHz with each frame's
 * data moved by DMA9: the same frames and bytes as the core's read, no word
 * taken from the receive FIFO by the core, the channel started before the
 * receive channel for each frame at the frame's place, stopped after, and
 * the bus at its usual rate again. A device that stops, and a DMA error,
 * fail within a word's bound with the channel stopped and the bus put back,
 * for S-02's read again by the core. A channel left enabled and idle is
 * stopped and used; one still running is refused.
 */
static void test_flash_read_dma(void)
{
	static uint8_t buf[0x12345u];
	const uint32_t at = 0x10000u, n = sizeof(buf);
	struct wb_spi s;
	struct wb_spiflash f;
	uint8_t b[4];
	uint32_t i;

	rxd_setup(&s, &f, buf);
	dev.slowest = 1u;
	for (i = 0; i < FLASH_SIZE; i++)
		dev.mem[i] = (uint8_t)(i * 29u + (i >> 8) * 5u + (i >> 16));
	memset(buf, 0, sizeof(buf));
	CHECK(wb_spiflash_read_dma(&f, 1u, at, buf, n) == WB_SPIFLASH_OK);
	CHECK(memcmp(buf, dev.mem + at, n) == 0);
	CHECK(dev.nops == 3u && dev.nreads == 3u && dev.fast_frames == 3u);
	CHECK(dev.read_addr[0] == at && dev.read_addr[1] == at + WB_SPIFLASH_FAST_FRAME &&
	      dev.read_addr[2] == at + 2u * WB_SPIFLASH_FAST_FRAME);
	CHECK(rxd.starts == 3u && rxd.bad == 0u && rxd.core_reads == 0u);
	for (i = 0; i < 3u; i++)
		CHECK(rxd.first_addr[i] == (uint32_t)(uintptr_t)(buf + i * WB_SPIFLASH_FAST_FRAME));
	CHECK(!wb_dma_enabled(RXD_CH) && !rxd.on);
	CHECK(dev.violations == 0 && dev.too_fast == 0 && !dev.selected);
	CHECK(dev.tfifo == 3u * 4u);
	CHECK(fake_mmr_get(SPI_CLK(2)) == 3u);
	CHECK(fake_mmr_get(SPI_CTL(2)) == (flash_cfg.ctl | SPI_CTL_EN));
	CHECK(dev.rxctl == 0 && dev.txctl == 0);
	CHECK(wb_spiflash_read(&f, at + 5u, b, 4) == WB_SPIFLASH_OK);
	CHECK(memcmp(b, dev.mem + at + 5u, 4) == 0);

	/* a frame's worth, less and one more, from an odd place */
	for (i = 0; i < 3u; i++) {
		const uint32_t len = WB_SPIFLASH_FAST_FRAME - 1u + i;

		rxd_setup(&s, &f, buf);
		for (uint32_t k = 0; k < len; k++)
			dev.mem[0x20003u + k] = (uint8_t)(k * 7u + i);
		CHECK(wb_spiflash_read_dma(&f, 1u, 0x20003u, buf + 1, len) == WB_SPIFLASH_OK);
		CHECK(memcmp(buf + 1, dev.mem + 0x20003u, len) == 0);
		CHECK(dev.nreads == (i == 2u ? 2u : 1u) && rxd.bad == 0u && rxd.core_reads == 0u);
	}

	/* the channel stops moving, or reports an error: a failure soon */
	for (i = 0; i < 2u; i++) {
		rxd_setup(&s, &f, buf);
		if (i == 0u)
			rxd.stall = true;
		else
			rxd.error = true;
		fake_core_set_cycle_step(1);
		fake_core_set_cycles(0);
		CHECK(wb_spiflash_read_dma(&f, 1u, at, buf, n) == WB_SPIFLASH_ERR_IO);
		CHECK(fake_core_peek_cycles() <= 400u * 400u);
		CHECK(rxd.starts == 1u && !wb_dma_enabled(RXD_CH) && !rxd.on);
		CHECK(fake_mmr_get(SPI_CLK(2)) == 3u);
		CHECK(fake_mmr_get(SPI_CTL(2)) == (flash_cfg.ctl | SPI_CTL_EN));
		CHECK(!dev.selected && dev.rxctl == 0 && dev.txctl == 0);
		rxd.stall = false;
		rxd.error = false;
		CHECK(wb_spiflash_read(&f, at, b, 4) == WB_SPIFLASH_OK);
		CHECK(memcmp(b, dev.mem + at, 4) == 0);
	}

	/* a channel left enabled: idle, stopped and used; running, refused */
	rxd_setup(&s, &f, buf);
	fake_mmr_set(DMA_CFG(RXD_CH), DMA_CFG_EN);
	CHECK(wb_spiflash_read_dma(&f, 1u, at, buf, 64u) == WB_SPIFLASH_OK);
	CHECK(rxd.starts == 1u && memcmp(buf, dev.mem + at, 64u) == 0);
	rxd_setup(&s, &f, buf);
	fake_mmr_set(DMA_CFG(RXD_CH), DMA_CFG_EN);
	rxd.running = true;
	CHECK(wb_spiflash_read_dma(&f, 1u, at, buf, 64u) == WB_SPIFLASH_ERR_IO);
	CHECK(rxd.starts == 0u && !dev.selected);
	CHECK(fake_mmr_get(SPI_CLK(2)) == 3u);
}

/*
 * S-13b.080: a done counts only with the words moved. A channel that shows
 * done with nothing moved (its address and SPI_RWC unchanged) while the
 * device fills the receive FIFO, and one that leaves words in the FIFO,
 * fail the read with their stages; the FIFO is read empty after it, so the
 * next read, as S-02's core-fed read again, gets the flash's bytes from its
 * first. A read that moves every word reads nothing off.
 */
static void test_flash_read_dma_checks(void)
{
	static uint8_t buf[0x9000u];
	const uint32_t at = 0x10000u, n = sizeof(buf);
	struct wb_spi s;
	struct wb_spiflash f;
	struct wb_spi_fault fault;
	uint8_t b[8];
	uint32_t i;

	for (unsigned c = 0; c < 4u; c++) {
		rxd_setup(&s, &f, buf);
		for (i = 0; i < FLASH_SIZE; i++)
			dev.mem[i] = (uint8_t)(i * 13u + (i >> 8) * 3u + 1u);
		memset(buf, 0, sizeof(buf));
		if (c == 1u)
			rxd.no_move = true;
		else if (c == 2u)
			rxd.extra = 8u;
		else if (c == 3u)
			rxd.extra = 200u;	/* more than the drain's bound */
		if (c == 0u) {
			CHECK(wb_spiflash_read_dma(&f, 1u, at, buf, n) == WB_SPIFLASH_OK);
			CHECK(memcmp(buf, dev.mem + at, n) == 0);
			CHECK(wb_spi_recv_dma_drained(&s) == 0u && rxd.core_reads == 0u);
		} else {
			CHECK(wb_spiflash_read_dma(&f, 1u, at, buf, n) == WB_SPIFLASH_ERR_IO);
			wb_spi_last_fault(&s, &fault);
			CHECK(fault.rc == WB_SPI_ERR_HW);
			CHECK(fault.stage == (c == 1u ? WB_SPI_STAGE_DMA_UNMOVED : WB_SPI_STAGE_DMA_LEFT));
			CHECK(rxd.starts == 1u && !wb_dma_enabled(RXD_CH) && !rxd.on);
			CHECK(wb_spi_recv_dma_drained(&s) == (c == 3u ? 64u : 8u));
			if (c == 3u)	/* the controller's disable is left to clear the rest */
				dev.qh = dev.qt;
			CHECK(dev.qh == dev.qt);
		}
		CHECK(fake_mmr_get(SPI_CLK(2)) == 3u);
		CHECK(!dev.selected && dev.rxctl == 0 && dev.txctl == 0);
		CHECK(wb_spiflash_read(&f, at, b, sizeof(b)) == WB_SPIFLASH_OK);
		CHECK(memcmp(b, dev.mem + at, sizeof(b)) == 0);
	}
}

/*
 * S-02.016 over the DMA read: a slot whose read by DMA fails, done with
 * nothing moved or with words left in the receive FIFO, is read again by
 * the core from the flash's first byte and found good; one read whole by
 * DMA is good from the DMA's bytes.
 */
/* CRC-32, zlib's convention, bit by bit. */
static uint32_t crc32_bits(uint32_t crc, const uint8_t *p, uint32_t n)
{
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (unsigned k = 0; k < 8u; k++)
			crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

static int read_dma_50(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	return wb_spiflash_read_dma(ctx, 1u, off, buf, len);
}

static void test_flash_read_dma_fallback(void)
{
	static uint8_t buf[0x9000u];
	static uint8_t scratch[512];
	const uint32_t len = 0x8C21u;
	struct wb_spi s;
	struct wb_spiflash f;
	struct s02_flash_ops ops;
	struct s02_env env;
	struct s02_image im;
	struct s02_record rec;
	uint8_t raw[S02_RECORD_SIZE];
	uint32_t i;

	for (unsigned c = 0; c < 3u; c++) {
		rxd_setup(&s, &f, buf);
		for (i = 0; i < len; i++)
			dev.mem[S02_SLOT0_OFF + i] = (uint8_t)(i * 7u + (i >> 9));
		rec.crc32 = crc32_bits(0, dev.mem + S02_SLOT0_OFF, len);
		rec.length = len;
		rec.tag = S02_RECORD_TAG;
		s02_record_encode(&rec, raw);
		memcpy(dev.mem + S02_RECORD0_OFF, raw, sizeof(raw));
		rxd.no_move = c == 1u;
		rxd.extra = c == 2u ? 8u : 0u;
		ops = (struct s02_flash_ops){
			&f, wb_spiflash_read, wb_spiflash_erase, wb_spiflash_program,
		};
		env = (struct s02_env){ &ops, crc32_bits, scratch, sizeof(scratch), NULL };
		memset(buf, 0, sizeof(buf));
		s02_image_init(&im, buf, sizeof(buf), read_dma_50, &f);
		CHECK(s02_image_read(&env, 0u, &im) == S02_OK);
		CHECK(im.by_fast == (c == 0u) && im.len == len);
		CHECK(s02_image_check(&env, &im) == S02_OK);
		CHECK(im.good && memcmp(buf, dev.mem + S02_SLOT0_OFF, len) == 0);
		CHECK(rxd.starts == (c == 0u ? 2u : 1u) && dev.qh == dev.qt);
	}
}

/* The s02_flash_ops adapter is an initializer. */
static void test_s02_adapter(void)
{
	struct wb_spi s;
	struct wb_spiflash f;
	struct s02_flash_ops ops;
	uint8_t b = 0;

	flash_setup(&s, &f);
	ops = (struct s02_flash_ops){
		&f, wb_spiflash_read, wb_spiflash_erase, wb_spiflash_program,
	};
	dev.mem[5] = 0x5A;
	CHECK(ops.read(ops.ctx, 5, &b, 1) == 0);
	CHECK(b == 0x5A);
	CHECK(ops.erase(ops.ctx, 0) == 0);
	CHECK(ops.program(ops.ctx, 5, &b, 1) == 0);
	CHECK(dev.mem[5] == 0x5A);
}

/* S-13b.040/.041: 32-bit words, memory order, then quad I/O. */
static void test_bridge_words(void)
{
	static const uint8_t tx[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	static const struct wb_spi_config pre = { 0x00002592u, 3, 0, 0 };
	static const struct fake_mmr_access quad[] = {
		FAKE_W32(SPI_CTL(0), 0),
		FAKE_W32(SPI_CLK(0), 1u),
		FAKE_W32(SPI_CTL(0), 0x00242593u),
		FAKE_SYNC,
	};
	struct wb_spi s;
	uint8_t rx[8];

	reset_dev(0, 3, echo_reply);
	CHECK(wb_spi_init(&s, 0, &pre, &cclk400, 500) == WB_SPI_OK);
	CHECK(wb_spi_word_bytes(&s) == 4u);
	CHECK(wb_spi_xfer(&s, tx, rx, 8) == WB_SPI_OK);
	CHECK(rx[0] == 0xFE && rx[3] == 0xFB && rx[7] == 0xF7);
	CHECK(fake_mmr_get(SPI_TFIFO(0)) == 0x08070605u);
	CHECK(wb_spi_xfer(&s, tx, rx, 6) == WB_SPI_ERR_ARG);

	fake_mmr_clear_log();
	CHECK(wb_spi_reconfigure(&s, 0x00242592u, 1) == WB_SPI_OK);
	CHECK(fake_mmr_expect("quad", quad, ARRAY_LEN(quad)));
	/* Full duplex is refused in quad I/O. */
	CHECK(wb_spi_xfer(&s, tx, rx, 8) == WB_SPI_ERR_ARG);
	CHECK(wb_spi_send(&s, tx, 8) == WB_SPI_OK);
	CHECK(dev.rxctl == 0 && dev.txctl == 0);
	CHECK(wb_spi_recv(&s, rx, 8) == WB_SPI_OK);
	CHECK(rx[0] == 0xFF && rx[7] == 0xFF);
	CHECK(dev.violations == 0);
	CHECK(fake_mmr_get(SPI_CTL(0)) == 0x00242593u);
}

static void test_timeouts_and_errors(void)
{
	static const uint8_t tx[4];
	struct wb_spi s;
	uint8_t rx[4];
	static const struct wb_spi_config c = { 0x00002592u, 3, 0, 0 };

	reset_dev(0, 3, echo_reply);
	CHECK(wb_spi_init(&s, 0, &c, &cclk400, 50) == WB_SPI_OK);
	fake_core_set_cycle_step(400);
	dev.mute = true;	/* the ready line held: nothing moves */
	CHECK(wb_spi_xfer(&s, tx, rx, 4) == WB_SPI_TIMEOUT);
	CHECK(dev.rxctl == 0 && dev.txctl == 0);
	CHECK(wb_spi_send(&s, tx, 4) == WB_SPI_TIMEOUT);
	CHECK(wb_spi_recv(&s, rx, 4) == WB_SPI_TIMEOUT);
	/* Each gave up after about one word's time, not more. */
	CHECK(fake_core_peek_cycles() < 3u * 60u * 400u * 2u);

	/* A stale overrun is cleared; one during the transfer fails it. */
	dev.mute = false;
	dev.stat = SPI_STAT_ROR;
	CHECK(wb_spi_xfer(&s, tx, rx, 4) == WB_SPI_OK);
	dev.raise_ror = true;
	CHECK(wb_spi_xfer(&s, tx, rx, 4) == WB_SPI_ERR_HW);
	CHECK(dev.stat == 0);
}

/* The words the device was sent, in order. */
static uint32_t sent[0x10000u / 4u + 16u];
static size_t nsent;

static uint32_t take_reply(struct dev *d, uint32_t tx)
{
	(void)d;
	if (nsent < ARRAY_LEN(sent))
		sent[nsent] = tx;
	nsent++;
	return 0;
}

static bool sent_is(const uint8_t *buf, size_t n)
{
	size_t i;

	if (nsent != n / 4u)
		return false;
	for (i = 0; i < n; i++)
		if ((uint8_t)(sent[i / 4u] >> (8u * (i % 4u))) != buf[i])
			return false;
	return true;
}

/* S-13b.040's controller before the link, and two rungs' control values. */
static const struct wb_spi_config bridge_pre = { 0x00002592u, 3, 0, 0 };
#define PRE_EN      0x00002593u

/*
 * The device of dev_hook with a transmit FIFO that reads full for the next
 * `full` status reads (`burst` more every `every` reads), TF held back for
 * `tf_late` reads once it is due, SPI_TWC read as the words still to go,
 * and DMA_STAT's error cause `errc`.
 */
static struct {
	unsigned full, every, burst, tf_late, stat_reads, waits;
	bool in_full;
} room;

static unsigned idles;

static void count_idle(void)
{
	idles++;
}

static bool room_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		      wb_mmr_addr addr, uint32_t *value)
{
	const bool r = dev_hook(ctx, op, width, addr, value);

	if (op == FAKE_MMR_READ && addr == SPI_TWC(dev.n))
		*value = dev.twc;
	if (op != FAKE_MMR_READ || addr != SPI_STAT(dev.n))
		return r;
	room.stat_reads++;
	if (room.every != 0u && room.full == 0u &&
	    room.stat_reads % room.every == 0u)
		room.full = room.burst;
	if (room.full != 0u) {
		room.full--;
		if (!room.in_full)
			room.waits++;
		room.in_full = true;
		*value = (*value & ~SPI_STAT_TFS_MASK) | SPI_STAT_TFF;
	} else {
		room.in_full = false;
	}
	if ((*value & SPI_STAT_TF) && room.tf_late != 0u) {
		room.tf_late--;
		*value &= ~SPI_STAT_TF;
	}
	return r;
}

static void room_up(struct wb_spi *s, uint32_t word_us)
{
	reset_dev(0, 3, take_reply);
	fake_mmr_set_hook(room_hook, &dev);
	memset(&room, 0, sizeof(room));
	idles = 0;
	nsent = 0;
	CHECK(wb_spi_init(s, 0, &bridge_pre, &cclk400, word_us) == WB_SPI_OK);
	fake_core_set_cycle_step(400);		/* 1 us a clock read */
	fake_mmr_clear_log();
}

static bool fault_is(const struct wb_spi *s, int32_t rc, uint32_t stage,
		     uint32_t left)
{
	struct wb_spi_fault f;

	wb_spi_last_fault(s, &f);
	if (f.rc == rc && f.stage == stage && f.left == left)
		return true;
	printf("  fault: rc %ld stage 0x%lx left %lu stat 0x%08lx\n",
	       (long)f.rc, (unsigned long)f.stage, (unsigned long)f.left,
	       (unsigned long)f.stat);
	return false;
}

static unsigned log_reads(wb_mmr_addr addr)
{
	const struct fake_mmr_access *l = fake_mmr_log();
	unsigned n = 0;
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (l[i].op == FAKE_MMR_READ && l[i].addr == addr)
			n++;
	return n;
}

/*
 * The word loops look at SPI_STAT first: with room, a word is written with
 * no clock read and no wait, two to a status read when the FIFO is empty;
 * only TF's wait reads the clock. A FIFO found full waits within a word's
 * bound from the moment it was found full, as before.
 */
static void test_fast_path(void)
{
	static uint32_t buf[256];
	static uint8_t bytes[256 * 4 + 1];
	const uint8_t *b = (const uint8_t *)buf;
	struct wb_spi s;
	uint64_t t0;
	uint32_t rx[64];
	size_t i;

	for (i = 0; i < ARRAY_LEN(buf); i++)
		buf[i] = 0x9E3779B9u * (uint32_t)(i + 1u);
	for (i = 0; i < sizeof(bytes); i++)
		bytes[i] = (uint8_t)(i * 29u + 7u);

	room_up(&s, 50u);
	fake_core_set_cycle_step(1);
	t0 = fake_core_peek_cycles();
	CHECK(wb_spi_send(&s, b, sizeof(buf)) == WB_SPI_OK);
	CHECK(fake_core_peek_cycles() - t0 <= 2u);	/* TF's wait alone */
	CHECK(sent_is(b, sizeof(buf)));
	CHECK(log_reads(SPI_STAT(0)) <= ARRAY_LEN(buf) / 2u + 2u);
	CHECK(fault_is(&s, 0, 0u, 0u));

	/* not aligned: a word at a time, the same bytes, still no wait */
	room_up(&s, 50u);
	fake_core_set_cycle_step(1);
	t0 = fake_core_peek_cycles();
	CHECK(wb_spi_send(&s, bytes + 1, 64u) == WB_SPI_OK);
	CHECK(fake_core_peek_cycles() - t0 <= 2u);
	CHECK(sent_is(bytes + 1, 64u));

	/* a receive: no clock read at all while words are there */
	room_up(&s, 50u);
	fake_core_set_cycle_step(1);
	t0 = fake_core_peek_cycles();
	CHECK(wb_spi_recv(&s, (uint8_t *)rx, sizeof(rx)) == WB_SPI_OK);
	CHECK(fake_core_peek_cycles() == t0);

	/* full for 30 status reads (30 us): waited for, then whole */
	room_up(&s, 50u);
	room.full = 30u;
	CHECK(wb_spi_send(&s, b, 64u) == WB_SPI_OK);
	CHECK(sent_is(b, 64u));
	CHECK(room.waits == 1u);

	/* full for good: given up after the word's bound (50 us), not more */
	room_up(&s, 50u);
	room.full = UINT32_MAX;
	fake_core_set_cycles(0);
	CHECK(wb_spi_send(&s, b, 64u) == WB_SPI_TIMEOUT);
	CHECK(fake_core_peek_cycles() >= 50u * 400u);
	CHECK(fake_core_peek_cycles() <= 53u * 400u);
	CHECK(nsent == 0u);
	CHECK(fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_TX_ROOM, 16u));

	/* full after 10 words: the bound runs from there */
	room_up(&s, 50u);
	room.every = 6u;
	room.burst = UINT32_MAX;
	fake_core_set_cycles(0);
	CHECK(wb_spi_send(&s, b, 64u) == WB_SPI_TIMEOUT);
	CHECK(nsent == 10u);
	CHECK(fake_core_peek_cycles() <= 53u * 400u);
	CHECK(fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_TX_ROOM, 6u));

	/* a receive whose words stop: the word's bound */
	room_up(&s, 50u);
	dev.mute = true;
	fake_core_set_cycles(0);
	CHECK(wb_spi_recv(&s, (uint8_t *)rx, sizeof(rx)) == WB_SPI_TIMEOUT);
	CHECK(fake_core_peek_cycles() <= 53u * 400u);
	CHECK(fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_RX_WORD, 64u));
}

/* S-13b.020-.022: the boot link's bytes, one in flight at a time. A byte is
 * the transmit FIFO, the status and the receive FIFO, and no clock read. */
static void test_words(void)
{
	static const struct wb_spi_config link = {
		SPI_CTL_MSTR | SPI_CTL_CPHA | SPI_CTL_SIZE8, 2u, 0u, 3u,
	};
	static const struct fake_mmr_access open[] = {
		FAKE_W32(SPI_STAT(0), SPI_STAT_W1C),
		FAKE_W32(SPI_RXCTL(0), SPI_XCTL_EN),
		FAKE_W32(SPI_TXCTL(0), SPI_XCTL_EN | SPI_XCTL_TI),
	};
	static const struct fake_mmr_access byte[] = {
		FAKE_W32(SPI_TFIFO(0), 0x5Au),
		FAKE_R32(SPI_STAT(0), 4u << 16),
		FAKE_R32(SPI_RFIFO(0), 0xFFFFFFA5u),
	};
	static const struct fake_mmr_access shut[] = {
		FAKE_R32(SPI_STAT(0), (4u << 16) | SPI_STAT_RFE),
		FAKE_W32(SPI_RXCTL(0), 0),
		FAKE_W32(SPI_TXCTL(0), 0),
		FAKE_W32(SPI_STAT(0), 0),
		FAKE_SYNC,
	};
	struct wb_spi s;
	uint64_t t0;
	unsigned i;

	reset_dev(0, 3, echo_reply);
	CHECK(wb_spi_init(&s, 0, &link, &cclk400, 100) == WB_SPI_OK);
	fake_core_set_cycle_step(1);
	t0 = fake_core_peek_cycles();
	fake_mmr_clear_log();
	CHECK(wb_spi_words_begin(&s) == WB_SPI_OK);
	CHECK(fake_mmr_expect("words begin", open, ARRAY_LEN(open)));
	fake_mmr_clear_log();
	CHECK((uint8_t)wb_spi_word(&s, 0x5Au) == 0xA5u);
	CHECK(fake_mmr_expect("word", byte, ARRAY_LEN(byte)));
	for (i = 0; i < 1000u; i++)
		CHECK((uint8_t)wb_spi_word(&s, i & 0xFFu) == (uint8_t)~i);
	fake_mmr_clear_log();
	CHECK(wb_spi_words_end(&s) == WB_SPI_OK);
	CHECK(fake_mmr_expect("words end", shut, ARRAY_LEN(shut)));
	CHECK(fake_core_peek_cycles() == t0);	/* the clock never read */
	CHECK(dev.violations == 0);
	CHECK(fault_is(&s, 0, 0u, 0u));

	/* a byte that never comes in: all ones after the word's bound, the
	 * run's end says so, and the channels are closed */
	CHECK(wb_spi_words_begin(&s) == WB_SPI_OK);
	dev.mute = true;
	fake_core_set_cycles(0);
	CHECK(wb_spi_word(&s, 0x33u) == UINT32_MAX);
	CHECK(fake_core_peek_cycles() >= 100u * 400u);
	CHECK(fake_core_peek_cycles() <= 103u * 400u);
	dev.mute = false;
	CHECK((uint8_t)wb_spi_word(&s, 0x0Fu) == 0xF0u);
	CHECK(wb_spi_words_end(&s) == WB_SPI_TIMEOUT);
	CHECK(fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_RX_WORD, 1u));
	CHECK(dev.rxctl == 0 && dev.txctl == 0);
	/* the next run starts clean */
	CHECK(wb_spi_words_begin(&s) == WB_SPI_OK);
	CHECK((uint8_t)wb_spi_word(&s, 0x01u) == 0xFEu);
	CHECK(wb_spi_words_end(&s) == WB_SPI_OK);

	/* refused in quad I/O, as wb_spi_xfer() is */
	CHECK(wb_spi_reconfigure(&s, 0x00242592u, 1) == WB_SPI_OK);
	CHECK(wb_spi_words_begin(&s) == WB_SPI_ERR_ARG);
}

/* The plain mode's register writes, word for word as they were. */
static void test_plain_writes(void)
{
	static const uint8_t tx[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
	static const struct fake_mmr_access send[] = {
		FAKE_W32(SPI_STAT(0), SPI_STAT_W1C),
		FAKE_W32(SPI_TWC(0), 3u),
		FAKE_W32(SPI_TXCTL(0), 0x0Du),
		FAKE_W32(SPI_TFIFO(0), 0x04030201u),
		FAKE_W32(SPI_TFIFO(0), 0x08070605u),
		FAKE_W32(SPI_TFIFO(0), 0x0C0B0A09u),
		FAKE_W32(SPI_RXCTL(0), 0u),
		FAKE_W32(SPI_TXCTL(0), 0u),
		FAKE_W32(SPI_STAT(0), SPI_STAT_TF),
	};
	static const struct fake_mmr_access recv[] = {
		FAKE_W32(SPI_STAT(0), SPI_STAT_W1C),
		FAKE_W32(SPI_RWC(0), 3u),
		FAKE_W32(SPI_RXCTL(0), 0x0Du),
		FAKE_W32(SPI_RXCTL(0), 0u),
		FAKE_W32(SPI_TXCTL(0), 0u),
		FAKE_W32(SPI_STAT(0), SPI_STAT_RF),
	};
	_Alignas(4) uint8_t rx[12];
	struct wb_spi s;

	reset_dev(0, 3, echo_reply);
	CHECK(wb_spi_init(&s, 0, &bridge_pre, &cclk400, 50u) == WB_SPI_OK);
	fake_mmr_clear_log();
	CHECK(wb_spi_send(&s, tx, sizeof(tx)) == WB_SPI_OK);
	CHECK(fake_mmr_expect_writes("plain send", send, ARRAY_LEN(send)));
	dev.stat = 0u;
	fake_mmr_clear_log();
	CHECK(wb_spi_recv(&s, rx, sizeof(rx)) == WB_SPI_OK);
	CHECK(fake_mmr_expect_writes("plain recv", recv, ARRAY_LEN(recv)));
	CHECK(rx[0] == 0xFF && rx[11] == 0xFF);
}

/* The fault record at each failure exit, and when it is cleared. */
static void test_fault_record(void)
{
	static uint32_t buf[16];
	const uint8_t *b = (const uint8_t *)buf;
	struct wb_spi_fault f;
	struct wb_spi s;
	uint8_t rx[4];

	room_up(&s, 50u);
	CHECK(fault_is(&s, 0, 0u, 0u));
	CHECK(wb_spi_send(&s, b, 6u) == WB_SPI_ERR_ARG);
	CHECK(fault_is(&s, WB_SPI_ERR_ARG, WB_SPI_STAGE_ARG, 0u));
	CHECK(wb_spi_recv(&s, rx, 4u * 0x10000u) == WB_SPI_ERR_ARG);	/* RWC is 16 bits */
	CHECK(wb_spi_dma_send(&s, NULL, b, 4u * 0x10000u) == WB_SPI_ERR_ARG);
	/* a later operation of the same session that passes leaves it */
	CHECK(wb_spi_send(&s, b, sizeof(buf)) == WB_SPI_OK);
	CHECK(fault_is(&s, WB_SPI_ERR_ARG, WB_SPI_STAGE_ARG, 0u));
	/* the next session's first operation clears it */
	CHECK(wb_spi_clock(&s, s.clk, false) == WB_SPI_OK);
	CHECK(fault_is(&s, WB_SPI_ERR_ARG, WB_SPI_STAGE_ARG, 0u));
	CHECK(wb_spi_send(&s, b, sizeof(buf)) == WB_SPI_OK);
	CHECK(fault_is(&s, 0, 0u, 0u));

	/* words taken, TF never: the transmit's end */
	room_up(&s, 50u);
	room.tf_late = UINT32_MAX;
	CHECK(wb_spi_send(&s, b, sizeof(buf)) == WB_SPI_TIMEOUT);
	CHECK(fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_TX_END, 0u));
	wb_spi_last_fault(&s, &f);
	CHECK(!(f.stat & SPI_STAT_TF));

	/* an overrun found at the end */
	room_up(&s, 50u);
	dev.raise_ror = true;
	CHECK(wb_spi_xfer(&s, b, NULL, 4u) == WB_SPI_ERR_HW);
	CHECK(fault_is(&s, WB_SPI_ERR_HW, WB_SPI_STAGE_OVERRUN, 0u));
	wb_spi_last_fault(&s, &f);
	CHECK(f.stat & SPI_STAT_ROR);

}

/*
 * The idle hook: once a millisecond in a wait that lasts, never in a word
 * that moves, never in short waits however many.
 */
static void test_idle_hook(void)
{
	static uint32_t buf[1024];
	const uint8_t *b = (const uint8_t *)buf;
	struct wb_spi s;
	uint8_t rx[4];

	/* TF 2.5 ms after the last word, within the bound */
	room_up(&s, 5000u);
	wb_spi_set_idle(&s, count_idle);
	room.tf_late = 2500u;
	CHECK(wb_spi_send(&s, b, 64u) == WB_SPI_OK);
	CHECK(idles == 2u);

	/* a receive whose word never comes */
	room_up(&s, 5000u);
	wb_spi_set_idle(&s, count_idle);
	dev.mute = true;
	CHECK(wb_spi_recv(&s, rx, 4u) == WB_SPI_TIMEOUT);
	CHECK(idles >= 4u && idles <= 5u);

	/* a long send whose FIFO is often full for 20 us: hundreds of waits,
	 * many milliseconds in all, no idle */
	room_up(&s, 5000u);
	wb_spi_set_idle(&s, count_idle);
	room.every = 3u;
	room.burst = 20u;
	fake_core_set_cycles(0);
	CHECK(wb_spi_send(&s, b, sizeof(buf)) == WB_SPI_OK);
	CHECK(sent_is(b, sizeof(buf)));
	CHECK(room.waits > 150u);
	CHECK(fake_core_peek_cycles() > 2000u * 400u);
	CHECK(idles == 0u);

	/* a send with room: no idle, however long */
	room_up(&s, 5000u);
	wb_spi_set_idle(&s, count_idle);
	CHECK(wb_spi_send(&s, b, sizeof(buf)) == WB_SPI_OK);
	CHECK(idles == 0u);
}

/* ---- the bridge's sessions by DMA (S-13b.083) ---- */

/* The checkers in C agree with check.py on every row of its
 * vectors, positive and negative, before they judge the driver. */
static void test_s13b_vectors(void)
{
	size_t i;

	for (i = 0; i < ARRAY_LEN(s13bv_dma); i++) {
		const struct s13bv_dma *r = &s13bv_dma[i];
		const unsigned got = s13b_bridge_dma(r->kind, r->trace, r->n, r->words,
						     r->parts, r->nparts);

		if (got != r->expect)
			printf("  bridge_dma %s: 0x%x, not 0x%x\n", r->name, got, r->expect);
		CHECK(got == r->expect);
	}
	for (i = 0; i < ARRAY_LEN(s13bv_ses); i++) {
		const struct s13bv_ses *r = &s13bv_ses[i];
		const unsigned got = s13b_master_sessions(r->trace, r->n);

		if (got != r->expect)
			printf("  bridge_sessions %s: 0x%x, not 0x%x\n", r->name, got, r->expect);
		CHECK(got == r->expect);
	}
	for (i = 0; i < ARRAY_LEN(s13bv_setup); i++) {
		const struct s13bv_setup *r = &s13bv_setup[i];
		const unsigned got = s13b_reply_setup(&r->s);

		if (got != r->expect)
			printf("  reply_setup %s: 0x%x, not 0x%x\n", r->name, got, r->expect);
		CHECK(got == r->expect);
	}
	for (i = 0; i < ARRAY_LEN(s13bv_tx); i++) {
		const struct s13bv_tx *r = &s13bv_tx[i];
		const unsigned got = s13b_slave_tx(S13BV_TX_WORDS, r->trace, r->n, r->quad, false);

		if (got != r->expect)
			printf("  slave_tx %s: 0x%x, not 0x%x\n", r->name, got, r->expect);
		CHECK(got == r->expect);
	}
	CHECK(ARRAY_LEN(s13bv_dma) >= 50u && ARRAY_LEN(s13bv_ses) >= 15u &&
	      ARRAY_LEN(s13bv_setup) >= 20u && ARRAY_LEN(s13bv_tx) >= 30u);
}

static const struct s13bv_setup *setup_row(const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_LEN(s13bv_setup); i++)
		if (strcmp(s13bv_setup[i].name, name) == 0)
			return &s13bv_setup[i];
	CHECK(0);
	return &s13bv_setup[0];
}

static const struct s13bv_tx *tx_row(const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_LEN(s13bv_tx); i++)
		if (strcmp(s13bv_tx[i].name, name) == 0)
			return &s13bv_tx[i];
	CHECK(0);
	return &s13bv_tx[0];
}

/* S-13b.083's TF end in the two checkers, as test_s13b.py takes it:
 * REN clear and the quad order in a single-I/O set-up, TF as the finish. */
static void test_s13b_factory_end(void)
{
	struct s13b_reply_setup f = setup_row("dma_tf_order")->s;
	const struct s13b_reply_setup early_row = setup_row("quad_tf_before_dma_done")->s;
	struct s13b_reply_setup early = early_row;
	const struct s13bv_tx *tf = tx_row("tf_after_dma"), *tf_early = tx_row("quad_tf_early");

	f.link = S13B_LINK_POST;
	f.ctl = setup_row("single_enable_last")->s.ctl;
	f.rxctl = 0x10014u;
	f.tf_end = true;
	CHECK(s13b_reply_setup(&f) == 0u);
	f.rxctl = 1u;
	CHECK(s13b_reply_setup(&f) == S13B_REPLY_RX_ON);
	f.rxctl = 0x10014u;
	f.tf_end = false;
	CHECK(s13b_reply_setup(&f) == S13B_REPLY_RX_OFF);
	early.link = S13B_LINK_POST;
	early.ctl = f.ctl;
	early.rxctl = 0x10014u;
	early.tf_end = true;
	CHECK(s13b_reply_setup(&early) == S13B_REPLY_TF_UNMASK);

	CHECK(s13b_slave_tx(S13BV_TX_WORDS, tf->trace, tf->n, false, true) == 0u);
	CHECK(s13b_slave_tx(S13BV_TX_WORDS, tf->trace, tf->n, false, false) == S13B_TX_END_NO_COUNT);
	CHECK(s13b_slave_tx(S13BV_TX_WORDS, tf_early->trace, tf_early->n, false, true) ==
	      tf_early->expect);
}

/*
 * DMA4 and DMA5 on the master, with dev_hook's SPI0 as their controller. A
 * channel given its array (wb_spi_tx_array, wb_spi_rx_array) and enabled
 * moves words at each of its status reads, after `stall` reads that move
 * nothing, or errs: DMA4's go to the controller as the core's would (the
 * device's reply, SPI_TWC, TF once SPI_TWC reaches 0), down to `hold` words
 * not clocked; DMA5's come from the receive FIFO dev_hook fills, with the
 * modify of 0 of a dropped part keeping one place. The done status is set
 * once the array is through. SPI_ILAT.TF latches TF while SPI_IMSK.TF is
 * set. Every access is also kept as check_bridge_dma's trace (tr), and as
 * check_master_sessions's events (ses), each word moved one.
 */
static struct {
	bool on[2];
	unsigned at[2];		/* the descriptor under way */
	uint32_t off[2];	/* its bytes moved */
	uint32_t stat[2];
	unsigned stall, hold;
	bool error, no_tf, ror;
	uint32_t errc;
	uint32_t imsk, ilat;
	uint32_t fifo[2];	/* the controller's transmit FIFO */
	unsigned nfifo;
} bm;
static struct s13b_trace tr;
static struct s13b_sev ses[8192];
static size_t nses;

static void ses_push(enum s13b_sev_kind ev, bool on, int miom)
{
	if (nses < ARRAY_LEN(ses))
		ses[nses++] = (struct s13b_sev){ ev, on, miom };
}

/* The transmit FIFO's words clocked, down to `hold` not clocked. */
static void bm_clock(void)
{
	while (bm.nfifo != 0u && dev.twc > bm.hold) {
		uint32_t w = bm.fifo[0];

		bm.fifo[0] = bm.fifo[1];
		bm.nfifo--;
		(void)dev_hook(&dev, FAKE_MMR_WRITE, 32u, SPI_TFIFO(0), &w);
		if (bm.no_tf)
			dev.stat &= ~SPI_STAT_TF;
		ses_push(S13B_EV_WORD, false, -1);
	}
}

static void bm_move(unsigned c)
{
	const struct wb_spi_array *a = c == 0u ? &wb_spi_tx_array : &wb_spi_rx_array;

	if (c == 0u)
		bm_clock();
	while (bm.at[c] < a->n) {
		const unsigned i = bm.at[c];
		uint8_t *p = a->mem[i] + (a->d[i].mod != 0u ? bm.off[c] : 0u);

		if (bm.off[c] == a->bytes[i]) {
			bm.at[c]++;
			bm.off[c] = 0;
			continue;
		}
		if (c == 0u) {
			if (bm.nfifo == 2u)
				return;
			bm.fifo[bm.nfifo++] = (uint32_t)p[0] | (uint32_t)p[1] << 8 |
					      (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
			bm_clock();
		} else {
			uint32_t w;

			if (dev.qh != dev.qt) {
				w = dev.q[dev.qh++ % ARRAY_LEN(dev.q)];
			} else if (dev.rx_due != 0u && !dev.mute) {
				w = dev.reply(&dev, 0u);
				dev.rx_due--;
			} else {
				return;
			}
			p[0] = (uint8_t)w;
			p[1] = (uint8_t)(w >> 8);
			p[2] = (uint8_t)(w >> 16);
			p[3] = (uint8_t)(w >> 24);
			if (bm.ror)
				dev.stat |= SPI_STAT_ROR;
			ses_push(S13B_EV_WORD, false, -1);
		}
		bm.off[c] += 4u;
	}
	bm.stat[c] |= DMA_STAT_IRQDONE;
}

static uint32_t bm_left(unsigned c)
{
	const struct wb_spi_array *a = c == 0u ? &wb_spi_tx_array : &wb_spi_rx_array;
	uint64_t n = 0;
	unsigned i;

	for (i = bm.at[c]; i < a->n; i++)
		n += a->bytes[i];
	return (uint32_t)((n - (bm.at[c] < a->n ? bm.off[c] : 0u)) / 4u);
}

static bool bm_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		    wb_mmr_addr addr, uint32_t *value)
{
	unsigned c;
	bool r = true;

	for (c = 0; c < 2u; c++) {
		const unsigned ch = 4u + c;

		if (op == FAKE_MMR_WRITE && addr == DMA_CFG(ch)) {
			bm.on[c] = (*value & DMA_CFG_EN) != 0u;
			if (bm.on[c]) {
				bm.at[c] = 0;
				bm.off[c] = 0;
			}
		} else if (op == FAKE_MMR_WRITE && addr == DMA_STAT(ch)) {
			bm.stat[c] &= ~*value;
		} else if (op == FAKE_MMR_READ && addr == DMA_STAT(ch)) {
			if (!bm.on[c])
				;
			else if (bm.error)
				bm.stat[c] |= DMA_STAT_IRQERR | bm.errc << DMA_STAT_ERRC_SHIFT;
			else if (bm.stall != 0u)
				bm.stall--;
			else
				bm_move(c);
			*value = bm.stat[c];
		} else if (op == FAKE_MMR_READ && addr == DMA_XCNT_CUR(ch)) {
			*value = bm_left(c);
		}
	}
	if (op == FAKE_MMR_WRITE && addr == SPI_IMSK_SET(0))
		bm.imsk |= *value;
	else if (op == FAKE_MMR_WRITE && addr == SPI_IMSK_CLR(0))
		bm.imsk &= ~*value;
	else if (op == FAKE_MMR_WRITE && addr == SPI_ILAT_CLR(0))
		bm.ilat &= ~*value;
	if (op == FAKE_MMR_READ && addr == SPI_TWC(0)) {
		*value = dev.twc;
	} else if (op == FAKE_MMR_READ && addr == SPI_RWC(0)) {
		*value = dev.rwc;
	} else if (op == FAKE_MMR_READ && addr == SPI_ILAT(0)) {
		bm.ilat |= dev.stat & bm.imsk & SPI_STAT_TF;
		*value = bm.ilat;
	} else {
		r = dev_hook(ctx, op, width, addr, value);
	}
	if (op == FAKE_MMR_WRITE && addr == SPI_CTL(0))
		ses_push(S13B_EV_EN, (*value & SPI_CTL_EN) != 0u,
			 (int)((*value & SPI_CTL_MIOM_MASK) >> 20));
	else if (op == FAKE_MMR_WRITE &&
		 (addr == SPI_STAT(0) || addr == SPI_TWC(0) || addr == SPI_RWC(0) ||
		  addr == SPI_TXCTL(0) || addr == SPI_RXCTL(0) || addr == DMA_CFG(4) ||
		  addr == DMA_CFG(5) || addr == DMA_STAT(4) || addr == DMA_STAT(5) ||
		  addr == DMA_DSCPTR_CUR(4) || addr == DMA_DSCPTR_CUR(5)))
		ses_push(S13B_EV_SETUP, false, -1);
	s13b_access(&tr, op, addr, *value);
	return r;
}

/* The device's reply to a receive's word k: a count, so that each part's
 * bytes are known. */
static uint32_t count_reply(struct dev *d, uint32_t tx)
{
	(void)tx;
	return 0xA5000000u + (uint32_t)d->tfifo++;
}

static void bridge_up(struct wb_spi *s, uint32_t word_us)
{
	reset_dev(0, 3, take_reply);
	fake_mmr_set_hook(bm_hook, &dev);
	memset(&bm, 0, sizeof(bm));
	nsent = 0;
	nses = 0;
	CHECK(wb_spi_init(s, 0, &bridge_pre, &cclk400, word_us) == WB_SPI_OK);
	fake_core_set_cycle_step(400);		/* 1 us a clock read */
	fake_mmr_clear_log();
	s13b_start(&tr);
}

/* A session as the bridge's master makes it (S-09.010): PC11 low, the
 * transfer, the session closed, PC11 high. */
static void select_on(void)
{
	s13b_select(&tr, true);
	ses_push(S13B_EV_SELECT, true, -1);
}

static void select_off(const struct wb_spi *s)
{
	wb_spi_dma_close(s);
	s13b_select(&tr, false);
	ses_push(S13B_EV_SELECT, false, -1);
}

/* The rungs of S-13b.041's table, as the board sets them. */
static const struct wb_spi_config rungs[] = {
	{ 0x00002592u, 3u, 0u, 0u }, { 0x00042592u, 1u, 0u, 0u },
	{ 0x00242592u, 3u, 0u, 0u }, { 0x00242592u, 1u, 0u, 0u },
};

/* After a request session: EN clear, single I/O at the pre-link clock, both
 * channels off, DMA4 disabled with no status, TF masked. */
static bool send_left(void)
{
	return !(fake_mmr_get(SPI_CTL(0)) & (SPI_CTL_EN | SPI_CTL_MIOM_MASK)) &&
	       fake_mmr_get(SPI_CLK(0)) == 3u && !(dev.txctl & SPI_XCTL_EN) &&
	       !(dev.rxctl & SPI_XCTL_EN) && !bm.on[0] && bm.stat[0] == 0u &&
	       !(bm.imsk & SPI_STAT_TF) && bm.ilat == 0u;
}

/*
 * S-13b.083's send in every form: the plain set-up and each rung, lengths
 * from a header alone to the largest request; every word whole, and the
 * trace as check_bridge_dma's master_send and check_master_sessions want it.
 */
static void test_bridge_send(void)
{
	static const size_t lens[] = { 12u, 76u, 4096u + 12u, 0x10000u + 12u + 8u };
	static uint32_t buf[(0x10000u + 20u) / 4u];
	const uint8_t *b = (const uint8_t *)buf;
	size_t i, k;
	char name[64];

	for (i = 0; i < ARRAY_LEN(buf); i++)
		buf[i] = 0x9E3779B9u * (uint32_t)(i + 7u);
	for (k = 0; k <= ARRAY_LEN(rungs); k++) {
		for (i = 0; i < ARRAY_LEN(lens); i++) {
			const struct wb_spi_config *rung = k == 0u ? NULL : &rungs[k - 1u];
			struct wb_spi s;

			bridge_up(&s, 50u);
			select_on();
			CHECK(wb_spi_dma_send(&s, rung, b, lens[i]) == WB_SPI_OK);
			select_off(&s);
			CHECK(sent_is(b, lens[i]));
			CHECK(send_left());
			snprintf(name, sizeof name, "master_send_rung%zu_%zu", k, lens[i]);
			CHECK(s13b_ok(&tr, name, S13B_MASTER_SEND, (uint32_t)(lens[i] / 4u), NULL, 0u));
			CHECK(s13b_master_sessions(ses, nses) == 0u);
			CHECK(fault_is(&s, 0, 0u, 0u));
			CHECK(dev.violations == 0u && fake_mmr_errors() == 0u);
			if (rung != NULL)
				CHECK(dev.ctl == 0u);	/* SPI_CTL 0 after a rung */
		}
	}
}

/* A reply in its parts: each part's bytes where they belong, the bytes
 * around the buffer untouched, the trace master_receive's. */
static bool recv_parts(const size_t *parts, unsigned nparts, size_t lead,
		       const char *name)
{
	static _Alignas(32) uint8_t rbuf[0x10000u + 128u];
	uint32_t want[8];
	struct wb_spi s;
	size_t at = lead, i, k;
	unsigned p;
	bool ok = true;

	bridge_up(&s, 50u);
	dev.reply = count_reply;
	memset(rbuf, 0x5C, sizeof(rbuf));
	select_on();
	for (p = 0; p < nparts; p++) {
		ok = ok && wb_spi_dma_recv(&s, rbuf + at, parts[p]) == WB_SPI_OK;
		want[p] = (uint32_t)parts[p];
		at += parts[p];
	}
	select_off(&s);
	for (i = lead, k = 0; i < at; i += 4u, k++)
		ok = ok && rbuf[i] == (uint8_t)k && rbuf[i + 3u] == 0xA5u;
	for (i = 0; i < lead; i++)
		ok = ok && rbuf[i] == 0x5C;
	for (i = at; i < at + 64u; i++)
		ok = ok && rbuf[i] == 0x5C;
	ok = ok && s13b_ok(&tr, name, S13B_MASTER_RECEIVE, 0u, want, nparts);
	ok = ok && s13b_master_sessions(ses, nses) == 0u;
	ok = ok && !(fake_mmr_get(SPI_CTL(0)) & SPI_CTL_EN) && !(dev.rxctl & SPI_XCTL_EN) &&
	     !bm.on[1] && fault_is(&s, 0, 0u, 0u) && fake_mmr_errors() == 0u;
	return ok;
}

/*
 * S-13b.083's receive, and a reply taken in parts (S-13b.040): the first
 * part with EN clear and EN last, each later one with EN set; buffers in a
 * line, across lines, from a line's middle, with whole lines between.
 */
static void test_bridge_recv(void)
{
	static const size_t one[] = { 8u }, two[] = { 4u, 520u }, three[] = { 4u, 8u, 4u };
	static const size_t imm[] = { 8u, 4u }, big[] = { 4u, 0x10000u + 8u, 4u };
	static const size_t leads[] = { 0u, 4u, 20u, 28u, 32u };
	char name[64];
	size_t l;

	for (l = 0; l < ARRAY_LEN(leads); l++) {
		snprintf(name, sizeof name, "master_receive_one_%zu", leads[l]);
		CHECK(recv_parts(one, 1u, leads[l], name));
		snprintf(name, sizeof name, "master_receive_two_%zu", leads[l]);
		CHECK(recv_parts(two, 2u, leads[l], name));
		snprintf(name, sizeof name, "master_receive_three_%zu", leads[l]);
		CHECK(recv_parts(three, 3u, leads[l], name));
		snprintf(name, sizeof name, "master_receive_checked_%zu", leads[l]);
		CHECK(recv_parts(imm, 2u, leads[l], name));
	}
	CHECK(recv_parts(big, 3u, 20u, "master_receive_big"));
}

/* Sessions one after another, each set up with EN clear and EN set last,
 * SPI0 reset between them (S-13b.040), the rung's sends among them. */
static void test_bridge_sessions(void)
{
	static uint32_t req[19], rep[64];
	const uint8_t *b = (const uint8_t *)req;
	struct wb_spi s;
	unsigned k;

	bridge_up(&s, 50u);
	for (k = 0; k < 6u; k++) {
		select_on();
		CHECK(wb_spi_dma_send(&s, k % 2u ? &rungs[2 + k % 2u] : NULL, b,
				      sizeof(req)) == WB_SPI_OK);
		select_off(&s);
		select_on();
		CHECK(wb_spi_dma_recv(&s, (uint8_t *)rep, 4u) == WB_SPI_OK);
		CHECK(wb_spi_dma_recv(&s, (uint8_t *)rep + 4u, 4u + 4u * k) == WB_SPI_OK);
		select_off(&s);
	}
	CHECK(s13b_master_sessions(ses, nses) == 0u);
	CHECK(fake_mmr_errors() == 0u);
	/* the controller's set-up written with EN set is caught */
	CHECK(s13b_master_sessions((const struct s13b_sev[]){
		{ S13B_EV_EN, true, 0 }, { S13B_EV_SELECT, true, -1 }, { S13B_EV_WORD, false, -1 },
		{ S13B_EV_EN, false, -1 }, { S13B_EV_SELECT, false, -1 }, { S13B_EV_EN, true, -1 },
		{ S13B_EV_SETUP, false, -1 }, { S13B_EV_SELECT, true, -1 },
		{ S13B_EV_WORD, false, -1 } }, 9u) == S13B_ENABLE_LAST);
}

/* The controller and its channels as a fresh wb_spi_init() leaves them. */
static bool fresh_state(void)
{
	return fake_mmr_get(SPI_CTL(0)) == PRE_EN &&
	       fake_mmr_get(SPI_CLK(0)) == 3u && fake_mmr_get(SPI_DLY(0)) == 0u &&
	       fake_mmr_get(SPI_SLVSEL(0)) == SPI_SLVSEL_IDLE &&
	       dev.txctl == 0u && dev.rxctl == 0u &&
	       fake_mmr_get(SPI_TWC(0)) == 0u && fake_mmr_get(SPI_RWC(0)) == 0u &&
	       fake_mmr_get(SPI_TWCR(0)) == 0u && fake_mmr_get(SPI_RWCR(0)) == 0u &&
	       fake_mmr_get(DMA_CFG(4)) == 0u && fake_mmr_get(DMA_CFG(5)) == 0u &&
	       !bm.on[0] && !bm.on[1];
}

/* After a failed transfer (S-13b.083): its channel disabled, its status
 * acknowledged, TF masked, both channels off, EN clear. */
static bool failed_left(unsigned c)
{
	return !bm.on[c] && (bm.stat[c] & 7u) == 0u && !(bm.imsk & SPI_STAT_TF) &&
	       bm.ilat == 0u && dev.txctl == 0u && dev.rxctl == 0u &&
	       !(fake_mmr_get(SPI_CTL(0)) & SPI_CTL_EN);
}

/*
 * Every failure exit of a session, its record, what it leaves, and what the
 * bus does after it: wb_spi_spoiled() says so, wb_spi_restart() leaves the
 * controller as a fresh start does, and the next session goes through, set
 * up as a session's first.
 */
static void test_bridge_failures(void)
{
	static uint32_t buf[16];
	const uint8_t *b = (const uint8_t *)buf;
	uint32_t more[4];
	struct wb_spi s;
	unsigned k;

	/* a slave that holds every word: a stall after a word's bound */
	bridge_up(&s, 50u);
	bm.hold = 16u;
	dev.stat |= SPI_STAT_FCS;
	fake_core_set_cycles(0);
	CHECK(wb_spi_dma_send(&s, NULL, b, sizeof(buf)) == WB_SPI_TIMEOUT);
	CHECK(fake_core_peek_cycles() < 4u * 60u * 400u);
	CHECK(fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_DMA_STALL, 16u));
	wb_spi_last_fault_more(&s, more);
	CHECK(more[2] == 0x0001001Du && more[3] == 16u);	/* SPI_TXCTL, SPI_TWC */
	CHECK(failed_left(0));

	/* the DMA's error, with its cause */
	bridge_up(&s, 50u);
	bm.error = true;
	bm.errc = 5u;
	CHECK(wb_spi_dma_send(&s, &rungs[3], b, sizeof(buf)) == WB_SPI_ERR_HW);
	CHECK(fault_is(&s, WB_SPI_ERR_HW, WB_SPI_STAGE_DMA_ERR |
		       5u << WB_SPI_STAGE_ERRC_SHIFT, 16u));
	CHECK(failed_left(0) && fake_mmr_get(SPI_CTL(0)) == 0u &&
	      fake_mmr_get(SPI_CLK(0)) == 3u);

	/* every word delivered, TF never: the transmit's end */
	bridge_up(&s, 50u);
	bm.no_tf = true;
	CHECK(wb_spi_dma_send(&s, NULL, b, sizeof(buf)) == WB_SPI_TIMEOUT);
	CHECK(fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_TX_END, 0u));
	CHECK(failed_left(0));

	/* the DMA done, the slave holding the last word: TF's wait */
	bridge_up(&s, 50u);
	bm.hold = 1u;
	CHECK(wb_spi_dma_send(&s, NULL, b, sizeof(buf)) == WB_SPI_TIMEOUT);
	CHECK(fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_TX_END, 1u));
	CHECK(failed_left(0));

	/* a receive whose words never come: the receive channel's account */
	bridge_up(&s, 50u);
	dev.mute = true;
	fake_core_set_cycles(0);
	CHECK(wb_spi_dma_recv(&s, (uint8_t *)buf, 8u) == WB_SPI_TIMEOUT);
	CHECK(fake_core_peek_cycles() < 4u * 60u * 400u);
	CHECK(fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_RX_WORD, 2u));
	wb_spi_last_fault_more(&s, more);
	CHECK(more[2] == 0x0001001Du && more[3] == 2u);	/* SPI_RXCTL, SPI_RWC */
	CHECK(failed_left(1));

	/* a receive overrun found at a part's end */
	bridge_up(&s, 50u);
	bm.ror = true;
	CHECK(wb_spi_dma_recv(&s, (uint8_t *)buf, 8u) == WB_SPI_ERR_HW);
	CHECK(fault_is(&s, WB_SPI_ERR_HW, WB_SPI_STAGE_OVERRUN, 0u));
	CHECK(failed_left(1));

	/* after each: the bus's restart, then a session whose first part is
	 * set up as a first part */
	for (k = 0; k < 4u; k++) {
		int rc;

		bridge_up(&s, 50u);
		CHECK(!wb_spi_spoiled(&s));
		if (k == 0u) {
			bm.stall = 1000000u;
			rc = wb_spi_dma_send(&s, &rungs[2], b, sizeof(buf));
		} else if (k == 1u) {
			bm.error = true;
			rc = wb_spi_dma_send(&s, NULL, b, sizeof(buf));
		} else {
			CHECK(wb_spi_dma_recv(&s, (uint8_t *)buf, 4u) == WB_SPI_OK);
			bm.stall = k == 2u ? 1000000u : 0u;
			dev.mute = k == 3u;
			rc = wb_spi_dma_recv(&s, (uint8_t *)buf + 4u, 8u);
		}
		CHECK(rc != WB_SPI_OK);
		select_off(&s);
		CHECK(wb_spi_spoiled(&s));
		bm.stall = 0u;
		bm.error = false;
		dev.mute = false;
		CHECK(wb_spi_restart(&s, bridge_pre.ctl, bridge_pre.clk) == WB_SPI_OK);
		CHECK(fresh_state());
		s13b_start(&tr);
		nsent = 0;
		select_on();
		CHECK(wb_spi_dma_send(&s, NULL, b, 12u) == WB_SPI_OK);
		select_off(&s);
		CHECK(sent_is(b, 12u));
		CHECK(s13b_ok(&tr, "master_send_after_failure", S13B_MASTER_SEND, 3u, NULL, 0u));
		s13b_start(&tr);
		select_on();
		CHECK(wb_spi_dma_recv(&s, (uint8_t *)buf, 8u) == WB_SPI_OK);
		select_off(&s);
		CHECK(s13b_ok(&tr, "master_receive_after_failure", S13B_MASTER_RECEIVE, 0u,
			      (const uint32_t[]){ 8u }, 1u));
		CHECK(fault_is(&s, 0, 0u, 0u));
		CHECK(!wb_spi_spoiled(&s));
	}
}

/* Refused before anything is written. */
static void test_bridge_args(void)
{
	static uint32_t buf[4];
	const uint8_t *b = (const uint8_t *)buf;
	const struct wb_spi_config with_en = { 0x00242593u, 1u, 0u, 0u };
	const struct wb_spi_config size16 = { (0x00242592u & ~SPI_CTL_SIZE_MASK) |
					      SPI_CTL_SIZE16, 1u, 0u, 0u };
	struct wb_spi s;

	bridge_up(&s, 50u);
	CHECK(wb_spi_dma_send(&s, NULL, b, 6u) == WB_SPI_ERR_ARG);
	CHECK(wb_spi_dma_send(&s, NULL, b, 0u) == WB_SPI_ERR_ARG);
	CHECK(wb_spi_dma_send(&s, &with_en, b, 8u) == WB_SPI_ERR_ARG);
	CHECK(wb_spi_dma_send(&s, &size16, b, 8u) == WB_SPI_ERR_ARG);
	CHECK(wb_spi_dma_recv(&s, (uint8_t *)buf, 2u) == WB_SPI_ERR_ARG);
	CHECK(wb_spi_dma_recv(&s, (uint8_t *)buf, 4u * 0x10000u) == WB_SPI_ERR_ARG);
	CHECK(fault_is(&s, WB_SPI_ERR_ARG, WB_SPI_STAGE_ARG, 0u));
	CHECK(fake_mmr_count() == 0u);
}

/* The idle hook once a millisecond in a stall; none in a transfer that
 * moves. */
static void test_bridge_idle(void)
{
	static uint32_t buf[1024];
	const uint8_t *b = (const uint8_t *)buf;
	struct wb_spi s;

	bridge_up(&s, 5000u);
	wb_spi_set_idle(&s, count_idle);
	idles = 0;
	bm.stall = 100000000u;
	CHECK(wb_spi_dma_send(&s, NULL, b, 64u) == WB_SPI_TIMEOUT);
	CHECK(idles >= 4u && idles <= 5u);

	bridge_up(&s, 5000u);
	wb_spi_set_idle(&s, count_idle);
	idles = 0;
	CHECK(wb_spi_dma_send(&s, NULL, b, sizeof(buf)) == WB_SPI_OK);
	CHECK(wb_spi_dma_recv(&s, (uint8_t *)buf, sizeof(buf)) == WB_SPI_OK);
	CHECK(idles == 0u);
}

/*
 * The cache (S-13b.083): a send's lines written back; a receive's whole
 * lines written back and invalidated, its partial lines through the L1
 * buffers, never touched by the line operations; the arrays as S-13b.083
 * gives them.
 */
static void test_bridge_cache(void)
{
	static _Alignas(32) uint8_t cbuf[256];
	const struct wb_spi_array *a = &wb_spi_rx_array;
	struct wb_spi s;
	unsigned flushes, invs;

	/* a send of 76 bytes: three lines written back, one buffer */
	bridge_up(&s, 50u);
	flushes = fake_core_flushes();
	CHECK(wb_spi_dma_send(&s, NULL, cbuf, 76u) == WB_SPI_OK);
	CHECK(fake_core_flushes() - flushes >= 3u);
	CHECK(wb_spi_tx_array.n == 1u && wb_spi_tx_array.bytes[0] == 76u);
	CHECK((wb_spi_tx_array.d[0].cfg & ~DMA_CFG_MSIZE_MASK) ==
	      ((WB_DMA_ARRAY_SEND & ~(DMA_CFG_FLOW_MASK | DMA_CFG_NDSIZE_MASK)) | DMA_CFG_INT_X));
	CHECK(wb_spi_tx_array.d[0].mod == 4u && wb_spi_tx_array.d[0].count == 19u);

	/* a receive of 120 bytes from a line's middle: 12 bytes to the next
	 * line, three whole lines, 12 after; only the whole lines invalidated */
	bridge_up(&s, 50u);
	invs = fake_core_flushinvs();
	CHECK(wb_spi_dma_recv(&s, cbuf + 20u, 120u) == WB_SPI_OK);
	CHECK(a->n == 3u && a->bytes[0] == 12u && a->bytes[1] == 96u && a->bytes[2] == 12u);
	CHECK(a->to[0] == cbuf + 20u && a->mem[1] == cbuf + 32u && a->to[1] == NULL &&
	      a->to[2] == cbuf + 128u);
	CHECK(a->d[0].cfg == (WB_DMA_ARRAY_RECV | 2u << DMA_CFG_MSIZE_SHIFT));
	CHECK(a->d[1].cfg == (WB_DMA_ARRAY_RECV | 5u << DMA_CFG_MSIZE_SHIFT) &&
	      a->d[1].count == 3u && a->d[1].mod == 32u);
	CHECK((a->d[2].cfg & (DMA_CFG_FLOW_MASK | DMA_CFG_NDSIZE_MASK | DMA_CFG_INT_X)) ==
	      DMA_CFG_INT_X);
	/* the three whole lines and the two separate buffers' */
	CHECK(fake_core_flushinvs() - invs == 5u);
	/* within one line: one buffer, copied out */
	CHECK(wb_spi_dma_recv(&s, cbuf + 4u, 8u) == WB_SPI_OK);
	CHECK(a->n == 1u && a->to[0] == cbuf + 4u);
	select_off(&s);
}

/*
 * HRM "Stop Flow Mode": a channel disabled in the middle of its work keeps
 * RUN in its waiting state until what it began has returned; a stalled
 * send stops DMA4 and waits for RUN to read idle before it clears the
 * status and returns.
 */
static unsigned drain_reads;

static bool drain_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		       wb_mmr_addr addr, uint32_t *value)
{
	const bool r = bm_hook(ctx, op, width, addr, value);

	if (op == FAKE_MMR_READ && addr == DMA_STAT(4) && !bm.on[0] && drain_reads != 0u) {
		drain_reads--;
		*value |= 4u << 8;
	}
	return r;
}

static void test_bridge_stop_waits_for_idle(void)
{
	static uint32_t buf[16];
	const struct fake_mmr_access *l;
	size_t i, off = SIZE_MAX, clr = SIZE_MAX;
	unsigned reads = 0;
	uint32_t last = UINT32_MAX;
	struct wb_spi s;

	bridge_up(&s, 50u);
	fake_mmr_set_hook(drain_hook, &dev);
	drain_reads = 5u;
	bm.stall = 1000000u;
	CHECK(wb_spi_dma_send(&s, NULL, (const uint8_t *)buf, sizeof(buf)) ==
	      WB_SPI_TIMEOUT);
	l = fake_mmr_log();
	for (i = 0; i < fake_mmr_count(); i++) {
		if (l[i].op == FAKE_MMR_WRITE && l[i].addr == DMA_CFG(4) &&
		    l[i].value == 0u && off == SIZE_MAX)
			off = i;
		if (off != SIZE_MAX && clr == SIZE_MAX && l[i].op == FAKE_MMR_READ &&
		    l[i].addr == DMA_STAT(4)) {
			reads++;
			last = l[i].value;
		}
		if (off != SIZE_MAX && l[i].op == FAKE_MMR_WRITE &&
		    l[i].addr == DMA_STAT(4))
			clr = i;
	}
	CHECK(off != SIZE_MAX && clr != SIZE_MAX && off < clr);
	CHECK(reads == 6u && (last & DMA_STAT_RUN_MASK) == 0u);
	CHECK(failed_left(0));
}

int main(void)
{
	RUN(test_init);
	RUN(test_flash_read_and_id);
	RUN(test_flash_write);
	RUN(test_flash_args);
	RUN(test_flash_stuck);
	RUN(test_flash_retry);
	RUN(test_flash_first_frame);
	RUN(test_flash_read_fast);
	RUN(test_flash_read_dma);
	RUN(test_flash_read_dma_checks);
	RUN(test_flash_read_dma_fallback);
	RUN(test_s02_adapter);
	RUN(test_bridge_words);
	RUN(test_timeouts_and_errors);
	RUN(test_fast_path);
	RUN(test_words);
	RUN(test_plain_writes);
	RUN(test_fault_record);
	RUN(test_idle_hook);
	RUN(test_s13b_vectors);
	RUN(test_s13b_factory_end);
	RUN(test_bridge_send);
	RUN(test_bridge_recv);
	RUN(test_bridge_sessions);
	RUN(test_bridge_failures);
	RUN(test_bridge_args);
	RUN(test_bridge_idle);
	RUN(test_bridge_cache);
	RUN(test_bridge_stop_waits_for_idle);
	return check_summary("test_drv_spi");
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SPI controllers (see waffle/drivers/bf70x_spi.h). */
#include "waffle/drivers/bf70x_spi.h"

#include "waffle/drivers/bf70x_cplb.h"
#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/mmr.h"
#include "waffle/drivers/bf70x_core.h"
#include "waffle/testos.h"

#include <string.h>

int wb_spi_check(unsigned n, const struct wb_spi_config *c)
{
	if (n >= WB_SPI_COUNT || (c->ctl & (SPI_CTL_EN | SPI_CTL_MMSE)) ||
	    (c->ctl & SPI_CTL_SIZE_MASK) == SPI_CTL_SIZE_MASK ||
	    (c->ctl & SPI_CTL_MIOM_MASK) == SPI_CTL_MIOM_MASK ||
	    c->clk > 0xFFFFu || c->dly > 0x3FFu || c->sel > 7u)
		return WB_SPI_ERR_ARG;
	return WB_SPI_OK;
}

/* Each controller's last failure, and whether the next operation is a
 * session's first (after init or a reset), which clears it. */
static struct wb_spi_fault fault[WB_SPI_COUNT];
static uint32_t fault_more[WB_SPI_COUNT][4];
static bool fresh[WB_SPI_COUNT];

static void fault_clear(unsigned n)
{
	fault[n] = (struct wb_spi_fault){ 0 };
	fault_more[n][0] = fault_more[n][1] = fault_more[n][2] = fault_more[n][3] = 0u;
}

static void fault_open(unsigned n)
{
	if (fresh[n]) {
		fault_clear(n);
		fresh[n] = false;
	}
}

/* The record, and with it (wb_spi_last_fault_more()) the DMA channel's
 * DMA_STAT and DMA_CFG, SPI_TXCTL and SPI_TWC as they are now, or with `rx`
 * the receive channel's and SPI_RXCTL and SPI_RWC: every caller records
 * before it stops the channel or clears the controller. A refused call
 * touches no register. */
static int fault_record(unsigned n, int rc, uint32_t stat, uint32_t left,
			uint32_t stage, bool rx)
{
	if (rc != WB_SPI_OK && n < WB_SPI_COUNT) {
		const unsigned ch = (rx ? WB_DMA_SPI0_RX : WB_DMA_SPI0_TX) + 2u * n;

		fault[n].rc = rc;
		fault[n].stat = stat;
		fault[n].left = left;
		fault[n].stage = stage;
		if (stage == WB_SPI_STAGE_ARG) {
			fault_more[n][0] = fault_more[n][1] = 0u;
			fault_more[n][2] = fault_more[n][3] = 0u;
		} else {
			fault_more[n][0] = wb_mmr_read32(DMA_STAT(ch));
			fault_more[n][1] = wb_mmr_read32(DMA_CFG(ch));
			fault_more[n][2] = wb_mmr_read32(rx ? SPI_RXCTL(n) : SPI_TXCTL(n));
			fault_more[n][3] = wb_mmr_read32(rx ? SPI_RWC(n) : SPI_TWC(n));
		}
	}
	return rc;
}

static int fault_note(unsigned n, int rc, uint32_t stat, uint32_t left,
		      uint32_t stage)
{
	return fault_record(n, rc, stat, left, stage, false);
}

void wb_spi_last_fault(const struct wb_spi *s, struct wb_spi_fault *out)
{
	*out = fault[s->n];
}

void wb_spi_last_fault_more(const struct wb_spi *s, uint32_t out[4])
{
	unsigned i;

	for (i = 0; i < 4u; i++)
		out[i] = fault_more[s->n][i];
}

void wb_spi_slave_last_fault(const struct wb_spi_slave *s,
			     struct wb_spi_fault *out)
{
	*out = fault[s->spi.n];
}

void wb_spi_set_idle(struct wb_spi *s, void (*idle)(void))
{
	s->idle = idle;
}

/*
 * Silicon anomaly 19000038: a single SPI_SLVSEL write does not take
 * effect; the same value is written twice, back to back.
 */
static void write_slvsel(unsigned n, uint32_t v)
{
	wb_mmr_write32(SPI_SLVSEL(n), v);
	wb_mmr_write32(SPI_SLVSEL(n), v);
}

int wb_spi_init(struct wb_spi *s, unsigned n, const struct wb_spi_config *c,
		const struct wb_time *t, uint32_t word_timeout_us)
{
	uint32_t slvsel = SPI_SLVSEL_IDLE;

	if (wb_spi_check(n, c) != WB_SPI_OK)
		return WB_SPI_ERR_ARG;
	s->n = n;
	s->ctl = c->ctl;
	s->clk = c->clk;
	s->dly = c->dly;
	s->sel = c->sel;
	s->t = t;
	s->word_timeout_us = word_timeout_us;
	s->idle = NULL;
	fault_clear(n);
	fresh[n] = true;

	if (c->sel != 0)
		slvsel |= SPI_SLVSEL_SSE(c->sel);
	wb_mmr_write32(SPI_CTL(n), 0);
	write_slvsel(n, slvsel);
	wb_mmr_write32(SPI_CLK(n), c->clk);
	wb_mmr_write32(SPI_DLY(n), c->dly);
	wb_mmr_write32(SPI_RXCTL(n), 0);
	wb_mmr_write32(SPI_TXCTL(n), 0);
	/* both counters are off: no reload of a count that runs out */
	wb_mmr_write32(SPI_RWCR(n), 0);
	wb_mmr_write32(SPI_TWCR(n), 0);
	wb_mmr_write32(SPI_IMSK_CLR(n), 0xFFFFu);
	wb_mmr_write32(SPI_STAT(n), SPI_STAT_W1C);
	wb_mmr_write32(SPI_CTL(n), c->ctl | SPI_CTL_EN);
	wb_mmr_sync();
	return WB_SPI_OK;
}

int wb_spi_clock(const struct wb_spi *s, uint32_t clk, bool fast)
{
	if (clk > 0xFFFFu || (fast && !(s->ctl & SPI_CTL_MSTR)))
		return WB_SPI_ERR_ARG;
	wb_mmr_write32(SPI_CTL(s->n), 0);
	wb_mmr_write32(SPI_CLK(s->n), clk);
	wb_mmr_write32(SPI_CTL(s->n), s->ctl | (fast ? SPI_CTL_FMODE : 0u) | SPI_CTL_EN);
	wb_mmr_sync();
	fresh[s->n] = true;
	return WB_SPI_OK;
}

int wb_spi_reconfigure(struct wb_spi *s, uint32_t ctl, uint32_t clk)
{
	const struct wb_spi_config c = { ctl, clk, 0, s->sel };

	if (wb_spi_check(s->n, &c) != WB_SPI_OK)
		return WB_SPI_ERR_ARG;
	s->ctl = ctl;
	s->clk = clk;
	wb_mmr_write32(SPI_CTL(s->n), 0);
	wb_mmr_write32(SPI_CLK(s->n), clk);
	wb_mmr_write32(SPI_CTL(s->n), ctl | SPI_CTL_EN);
	wb_mmr_sync();
	return WB_SPI_OK;
}

/* A channel as a fresh start leaves it: stopped once idle
 * (wb_dma_stop_idle()), its registers 0. */
static void dma_fresh(unsigned ch, const struct wb_time *t)
{
	wb_dma_stop_idle(ch, t);
	wb_mmr_write32(DMA_ADDRSTART(ch), 0u);
	wb_mmr_write32(DMA_XCNT(ch), 0u);
	wb_mmr_write32(DMA_XMOD(ch), 0u);
}

bool wb_spi_spoiled(const struct wb_spi *s)
{
	return fault[s->n].rc != 0 ||
	       wb_dma_enabled(WB_DMA_SPI0_TX + 2u * s->n) ||
	       wb_dma_enabled(WB_DMA_SPI0_RX + 2u * s->n);
}

int wb_spi_restart(struct wb_spi *s, uint32_t ctl, uint32_t clk)
{
	const struct wb_spi_config c = { ctl, clk, s->dly, s->sel };
	const unsigned n = s->n;

	if (wb_spi_check(n, &c) != WB_SPI_OK)
		return WB_SPI_ERR_ARG;
	s->ctl = ctl;
	s->clk = clk;
	wb_mmr_write32(SPI_CTL(n), 0u);
	wb_mmr_write32(SPI_RXCTL(n), 0u);
	wb_mmr_write32(SPI_TXCTL(n), 0u);
	wb_mmr_sync();
	dma_fresh(WB_DMA_SPI0_TX + 2u * n, s->t);
	dma_fresh(WB_DMA_SPI0_RX + 2u * n, s->t);
	write_slvsel(n, s->sel != 0u ?
		     SPI_SLVSEL_IDLE | SPI_SLVSEL_SSE(s->sel) : SPI_SLVSEL_IDLE);
	wb_mmr_write32(SPI_CLK(n), clk);
	wb_mmr_write32(SPI_DLY(n), s->dly);
	wb_mmr_write32(SPI_RWC(n), 0u);
	wb_mmr_write32(SPI_RWCR(n), 0u);
	wb_mmr_write32(SPI_TWC(n), 0u);
	wb_mmr_write32(SPI_TWCR(n), 0u);
	wb_mmr_write32(SPI_IMSK_CLR(n), 0xFFFFu);
	wb_mmr_write32(SPI_ILAT_CLR(n), 0xFFFFFFFFu);
	wb_mmr_write32(SPI_STAT(n), SPI_STAT_W1C);
	wb_mmr_write32(SPI_CTL(n), ctl | SPI_CTL_EN);
	wb_mmr_sync();
	fresh[n] = true;
	return WB_SPI_OK;
}

void wb_spi_disable(unsigned n)
{
	if (n >= WB_SPI_COUNT)
		return;
	wb_mmr_write32(SPI_CTL(n), 0);
	wb_mmr_write32(SPI_RXCTL(n), 0);
	wb_mmr_write32(SPI_TXCTL(n), 0);
	wb_mmr_sync();
}

void wb_spi_select(const struct wb_spi *s, bool on)
{
	uint32_t v;

	if (s->sel == 0)
		return;
	v = wb_mmr_read32(SPI_SLVSEL(s->n));
	if (on)
		v &= ~SPI_SLVSEL_SSEL(s->sel);
	else
		v |= SPI_SLVSEL_SSEL(s->sel);
	write_slvsel(s->n, v);
	wb_mmr_sync();
}

/* A word of a transfer's buffer, as it lies in memory. */
typedef uint32_t __attribute__((may_alias)) spi_word;

static unsigned word_log2(const struct wb_spi *s)
{
	return (s->ctl & SPI_CTL_SIZE_MASK) >> SPI_CTL_SIZE_SHIFT;
}

unsigned wb_spi_word_bytes(const struct wb_spi *s)
{
	return 1u << word_log2(s);
}

/* Words are in memory order, which is little-endian on this core. */
static uint32_t get_word(const uint8_t *p, unsigned w)
{
	uint32_t v = 0;

	while (w-- > 0)
		v = v << 8 | p[w];
	return v;
}

static void put_word(uint8_t *p, unsigned w, uint32_t v)
{
	unsigned i;

	for (i = 0; i < w; i++)
		p[i] = (uint8_t)(v >> (8u * i));
}

static bool words_ok(const struct wb_spi *s, size_t n)
{
	return n != 0 && n % wb_spi_word_bytes(s) == 0 &&
	       n / wb_spi_word_bytes(s) <= UINT32_MAX;
}

/* A transfer that SPI_TWC or SPI_RWC counts: at most 16 bits of words. */
static bool counted_ok(const struct wb_spi *s, size_t n)
{
	return words_ok(s, n) && n / wb_spi_word_bytes(s) <= SPI_TWC_MAX;
}

/* A call refused for its arguments: an operation of the session all the
 * same, so a session's first one opens the record. */
static int refused(const struct wb_spi *s)
{
	if (s->n < WB_SPI_COUNT)
		fault_open(s->n);
	return fault_note(s->n, WB_SPI_ERR_ARG, 0u, 0u, WB_SPI_STAGE_ARG);
}

/* Wait until a STAT bit is clear, within one word's time. */
static int wait_clear(const struct wb_spi *s, uint32_t bit)
{
	return wb_poll32(s->t, SPI_STAT(s->n), bit, 0, s->word_timeout_us) ==
	       WB_TIME_OK ? WB_SPI_OK : WB_SPI_TIMEOUT;
}

/* One word's bound and a millisecond, in core cycles. */
static uint64_t word_cycles(const struct wb_spi *s)
{
	return (uint64_t)s->word_timeout_us * s->t->cycles_per_us;
}

static uint64_t ms_cycles(const struct wb_spi *s)
{
	return (uint64_t)1000u * s->t->cycles_per_us;
}

/*
 * The slow half of a master's wait, entered once STAT has been found not
 * ready: until (STAT & mask) == want. It gives up after a word's bound with
 * nothing moved, the bound starting at the call and again whenever the STAT
 * bits in `level` change (the transmit FIFO's level, for TF), so it bounds
 * a word, not the transfer. The idle hook runs once a millisecond. *stat is
 * the last STAT read.
 */
#if WB_SPI_BOOT_DMA
/* SPI_STAT.FCS, the slave's ready line holding the controller, sampled by
 * the waits of a boot part (wb_spi_dma_send_bytes()) and nothing else. */
static struct wb_spi_fcs boot_fcs;
static bool boot_fcs_on;
#define FCS_NOTE(v)                                                     \
	do {                                                            \
		if (boot_fcs_on) {                                      \
			boot_fcs.samples++;                             \
			if ((v) & SPI_STAT_FCS)                         \
				boot_fcs.held++;                        \
		}                                                       \
	} while (0)
#else
#define FCS_NOTE(v) ((void)0)
#endif

static int wait_stat(const struct wb_spi *s, uint32_t mask, uint32_t want,
		     uint32_t level_mask, uint32_t *stat)
{
	const uint64_t bound = word_cycles(s), ms = ms_cycles(s);
	uint64_t from = wb_core_cycles(), idle_at = from + ms;
	uint32_t level = UINT32_MAX;

	for (;;) {
		const uint64_t now = wb_core_cycles();
		const uint32_t v = wb_mmr_read32(SPI_STAT(s->n));

		*stat = v;
		FCS_NOTE(v);
		if ((v & mask) == want)
			return WB_SPI_OK;
		if ((v & level_mask) != level) {
			level = v & level_mask;
			from = now;
		} else if (now - from >= bound) {
			return WB_SPI_TIMEOUT;
		}
		if (s->idle != NULL && now >= idle_at) {
			s->idle();
			idle_at = wb_core_cycles() + ms;
		}
	}
}

static void begin(const struct wb_spi *s)
{
	fault_open(s->n);
	wb_mmr_write32(SPI_STAT(s->n), SPI_STAT_W1C);
}

/* Both channels off, status cleared; a receive overrun or mode fault fails. */
static int finish(const struct wb_spi *s, int rc)
{
	uint32_t stat = wb_mmr_read32(SPI_STAT(s->n));

	wb_mmr_write32(SPI_RXCTL(s->n), 0);
	wb_mmr_write32(SPI_TXCTL(s->n), 0);
	wb_mmr_write32(SPI_STAT(s->n), stat & SPI_STAT_W1C);
	wb_mmr_sync();
	if (rc == WB_SPI_OK && (stat & (SPI_STAT_ROR | SPI_STAT_MF)))
		rc = fault_note(s->n, WB_SPI_ERR_HW, stat, 0u,
				WB_SPI_STAGE_OVERRUN);
	return rc;
}

/* The wait for TF that ends every send, recorded when it fails. */
static int tx_end(const struct wb_spi *s)
{
	uint32_t stat = 0u;
	const int rc = wait_stat(s, SPI_STAT_TF, SPI_STAT_TF, SPI_STAT_TFS_MASK,
				 &stat);

	return rc == WB_SPI_OK ? rc :
	       fault_note(s->n, rc, stat, wb_mmr_read32(SPI_TWC(s->n)),
			  WB_SPI_STAGE_TX_END);
}

/*
 * A send's words, the status looked at first: a word goes into the
 * transmit FIFO at once when there is room, and only a FIFO found full
 * waits (wait_stat()). Then the wait for TF. A failure is recorded with
 * SPI_TWC as the words not sent.
 */
static int send_words(const struct wb_spi *s, const uint8_t *tx, size_t n)
{
	const wb_mmr_addr stat_reg = SPI_STAT(s->n), fifo = SPI_TFIFO(s->n);
	const unsigned w = wb_spi_word_bytes(s);
	uint32_t stat = 0u;
	int rc = WB_SPI_OK;

	if (w == 4u && ((uintptr_t)tx & 3u) == 0u) {
		const spi_word *p = (const spi_word *)(const void *)tx;
		const spi_word *const end = p + n / 4u;

		/* an empty FIFO (TFS) takes two words for one status read */
		while (p < end) {
			uint32_t v = wb_mmr_read32(stat_reg);

			if (v & SPI_STAT_TFF) {
				rc = wait_stat(s, SPI_STAT_TFF, 0u, 0u, &stat);
				if (rc != WB_SPI_OK)
					break;
				v = stat;
			}
			if ((v & SPI_STAT_TFS_MASK) == SPI_STAT_TFS_EMPTY &&
			    end - p >= 2) {
				wb_mmr_write32(fifo, p[0]);
				wb_mmr_write32(fifo, p[1]);
				p += 2;
			} else {
				wb_mmr_write32(fifo, *p++);
			}
		}
	} else {
		size_t i;

		for (i = 0; i < n; i += w) {
			if ((wb_mmr_read32(stat_reg) & SPI_STAT_TFF) &&
			    (rc = wait_stat(s, SPI_STAT_TFF, 0u, 0u, &stat)) != WB_SPI_OK)
				break;
			wb_mmr_write32(fifo, get_word(tx + i, w));
		}
	}
	if (rc != WB_SPI_OK)
		return fault_note(s->n, rc, stat, wb_mmr_read32(SPI_TWC(s->n)),
				  WB_SPI_STAGE_TX_ROOM);
	return tx_end(s);
}

int wb_spi_xfer(const struct wb_spi *s, const uint8_t *tx, uint8_t *rx,
		size_t n)
{
	unsigned w = wb_spi_word_bytes(s);
	int rc = WB_SPI_OK;
	size_t i;

	if (!words_ok(s, n) || (s->ctl & SPI_CTL_MIOM_MASK))
		return refused(s);
	begin(s);
	/* The non-initiating channel first (HRM "SPI Programming"). */
	wb_mmr_write32(SPI_RXCTL(s->n), SPI_XCTL_EN);
	wb_mmr_write32(SPI_TXCTL(s->n), SPI_XCTL_EN | SPI_XCTL_TI);
	for (i = 0; i < n; i += w) {
		uint32_t v;

		rc = wait_clear(s, SPI_STAT_TFF);
		if (rc != WB_SPI_OK) {
			(void)fault_note(s->n, rc, wb_mmr_read32(SPI_STAT(s->n)),
					 (uint32_t)((n - i) / w), WB_SPI_STAGE_TX_ROOM);
			break;
		}
		wb_mmr_write32(SPI_TFIFO(s->n), tx ? get_word(tx + i, w) : 0);
		rc = wait_clear(s, SPI_STAT_RFE);
		if (rc != WB_SPI_OK) {
			(void)fault_note(s->n, rc, wb_mmr_read32(SPI_STAT(s->n)),
					 (uint32_t)((n - i) / w), WB_SPI_STAGE_RX_WORD);
			break;
		}
		v = wb_mmr_read32(SPI_RFIFO(s->n));
		if (rx)
			put_word(rx + i, w, v);
	}
	return finish(s, rc);
}

/* A run of single words: its first failure, for wb_spi_words_end(). */
static int words_rc[WB_SPI_COUNT];

/* Status reads a word in flight gets before the clock is looked at: a byte
 * at SCLK0 / 3 is in within a few. */
#define WORD_SPIN 64u

int wb_spi_words_begin(const struct wb_spi *s)
{
	if (s->ctl & SPI_CTL_MIOM_MASK)
		return refused(s);
	begin(s);
	words_rc[s->n] = WB_SPI_OK;
	/* The non-initiating channel first (HRM "SPI Programming"). */
	wb_mmr_write32(SPI_RXCTL(s->n), SPI_XCTL_EN);
	wb_mmr_write32(SPI_TXCTL(s->n), SPI_XCTL_EN | SPI_XCTL_TI);
	return WB_SPI_OK;
}

uint32_t wb_spi_word(const struct wb_spi *s, uint32_t tx)
{
	const wb_mmr_addr stat_reg = SPI_STAT(s->n);
	uint32_t stat = 0u;
	unsigned spin;
	int rc;

	/* the transmit FIFO is empty: the word before this one came back */
	wb_mmr_write32(SPI_TFIFO(s->n), tx);
	for (spin = 0; spin < WORD_SPIN; spin++)
		if (!(wb_mmr_read32(stat_reg) & SPI_STAT_RFE))
			return wb_mmr_read32(SPI_RFIFO(s->n));
	rc = wait_stat(s, SPI_STAT_RFE, 0u, 0u, &stat);
	if (rc == WB_SPI_OK)
		return wb_mmr_read32(SPI_RFIFO(s->n));
	if (words_rc[s->n] == WB_SPI_OK)
		words_rc[s->n] = fault_note(s->n, rc, stat, 1u, WB_SPI_STAGE_RX_WORD);
	return UINT32_MAX;
}

bool wb_spi_words_failed(const struct wb_spi *s)
{
	return words_rc[s->n] != WB_SPI_OK;
}

int wb_spi_words_end(const struct wb_spi *s)
{
	return finish(s, words_rc[s->n]);
}

int wb_spi_send(const struct wb_spi *s, const uint8_t *tx, size_t n)
{
	if (!counted_ok(s, n))
		return refused(s);
	begin(s);
	wb_mmr_write32(SPI_TWC(s->n), (uint32_t)(n / wb_spi_word_bytes(s)));
	wb_mmr_write32(SPI_TXCTL(s->n),
		       SPI_XCTL_EN | SPI_XCTL_TI | SPI_XCTL_WCEN);
	return finish(s, send_words(s, tx, n));
}

/*
 * As send_words(): a word is read at once when the receive FIFO has one,
 * and only an empty FIFO waits.
 */
int wb_spi_recv(const struct wb_spi *s, uint8_t *rx, size_t n)
{
	const wb_mmr_addr stat_reg = SPI_STAT(s->n), fifo = SPI_RFIFO(s->n);
	const unsigned w = wb_spi_word_bytes(s);
	uint32_t stat = 0u;
	int rc = WB_SPI_OK;
	size_t i;

	if (!counted_ok(s, n))
		return refused(s);
	begin(s);
	wb_mmr_write32(SPI_RWC(s->n), (uint32_t)(n / w));
	wb_mmr_write32(SPI_RXCTL(s->n),
		       SPI_XCTL_EN | SPI_XCTL_TI | SPI_XCTL_WCEN);
	if (w == 4u && ((uintptr_t)rx & 3u) == 0u) {
		spi_word *p = (spi_word *)(void *)rx;
		const spi_word *const end = p + n / 4u;

		/* a full FIFO (RFS) gives two words for one status read */
		while (p < end) {
			uint32_t v = wb_mmr_read32(stat_reg);

			if (v & SPI_STAT_RFE) {
				rc = wait_stat(s, SPI_STAT_RFE, 0u, 0u, &stat);
				if (rc != WB_SPI_OK)
					break;
				v = stat;
			}
			if ((v & SPI_STAT_RFS_MASK) == SPI_STAT_RFS_FULL &&
			    end - p >= 2) {
				p[0] = wb_mmr_read32(fifo);
				p[1] = wb_mmr_read32(fifo);
				p += 2;
			} else {
				*p++ = wb_mmr_read32(fifo);
			}
		}
		i = (size_t)((uint8_t *)p - rx);
	} else {
		for (i = 0; i < n; i += w) {
			if ((wb_mmr_read32(stat_reg) & SPI_STAT_RFE) &&
			    (rc = wait_stat(s, SPI_STAT_RFE, 0u, 0u, &stat)) != WB_SPI_OK)
				break;
			put_word(rx + i, w, wb_mmr_read32(fifo));
		}
	}
	if (rc != WB_SPI_OK)
		(void)fault_note(s->n, rc, stat, (uint32_t)((n - i) / w),
				 WB_SPI_STAGE_RX_WORD);
	return finish(s, rc);
}

/*
 * wb_spi_recv()'s receive with the words taken from the receive FIFO by the
 * controller's receive DMA channel (S-13b.080): bytes only, single I/O.
 * The channel first, then the receive channel with RTI under RWC, as
 * wb_spi_recv() starts it, and RDR set to "not empty" so that each word
 * asks the channel for its move. The wait ends at the channel's done status
 * (its interrupt is not enabled at the SEC); it gives up on a DMA error, or
 * after a word's bound with the channel's count still, the idle hook once a
 * millisecond. A done counts only with the words moved: SPI_RWC run out,
 * the channel's address advanced by the length, and the receive FIFO
 * empty. On any failure the receive channel is turned off and the core
 * reads the receive FIFO empty (bounded), so no word a channel left there
 * can lead the next read. The buffer's cache lines are invalidated before
 * the start (wb_dma_start()) and after the end, so no line holds a byte
 * the channel did not write. A channel found running refuses the call;
 * every other return leaves the channel stopped.
 */
#define RECV_DMA_RXCTL (SPI_XCTL_EN | SPI_XCTL_TI | SPI_XCTL_WCEN | SPI_XCTL_DR_NE)
/* More reads than the receive FIFO holds words. */
#define RECV_DMA_DRAIN 64u

static uint8_t recv_drained[WB_SPI_COUNT];

unsigned wb_spi_recv_dma_drained(const struct wb_spi *s)
{
	return s->n < WB_SPI_COUNT ? recv_drained[s->n] : 0u;
}

/* The receive channel off, then the receive FIFO read until it is empty. */
static unsigned recv_drain(const struct wb_spi *s)
{
	unsigned k = 0;

	wb_mmr_write32(SPI_RXCTL(s->n), 0);
	wb_mmr_sync();
	while (k < RECV_DMA_DRAIN && !(wb_mmr_read32(SPI_STAT(s->n)) & SPI_STAT_RFE)) {
		(void)wb_mmr_read32(SPI_RFIFO(s->n));
		k++;
	}
	return k;
}

static int recv_dma_wait(const struct wb_spi *s, unsigned ch, uint32_t *stage)
{
	const uint64_t bound = word_cycles(s), ms = ms_cycles(s);
	uint64_t from = wb_core_cycles(), idle_at = from + ms;
	uint32_t was = UINT32_MAX;

	for (;;) {
		const uint64_t now = wb_core_cycles();
		const uint32_t dstat = wb_mmr_read32(DMA_STAT(ch));
		const uint32_t left = wb_mmr_read32(DMA_XCNT_CUR(ch));

		if (dstat & DMA_STAT_IRQERR) {
			*stage = WB_SPI_STAGE_DMA_ERR |
				 ((dstat & DMA_STAT_ERRC_MASK) >> DMA_STAT_ERRC_SHIFT)
					 << WB_SPI_STAGE_ERRC_SHIFT;
			return WB_SPI_ERR_HW;
		}
		if (dstat & DMA_STAT_IRQDONE)
			return WB_SPI_OK;
		if (left != was) {
			was = left;
			from = now;
		} else if (now - from >= bound) {
			*stage = WB_SPI_STAGE_RX_WORD;
			return WB_SPI_TIMEOUT;
		}
		if (s->idle != NULL && now >= idle_at) {
			s->idle();
			idle_at = wb_core_cycles() + ms;
		}
	}
}

int wb_spi_recv_dma(const struct wb_spi *s, uint8_t *rx, size_t n)
{
	const unsigned ch = WB_DMA_SPI0_RX + 2u * s->n;
	const uint32_t at = (uint32_t)(uintptr_t)rx;
	struct wb_dma_xfer x = { 0 };
	uint32_t stage = WB_SPI_STAGE_NONE;
	int rc;

	if (s->n < WB_SPI_COUNT)
		recv_drained[s->n] = 0u;
	if (!counted_ok(s, n) || wb_spi_word_bytes(s) != 1u ||
	    (s->ctl & SPI_CTL_MIOM_MASK) ||
	    (wb_dma_enabled(ch) && (wb_mmr_read32(DMA_STAT(ch)) & DMA_STAT_RUN_MASK) != 0u))
		return refused(s);
	/* a channel left enabled and idle (a boot ROM's, say) is stopped: it
	 * serves this controller alone (S-13b.080) */
	if (wb_dma_enabled(ch))
		wb_dma_stop(ch);
	x.addr = at;
	x.count = (uint32_t)n;
	x.msize = WB_DMA_1B;
	x.psize = WB_DMA_1B;
	x.to_mem = true;
	x.irq = true;
	begin(s);
	if (wb_dma_start(ch, &x) != WB_DMA_OK)
		return finish(s, fault_record(s->n, WB_SPI_ERR_HW, wb_mmr_read32(SPI_STAT(s->n)),
					      (uint32_t)n, WB_SPI_STAGE_DMA_START, true));
	wb_mmr_write32(SPI_RWC(s->n), (uint32_t)n);
	wb_mmr_write32(SPI_RXCTL(s->n), RECV_DMA_RXCTL);
	wb_mmr_sync();
	rc = recv_dma_wait(s, ch, &stage);
	if (rc == WB_SPI_OK) {
		if (wb_mmr_read32(SPI_RWC(s->n)) != 0u ||
		    wb_mmr_read32(DMA_ADDR_CUR(ch)) != at + (uint32_t)n) {
			rc = WB_SPI_ERR_HW;
			stage = WB_SPI_STAGE_DMA_UNMOVED;
		} else if (!(wb_mmr_read32(SPI_STAT(s->n)) & SPI_STAT_RFE)) {
			rc = WB_SPI_ERR_HW;
			stage = WB_SPI_STAGE_DMA_LEFT;
		}
	}
	if (rc != WB_SPI_OK)
		(void)fault_record(s->n, rc, wb_mmr_read32(SPI_STAT(s->n)),
				   wb_mmr_read32(DMA_XCNT_CUR(ch)), stage, true);
	wb_dma_stop_idle(ch, s->t);
	if (rc != WB_SPI_OK)
		recv_drained[s->n] = (uint8_t)recv_drain(s);
	wb_dcache_invalidate_range(at, (uint32_t)n);
	return finish(s, rc);
}

#if defined(OPXY_TEST_OS)
#include "../../testos/spi_master_q11.inc"
#endif

/* ---- the slave end ---- */

/* S-13b.083: the end each slave's replies use now, and whether a reply on
 * the TF end changed it to the count end. Plain memory, so that a
 * developer's slave read (S-10c.110) can see it. */
static uint8_t reply_end[WB_SPI_COUNT], reply_changed[WB_SPI_COUNT];

enum wb_spi_reply_end wb_spi_slave_reply_end(const struct wb_spi_slave *s,
					     bool *changed)
{
	if (changed != NULL)
		*changed = reply_changed[s->spi.n] != 0u;
	return reply_end[s->spi.n] == WB_SPI_END_COUNT ? WB_SPI_END_COUNT :
							 WB_SPI_END_TF;
}

bool wb_spi_slave_reply_end_set(const struct wb_spi_slave *s,
				enum wb_spi_reply_end end)
{
	if (reply_changed[s->spi.n] != 0u && end != WB_SPI_END_COUNT)
		return false;
	reply_end[s->spi.n] = (uint8_t)end;
	return true;
}

int wb_spi_slave_init(struct wb_spi_slave *s, unsigned n,
		      const struct wb_spi_config *c, uint8_t ready_pin,
		      uint8_t master_pin, const struct wb_time *t,
		      uint32_t word_timeout_us)
{
	uint32_t port = WB_PIN_PORT(ready_pin), bit = WB_PIN_BIT(ready_pin);

	if (wb_spi_check(n, c) != WB_SPI_OK || (c->ctl & SPI_CTL_MSTR) ||
	    !(c->ctl & SPI_CTL_FCEN) || c->sel != 0 ||
	    ready_pin >= WB_PIN_COUNT || master_pin >= WB_PIN_COUNT ||
	    ready_pin == master_pin)
		return WB_SPI_ERR_ARG;
	reply_end[n] = (uint8_t)WB_SPI_REPLY_END_BUILD;
	reply_changed[n] = 0u;
	s->ready_pin = ready_pin;
	s->master_pin = master_pin;
	/* S-13b.043: as a GPIO, an output driven high, its input off */
	wb_mmr_write32(PORT_INEN_CLR(port), bit);
	(void)wb_gpio_output(ready_pin, true);
	(void)wb_gpio_gpio(ready_pin);
	(void)wb_gpio_input(master_pin);
	return wb_spi_init(&s->spi, n, c, t, word_timeout_us);
}

#if defined(OPXY_TEST_OS)
#include "../../testos/spi_slave_records.inc"
#endif

#if defined(OPXY_TEST_OS)
/* SPI_STAT, or-ed into the record while an armed listen's request comes in. */
static uint32_t testos_stat(const struct wb_spi *p)
{
	const uint32_t stat = wb_mmr_read32(SPI_STAT(p->n));

	if (testos_armed_listen)
		wb_spi_testos_reqstat[OPXY_REQ_STAT] |= stat;
	return stat;
}
#endif

/* ---- the bridge's sessions by DMA (S-13b.083) ---- */

#define BRIDGE_TXCTL 0x0001001Du	/* TEN, TTI, TWCEN, TDR 1, TUWM 1 */
#define MASTER_RXCTL 0x0001001Du	/* REN, RTI, RWCEN, RDR 1, RUWM 1 */
#define SLAVE_RXCTL  0x00010015u	/* REN, RTI, RDR 1, RUWM 1: no word count */
#define DMA_W1C      (DMA_STAT_IRQDONE | DMA_STAT_IRQERR | DMA_STAT_PIRQ)
/* A part of a slave's receive: less than WB_SPI_BRIDGE_MAX, whole
 * 32-byte words of the dropped buffer (S-13b.083). */
#define SLAVE_PART_MAX (WB_SPI_BRIDGE_MAX - WB_DCACHE_LINE)

struct wb_spi_array wb_spi_tx_array WB_L1_BSS, wb_spi_rx_array WB_L1_BSS;
/* A receive's partial first and last cache lines, its dropped words, and a
 * reply's two queued words: L1, which no cache line holds. */
static _Alignas(32) uint8_t bounce[2][WB_DCACHE_LINE] WB_L1_BSS;
static _Alignas(32) uint8_t dropped[WB_DCACHE_LINE] WB_L1_BSS;
static _Alignas(8) uint32_t queued[2] WB_L1_BSS;

/* A master's receive session open: its later parts find EN set. */
static bool rx_open[WB_SPI_COUNT];
/* The clock a rung's send puts back (S-13b.040). */
static uint32_t last_clk[WB_SPI_COUNT];

/* Descriptors for n bytes at mem, split where one would be too long;
 * false when the array is full. */
static bool arr_put(struct wb_spi_array *a, uint32_t cfg, uint8_t *mem,
		    uint8_t *to, uint64_t n, bool same)
{
	while (n != 0u) {
		uint64_t k;

		if (a->n == WB_SPI_DESC_MAX)
			return false;
		k = wb_dma_desc_set(&a->d[a->n], cfg, (uint32_t)(uintptr_t)mem, n,
				    same);
		a->mem[a->n] = mem;
		a->to[a->n] = to;
		a->bytes[a->n] = k;
		a->n++;
		n -= k;
		if (!same) {
			mem += k;
			if (to != NULL)
				to += k;
		}
	}
	return true;
}

/*
 * A receive's buffer. Outside L1, its whole cache lines are written back and
 * invalidated and the DMA writes them directly; the bytes before the first
 * whole line and after the last go to their own 32-byte buffers (no line
 * the DMA writes holds anything else), copied out by arr_copy().
 */
static bool arr_recv(struct wb_spi_array *a, uint8_t *p, size_t n)
{
	const uint32_t at = (uint32_t)(uintptr_t)p;
	size_t head, mid;

	if (n == 0u)
		return true;
	if (wb_dma_in_l1(at))
		return arr_put(a, WB_DMA_ARRAY_RECV, p, NULL, n, false);
	head = (size_t)((0u - at) & (WB_DCACHE_LINE - 1u));
	if (head > n)
		head = n;
	mid = (n - head) & ~(size_t)(WB_DCACHE_LINE - 1u);
	wb_dma_cache_recv((uint32_t)(uintptr_t)bounce, sizeof bounce);
	if (head != 0u &&
	    !arr_put(a, WB_DMA_ARRAY_RECV, bounce[0], p, head, false))
		return false;
	wb_dma_cache_recv(at + (uint32_t)head, (uint32_t)mid);
	if (mid != 0u &&
	    !arr_put(a, WB_DMA_ARRAY_RECV, p + head, NULL, mid, false))
		return false;
	return n - head - mid == 0u ||
	       arr_put(a, WB_DMA_ARRAY_RECV, bounce[1], p + head + mid,
		       n - head - mid, false);
}

/* n bytes received and dropped, every word to the same place: what is
 * not a whole number of 32-byte words first, then the rest 32 bytes a word,
 * so that S-09.037's longest (2^32 - 1 sectors) takes few descriptors. */
static bool arr_drop(struct wb_spi_array *a, uint64_t n)
{
	const uint64_t odd = n % sizeof dropped;

	wb_dma_cache_recv((uint32_t)(uintptr_t)dropped, sizeof dropped);
	return arr_put(a, WB_DMA_ARRAY_RECV, dropped, NULL, odd, true) &&
	       arr_put(a, WB_DMA_ARRAY_RECV, dropped, NULL, n - odd, true);
}

/* The array's last descriptor marked, the array where the DMA reads it. */
static void arr_done(struct wb_spi_array *a)
{
	wb_dma_desc_last(&a->d[a->n - 1u]);
	wb_dma_cache_send((uint32_t)(uintptr_t)a->d,
			  (uint32_t)(a->n * sizeof a->d[0]));
}

/* A receive's separate buffers copied to their place. */
static void arr_copy(const struct wb_spi_array *a)
{
	unsigned i;

	for (i = 0; i < a->n; i++)
		if (a->to[i] != NULL)
			memcpy(a->to[i], a->mem[i], (size_t)a->bytes[i]);
}

static void arr_reset(struct wb_spi_array *a)
{
	a->n = 0;
}

/*
 * S-13b.083's waits: until the channel's done status, the bound a word's,
 * from the last movement of the DMA's count, its descriptor or the word
 * count (`count`, 0 for none): a transfer that does not move has failed.
 * The idle hook runs once a millisecond. `first`: the bound before the
 * first movement, 0 for a word's. With `watch`, the slave: the master-ready
 * pin high with no word waiting is the master's end (*ended set).
 */
static int dma_wait(const struct wb_spi *s, unsigned ch, wb_mmr_addr count,
		    uint64_t first, const struct wb_spi_slave *watch,
		    bool *ended, uint32_t *stage)
{
	const uint64_t ms = ms_cycles(s);
	uint64_t bound = first != 0u ? first : word_cycles(s);
	uint64_t from = wb_core_cycles(), idle_at = from + ms;
	uint32_t was[3] = { UINT32_MAX, UINT32_MAX, UINT32_MAX };

	for (;;) {
		const uint64_t now = wb_core_cycles();
		const uint32_t dstat = wb_mmr_read32(DMA_STAT(ch));
		const uint32_t is[3] = {
			wb_mmr_read32(DMA_XCNT_CUR(ch)),
			wb_mmr_read32(DMA_DSCPTR_CUR(ch)),
			count != 0u ? wb_mmr_read32(count) : 0u,
		};

		OPXY_TESTOS(if (watch != NULL) (void)testos_stat(s);)
#if WB_SPI_BOOT_DMA
		if (boot_fcs_on)
			FCS_NOTE(wb_mmr_read32(SPI_STAT(s->n)));
#endif
		if (dstat & DMA_STAT_IRQERR) {
			*stage = WB_SPI_STAGE_DMA_ERR |
				 ((dstat & DMA_STAT_ERRC_MASK) >> DMA_STAT_ERRC_SHIFT)
					 << WB_SPI_STAGE_ERRC_SHIFT;
			return WB_SPI_ERR_HW;
		}
		if (dstat & DMA_STAT_IRQDONE)
			return WB_SPI_OK;
		if (is[0] != was[0] || is[1] != was[1] || is[2] != was[2]) {
			if (was[0] != UINT32_MAX)
				bound = word_cycles(s);
			was[0] = is[0];
			was[1] = is[1];
			was[2] = is[2];
			from = now;
		} else if (watch != NULL && wb_gpio_get(watch->master_pin) &&
			   (wb_mmr_read32(SPI_STAT(s->n)) & SPI_STAT_RFE) &&
			   !(wb_mmr_read32(DMA_STAT(ch)) & (DMA_STAT_IRQDONE |
							    DMA_STAT_FIFOFILL_MASK))) {
			*ended = true;
			*stage = WB_SPI_STAGE_MASTER_END;
			return WB_SPI_TIMEOUT;
		} else if (now - from >= bound) {
			*stage = WB_SPI_STAGE_DMA_STALL;
			return WB_SPI_TIMEOUT;
		}
		if (s->idle != NULL && now >= idle_at) {
			s->idle();
			idle_at = wb_core_cycles() + ms;
		}
	}
}

/*
 * A master's transfer given up: recorded, both channels stopped once idle
 * with their status acknowledged, TF masked and its latch cleared, both
 * controller channels off, the status cleared and EN clear; after a rung's
 * send SPI_CTL 0 and its clock put back.
 */
static int master_fail(const struct wb_spi *s, int rc, uint32_t stage,
		       bool rx, uint32_t ctl, bool rung)
{
	const unsigned n = s->n;

	(void)fault_record(n, rc, wb_mmr_read32(SPI_STAT(n)),
			   wb_mmr_read32(rx ? SPI_RWC(n) : SPI_TWC(n)), stage, rx);
	wb_dma_stop_idle(WB_DMA_SPI0_TX + 2u * n, s->t);
	wb_dma_stop_idle(WB_DMA_SPI0_RX + 2u * n, s->t);
	wb_mmr_write32(SPI_IMSK_CLR(n), SPI_STAT_TF);
	wb_mmr_write32(SPI_ILAT_CLR(n), SPI_STAT_TF);
	wb_mmr_write32(SPI_TXCTL(n), 0u);
	wb_mmr_write32(SPI_RXCTL(n), 0u);
	wb_mmr_write32(SPI_STAT(n), SPI_STAT_W1C);
	wb_mmr_write32(SPI_CTL(n), ctl);	/* EN cleared, MIOM kept */
	if (rung) {
		wb_mmr_write32(SPI_CTL(n), 0u);
		wb_mmr_write32(SPI_CLK(n), last_clk[n]);
	}
	wb_mmr_sync();
	rx_open[n] = false;
	return rc;
}

/* The wait for SPI_ILAT.TF, the bound restarting while SPI_TWC or the
 * transmit FIFO's level moves. */
static int tf_wait(const struct wb_spi *s)
{
	const uint64_t bound = word_cycles(s), ms = ms_cycles(s);
	uint64_t from = wb_core_cycles(), idle_at = from + ms;
	uint32_t level = UINT32_MAX;

	for (;;) {
		const uint64_t now = wb_core_cycles();
		const uint32_t v = (wb_mmr_read32(SPI_STAT(s->n)) & SPI_STAT_TFS_MASK) |
				   (wb_mmr_read32(SPI_TWC(s->n)) & 0xFFFFu);

		if (wb_mmr_read32(SPI_ILAT(s->n)) & SPI_STAT_TF)
			return WB_SPI_OK;
		if (v != level) {
			level = v;
			from = now;
		} else if (now - from >= bound) {
			return WB_SPI_TIMEOUT;
		}
		if (s->idle != NULL && now >= idle_at) {
			s->idle();
			idle_at = wb_core_cycles() + ms;
		}
	}
}

/*
 * S-13b.083's send. The DMA's done status says only that the last word has
 * reached the transmit FIFO; the end is TF, unmasked once that status has
 * been seen. Both are read here (no handler): S-13b.083 fixes the registers
 * and their order, not how the events are taken, and a polled wait keeps
 * every bound and failure path in this function.
 */
int wb_spi_dma_send(const struct wb_spi *s, const struct wb_spi_config *rung,
		    const uint8_t *buf, size_t n)
{
	const unsigned ch = WB_DMA_SPI0_TX + 2u * s->n;
	const uint32_t ctl = rung != NULL ? rung->ctl : s->ctl;
	struct wb_spi_array *a = &wb_spi_tx_array;
	uint32_t stage = WB_SPI_STAGE_NONE, stat;
	int rc;

	if (!counted_ok(s, n) || n >= WB_SPI_BRIDGE_MAX ||
	    (rung != NULL && (wb_spi_check(s->n, rung) != WB_SPI_OK ||
			      ((rung->ctl ^ s->ctl) & SPI_CTL_SIZE_MASK) != 0u)))
		return refused(s);
	arr_reset(a);
	if (!arr_put(a, WB_DMA_ARRAY_SEND, (uint8_t *)(uintptr_t)buf, NULL, n,
		     false))
		return refused(s);
	arr_done(a);
	wb_dma_cache_send((uint32_t)(uintptr_t)buf, (uint32_t)n);
	fresh[s->n] = true;
	fault_open(s->n);
	rx_open[s->n] = false;
	if (rung != NULL) {
		last_clk[s->n] = wb_mmr_read32(SPI_CLK(s->n));
		wb_mmr_write32(SPI_CTL(s->n), 0u);
		wb_mmr_write32(SPI_CLK(s->n), rung->clk);
	}
	wb_mmr_write32(SPI_CTL(s->n), ctl);	/* EN clear: the write that may change MIOM */
	wb_mmr_write32(SPI_RXCTL(s->n), 0u);	/* the receive channel off */
	wb_mmr_write32(SPI_STAT(s->n), 0xFFFFFFFFu);
	wb_mmr_write32(DMA_STAT(ch), DMA_W1C);
	wb_dma_array_start(ch, a->d, WB_DMA_ARRAY_SEND);
	wb_mmr_write32(SPI_TWC(s->n), (uint32_t)(n / wb_spi_word_bytes(s)));
	wb_mmr_write32(SPI_TXCTL(s->n), BRIDGE_TXCTL);
	wb_mmr_write32(SPI_CTL(s->n), ctl | SPI_CTL_EN);	/* EN last */
	wb_mmr_sync();
	rc = dma_wait(s, ch, SPI_TWC(s->n), 0u, NULL, NULL, &stage);
	if (rc != WB_SPI_OK)
		return master_fail(s, rc, stage, false, ctl, rung != NULL);
	wb_mmr_write32(DMA_STAT(ch), DMA_STAT_IRQDONE);
	wb_mmr_write32(SPI_IMSK_SET(s->n), SPI_STAT_TF);
	wb_mmr_sync();
	if (tf_wait(s) != WB_SPI_OK)
		return master_fail(s, WB_SPI_TIMEOUT, WB_SPI_STAGE_TX_END, false,
				   ctl, rung != NULL);
	stat = wb_mmr_read32(SPI_STAT(s->n));
	wb_mmr_write32(SPI_IMSK_CLR(s->n), SPI_STAT_TF);
	wb_mmr_write32(SPI_STAT(s->n), SPI_STAT_TF);
	wb_mmr_write32(SPI_ILAT_CLR(s->n), SPI_STAT_TF);
	wb_mmr_write32(SPI_TXCTL(s->n), BRIDGE_TXCTL & ~SPI_XCTL_EN);
	wb_mmr_write32(SPI_CTL(s->n), ctl);	/* EN cleared, MIOM kept */
	wb_dma_stop(ch);
	if (rung != NULL) {
		wb_mmr_write32(SPI_CTL(s->n), 0u);
		wb_mmr_write32(SPI_CLK(s->n), last_clk[s->n]);
	}
	wb_mmr_sync();
	if (stat & SPI_STAT_MF)
		return fault_note(s->n, WB_SPI_ERR_HW, stat, 0u,
				  WB_SPI_STAGE_OVERRUN);
	return WB_SPI_OK;
}

int wb_spi_dma_recv(const struct wb_spi *s, uint8_t *buf, size_t n)
{
	const unsigned ch = WB_DMA_SPI0_RX + 2u * s->n;
	struct wb_spi_array *a = &wb_spi_rx_array;
	uint32_t stage = WB_SPI_STAGE_NONE, stat;
	int rc;

	if (!counted_ok(s, n) || n >= WB_SPI_BRIDGE_MAX)
		return refused(s);
	arr_reset(a);
	if (!arr_recv(a, buf, n))
		return refused(s);
	arr_done(a);
	if (!rx_open[s->n]) {
		/* the session's first part: EN clear, EN last */
		fresh[s->n] = true;
		fault_open(s->n);
		wb_mmr_write32(SPI_CTL(s->n), s->ctl);
		wb_mmr_write32(SPI_TXCTL(s->n), 0u);	/* the transmit channel off */
		wb_mmr_write32(SPI_STAT(s->n), SPI_STAT_W1C);
		wb_mmr_write32(DMA_STAT(ch), DMA_W1C);
		wb_dma_array_start(ch, a->d, WB_DMA_ARRAY_RECV);
		wb_mmr_write32(SPI_RWC(s->n), (uint32_t)(n / wb_spi_word_bytes(s)));
		wb_mmr_write32(SPI_RXCTL(s->n), MASTER_RXCTL);
		wb_mmr_write32(SPI_CTL(s->n), s->ctl | SPI_CTL_EN);	/* EN last */
		rx_open[s->n] = true;
	} else {
		/* a later part (S-13b.040): SPI_CTL untouched */
		wb_dma_array_start(ch, a->d, WB_DMA_ARRAY_RECV);
		wb_mmr_write32(SPI_RWC(s->n), (uint32_t)(n / wb_spi_word_bytes(s)));
		wb_mmr_write32(SPI_RXCTL(s->n), MASTER_RXCTL);
	}
	wb_mmr_sync();
	rc = dma_wait(s, ch, SPI_RWC(s->n), 0u, NULL, NULL, &stage);
	if (rc != WB_SPI_OK)
		return master_fail(s, rc, stage == WB_SPI_STAGE_DMA_STALL ?
				   WB_SPI_STAGE_RX_WORD : stage, true, s->ctl, false);
	wb_mmr_write32(DMA_STAT(ch), DMA_STAT_IRQDONE);
	wb_mmr_write32(SPI_RXCTL(s->n), MASTER_RXCTL & ~SPI_XCTL_EN);
	wb_mmr_sync();
	arr_copy(a);
	stat = wb_mmr_read32(SPI_STAT(s->n));
	if (stat & (SPI_STAT_ROR | SPI_STAT_MF))
		return master_fail(s, WB_SPI_ERR_HW, WB_SPI_STAGE_OVERRUN, true,
				   s->ctl, false);
	return WB_SPI_OK;
}

void wb_spi_dma_close(const struct wb_spi *s)
{
	if (!rx_open[s->n])
		return;
	wb_mmr_write32(SPI_CTL(s->n), s->ctl);	/* EN cleared */
	wb_dma_stop(WB_DMA_SPI0_RX + 2u * s->n);
	wb_mmr_sync();
	rx_open[s->n] = false;
}

#if WB_SPI_BOOT_DMA
/* How long a failed part's controller is given to clock what its FIFO holds,
 * and how long its count must then stay still. */
#define BOOT_DRAIN_US 100u
#define BOOT_STILL_US 10u

/*
 * A part given up: recorded, nothing more fed, the controller left to clock
 * what its FIFO holds while the ready line lets it (SPI_TWC and the FIFO's
 * level still for BOOT_STILL_US, or BOOT_DRAIN_US in all), then the words not
 * clocked read from SPI_TWC (HRM, SPI_TWC: the words remaining to be
 * transmitted), the transmit channel closed and the status cleared. EN stays
 * set: the select is the controller's.
 */
static uint32_t boot_part_fail(const struct wb_spi *s, int rc, uint32_t stage)
{
	const unsigned n = s->n, ch = WB_DMA_SPI0_TX + 2u * n;
	const uint64_t still = (uint64_t)BOOT_STILL_US * s->t->cycles_per_us;
	const uint64_t from = wb_core_cycles();
	const uint64_t bound = (uint64_t)BOOT_DRAIN_US * s->t->cycles_per_us;
	uint64_t moved = from;
	uint32_t was = UINT32_MAX, left;

	(void)fault_record(n, rc, wb_mmr_read32(SPI_STAT(n)), wb_mmr_read32(SPI_TWC(n)),
			   stage, false);
	wb_dma_stop_idle(ch, s->t);
	for (;;) {
		const uint64_t t = wb_core_cycles();
		const uint32_t v = (wb_mmr_read32(SPI_TWC(n)) & 0xFFFFu) |
				   (wb_mmr_read32(SPI_STAT(n)) & SPI_STAT_TFS_MASK);

		if (v != was) {
			was = v;
			moved = t;
		} else if (t - moved >= still || t - from >= bound) {
			break;
		}
	}
	left = wb_mmr_read32(SPI_TWC(n)) & 0xFFFFu;
	wb_mmr_write32(SPI_TXCTL(n), 0u);
	wb_mmr_write32(SPI_STAT(n), SPI_STAT_W1C);
	wb_mmr_sync();
	return left;
}

void wb_spi_boot_fcs(struct wb_spi_fcs *out, bool clear)
{
	*out = boot_fcs;
	if (clear)
		boot_fcs = (struct wb_spi_fcs){ 0 };
}

int wb_spi_dma_send_bytes(const struct wb_spi *s, const uint8_t *buf, size_t n,
			  uint32_t *left)
{
	const unsigned ch = WB_DMA_SPI0_TX + 2u * s->n;
	struct wb_spi_array *a = &wb_spi_tx_array;
	uint32_t stage = WB_SPI_STAGE_NONE;
	uint64_t end;
	int rc;

	*left = (uint32_t)n;
	if (wb_spi_word_bytes(s) != 1u || !(s->ctl & SPI_CTL_MSTR) ||
	    (s->ctl & SPI_CTL_MIOM_MASK) || !counted_ok(s, n))
		return refused(s);
	arr_reset(a);
	if (!arr_put(a, WB_DMA_ARRAY_SEND8, (uint8_t *)(uintptr_t)buf, NULL, n, false))
		return refused(s);
	arr_done(a);
	wb_dma_cache_send((uint32_t)(uintptr_t)buf, (uint32_t)n);
	fault_open(s->n);
	boot_fcs_on = true;
	wb_mmr_write32(SPI_TXCTL(s->n), 0u);
	wb_mmr_write32(SPI_RXCTL(s->n), 0u);
	wb_mmr_write32(SPI_STAT(s->n), SPI_STAT_W1C);
	wb_mmr_write32(DMA_STAT(ch), DMA_W1C);
	wb_dma_array_start(ch, a->d, WB_DMA_ARRAY_SEND8);
	wb_mmr_write32(SPI_TWC(s->n), (uint32_t)n);
	wb_mmr_write32(SPI_TXCTL(s->n), WB_SPI_BOOT_TXCTL);
	wb_mmr_write32(SPI_TXCTL(s->n), WB_SPI_BOOT_TXCTL | SPI_XCTL_EN);	/* TEN last */
	wb_mmr_sync();
	rc = dma_wait(s, ch, SPI_TWC(s->n), 0u, NULL, NULL, &stage);
	if (rc != WB_SPI_OK) {
		boot_fcs_on = false;
		*left = boot_part_fail(s, rc, stage);
		return rc;
	}
	wb_mmr_write32(DMA_STAT(ch), DMA_STAT_IRQDONE);
	rc = tx_end(s);
	boot_fcs_on = false;
	if (rc != WB_SPI_OK) {
		*left = boot_part_fail(s, rc, WB_SPI_STAGE_TX_END);
		return rc;
	}
	/* TF may come as the last byte starts: four bytes' time at SCLK0 / 3
	 * before the select may rise */
	end = wb_core_cycles() + s->t->cycles_per_us;
	while (wb_core_cycles() < end)
		;
	wb_mmr_write32(SPI_TXCTL(s->n), 0u);
	wb_mmr_write32(SPI_STAT(s->n), SPI_STAT_W1C);
	wb_dma_stop(ch);
	wb_mmr_sync();
	*left = 0u;
	return WB_SPI_OK;
}
#endif

/* S-13b.040: EN cleared with the rest of SPI_CTL kept, MIOM above all, so
 * that the next write, which may change MIOM, finds EN clear. */
static void en_off(const struct wb_spi *p)
{
	const uint32_t ctl = wb_mmr_read32(SPI_CTL(p->n));

	if (ctl & SPI_CTL_EN)
		wb_mmr_write32(SPI_CTL(p->n), ctl & ~SPI_CTL_EN);
}

/*
 * S-13b.044: a session's flow control follows its direction. A reply
 * (the slave transmits) holds the master on the transmit FIFO, ready
 * deasserted while it is empty (FCCH 1, FCWM 0); a request on the receive
 * FIFO, deasserted from half full (FCCH 0, FCWM 2). Written between
 * sessions (PB15 high) with the controller disabled, which also empties its
 * FIFOs.
 */
static uint32_t dir_ctl(const struct wb_spi *p, bool reply)
{
	uint32_t ctl = p->ctl & ~(SPI_CTL_FCCH | SPI_CTL_FCWM_MASK);

	/* The test OS's USB monitor can choose a set-up to try (wiki/Test-OS.md). */
	return ctl | (reply ? OPXY_TESTOS_OR((testos_reply_mode == 0u || (testos_reply_mode & OPXY_TESTOS_REPLY_FCCH)) ? SPI_CTL_FCCH | SPI_CTL_FCWM_TX_EMPTY : 0u,
					     SPI_CTL_FCCH | SPI_CTL_FCWM_TX_EMPTY) :
			      SPI_CTL_FCWM_RX_HALF);
}

#if defined(OPXY_TEST_OS)
/* The test OS's measured replies set SPI2 up in their own order. */
static void session_ctl(const struct wb_spi_slave *s, bool reply,
			unsigned flags)
{
	const struct wb_spi *p = &s->spi;

	if (!(flags & WB_SPI_SLAVE_SESSION))
		return;
	en_off(p);
	wb_mmr_write32(SPI_CTL(p->n), dir_ctl(p, reply));
	wb_mmr_write32(SPI_CTL(p->n), dir_ctl(p, reply) | SPI_CTL_EN);
}
#endif

/* S-13b.043: the ready pin goes to the controller once the transfer is
 * set up and the master-ready pin last read high. A session's first part
 * handed over clears the controller's fault record. */
static int hand_over(const struct wb_spi_slave *s, unsigned flags,
		     uint32_t start_us)
{
	if (flags & WB_SPI_SLAVE_SESSION) {
		uint64_t end = wb_time_deadline(s->spi.t, start_us);

		while (!wb_gpio_get(s->master_pin))
			if (wb_time_passed(end))
				return WB_SPI_TIMEOUT;
		fault[s->spi.n] = (struct wb_spi_fault){ 0 };
	}
	(void)wb_gpio_peripheral(s->ready_pin);
	wb_mmr_sync();
	return WB_SPI_OK;
}

/*
 * A session's end on the slave: the ready pin back (high), a reply's TF
 * masked and its latch cleared, the session's DMA channel stopped with its
 * status acknowledged, both controller channels off, the status cleared. A failure is recorded with `stage` and `left`
 * and SPI_STAT as read here, PA4 just taken back, and also clears EN (MIOM
 * kept), which empties both FIFOs; the next set-up enables SPI2 again. A
 * reply's receive channel is not part of its end (S-13b.045), so only a
 * request fails on a receive overrun.
 */
static int slave_fail(const struct wb_spi_slave *s, int rc, bool reply,
		      uint32_t stage, uint32_t left)
{
	const struct wb_spi *p = &s->spi;
	uint32_t stat;

	(void)wb_gpio_gpio(s->ready_pin);
	wb_mmr_sync();
	stat = wb_mmr_read32(SPI_STAT(p->n));
	if (reply) {
		wb_mmr_write32(SPI_IMSK_CLR(p->n), SPI_STAT_TF);
		wb_mmr_write32(SPI_ILAT_CLR(p->n), SPI_STAT_TF);
	}
	wb_mmr_write32(SPI_RXCTL(p->n), 0);
	wb_mmr_write32(SPI_TXCTL(p->n), 0);
	wb_dma_stop_idle((reply ? WB_DMA_SPI0_TX : WB_DMA_SPI0_RX) + 2u * p->n,
			 p->t);
	wb_mmr_write32(SPI_STAT(p->n), stat & SPI_STAT_W1C);
	if (rc == WB_SPI_OK && !reply && (stat & SPI_STAT_ROR)) {
		rc = WB_SPI_ERR_HW;
		stage = WB_SPI_STAGE_OVERRUN;
	}
	if (stage != WB_SPI_STAGE_NONE)
		(void)fault_record(p->n, rc, stat, left, stage, !reply);
	if (rc != WB_SPI_OK)
		wb_mmr_write32(SPI_CTL(p->n),
			       wb_mmr_read32(SPI_CTL(p->n)) & ~SPI_CTL_EN);
	wb_mmr_sync();
	return rc;
}

static int slave_end(const struct wb_spi_slave *s, int rc, bool reply)
{
	return slave_fail(s, rc, reply, WB_SPI_STAGE_NONE, 0u);
}

/* The listen's buffer for the request's first part, and its length; and
 * DMA9's current address and count as the listen left it, which move with
 * the first word the channel takes (wb_spi_slave_heard()). */
static uint8_t *listen_rx[WB_SPI_COUNT];
static size_t listen_head[WB_SPI_COUNT];
static uint32_t listen_at[WB_SPI_COUNT][2];

/*
 * S-13b.083's request set-up, ctl the request's SPI_CTL without EN: SPI2
 * disabled (which empties its FIFOs), the transmit channel off, then DMA9
 * disabled, the receive-finished interrupt masked, DMA9 given the first
 * part's array and enabled, SPI2_RXCTL with no word count, and EN last. The
 * test OS may ask for EN first (S-13b Q-11, (1g)).
 */
static void request_setup(const struct wb_spi *p, uint32_t ctl, bool en_first)
{
	const unsigned ch = WB_DMA_SPI0_RX + 2u * p->n;

	en_off(p);
	wb_mmr_write32(SPI_CTL(p->n), ctl);	/* EN clear: the write that may change MIOM */
	if (en_first)
		wb_mmr_write32(SPI_CTL(p->n), ctl | SPI_CTL_EN);
	begin(p);
	wb_mmr_write32(SPI_TXCTL(p->n), 0u);	/* only the receive channel */
	wb_dma_stop_idle(ch, p->t);
	wb_mmr_write32(SPI_IMSK_CLR(p->n), SPI_STAT_RF);
	wb_dma_array_start(ch, wb_spi_rx_array.d, WB_DMA_ARRAY_RECV);
	wb_mmr_write32(SPI_RXCTL(p->n), SLAVE_RXCTL);
	if (!en_first)
		wb_mmr_write32(SPI_CTL(p->n), ctl | SPI_CTL_EN);	/* EN last */
	wb_mmr_sync();
}

/* DMA9's current address and count before the master can clock a word:
 * the channel has fetched its descriptor by now, and only a word taken
 * moves either (wb_spi_slave_heard()). */
static void listen_mark(const struct wb_spi *p)
{
	const unsigned ch = WB_DMA_SPI0_RX + 2u * p->n;

	listen_at[p->n][0] = wb_mmr_read32(DMA_ADDR_CUR(ch));
	listen_at[p->n][1] = wb_mmr_read32(DMA_XCNT_CUR(ch));
}

/* The first part's array: rx's head bytes. */
static bool head_array(const struct wb_spi *p, uint8_t *rx, size_t head)
{
	if (!words_ok(p, head) || head >= WB_SPI_BRIDGE_MAX)
		return false;
	arr_reset(&wb_spi_rx_array);
	if (!arr_recv(&wb_spi_rx_array, rx, head))
		return false;
	arr_done(&wb_spi_rx_array);
	listen_rx[p->n] = rx;
	listen_head[p->n] = head;
	return true;
}

#if defined(OPXY_TEST_OS)
#include "../../testos/spi_slave_listen.inc"
#endif

int wb_spi_slave_listen(const struct wb_spi_slave *s, uint8_t *rx, size_t head,
			bool quad, uint32_t start_us)
{
	const struct wb_spi *p = &s->spi;
	int rc;

	if (!head_array(p, rx, head))
		return refused(p);
	OPXY_TESTOS(testos_armed_listen = false; if ((wb_spi_testos_req & 0xFF00u) != 0u) return testos_listen(s, start_us);)
	request_setup(p, dir_ctl(p, false) | (quad ? SPI_CTL_MIOM_QUAD : 0u), false);
	listen_mark(p);
	rc = hand_over(s, WB_SPI_SLAVE_SESSION, start_us);
	return rc == WB_SPI_OK ? rc :
	       slave_fail(s, rc, false, WB_SPI_STAGE_MASTER_READY, 0u);
}

void wb_spi_slave_unlisten(const struct wb_spi_slave *s)
{
	listen_rx[s->spi.n] = NULL;
	(void)slave_end(s, WB_SPI_TIMEOUT, false);
}

/* A word in: the receive FIFO not empty, the channel's FIFO holding one,
 * its done or error status, or its current address or count moved since
 * the listen (a word the channel took on to memory). */
bool wb_spi_slave_heard(const struct wb_spi_slave *s)
{
	const unsigned n = s->spi.n, ch = WB_DMA_SPI0_RX + 2u * n;

	return (wb_mmr_read32(DMA_STAT(ch)) &
		(DMA_STAT_IRQDONE | DMA_STAT_IRQERR | DMA_STAT_FIFOFILL_MASK)) != 0u ||
	       !(wb_mmr_read32(SPI_STAT(n)) & SPI_STAT_RFE) ||
	       wb_mmr_read32(DMA_ADDR_CUR(ch)) != listen_at[n][0] ||
	       wb_mmr_read32(DMA_XCNT_CUR(ch)) != listen_at[n][1];
}

/* The bytes still to come of the part under way: the current descriptor's
 * count, as the DMA has it. */
static uint32_t dma_left(unsigned ch)
{
	return wb_mmr_read32(DMA_XCNT_CUR(ch));
}

/*
 * After the request's last part: the ready pin taken back, then EN cleared
 * (MIOM kept), then REN, and DMA9 disabled; then the parts' separate
 * buffers copied out. A receive overrun fails the request.
 */
static int request_end(const struct wb_spi_slave *s)
{
	const struct wb_spi *p = &s->spi;
	uint32_t stat;

	(void)wb_gpio_gpio(s->ready_pin);
	wb_mmr_write32(SPI_CTL(p->n), wb_mmr_read32(SPI_CTL(p->n)) & ~SPI_CTL_EN);
	wb_mmr_write32(SPI_RXCTL(p->n), SLAVE_RXCTL & ~SPI_XCTL_EN);
	wb_dma_stop(WB_DMA_SPI0_RX + 2u * p->n);
	stat = wb_mmr_read32(SPI_STAT(p->n));
	wb_mmr_write32(SPI_STAT(p->n), stat & SPI_STAT_W1C);
	wb_mmr_sync();
	if (stat & SPI_STAT_ROR)
		return fault_record(p->n, WB_SPI_ERR_HW, stat, 0u,
				    WB_SPI_STAGE_OVERRUN, true);
	return WB_SPI_OK;
}

int wb_spi_slave_request(const struct wb_spi_slave *s, size_t room,
			 wb_spi_session_len len, void *ctx, size_t *kept,
			 uint32_t start_us)
{
	const struct wb_spi *p = &s->spi;
	const unsigned ch = WB_DMA_SPI0_RX + 2u * p->n;
	struct wb_spi_array *a = &wb_spi_rx_array;
	uint8_t *const rx = listen_rx[p->n];
	const size_t head = listen_head[p->n];
	uint32_t stage = WB_SPI_STAGE_NONE, tail = 0u;
	uint64_t total, drop;
	size_t keep;
	bool ended = false, first = true;
	int rc;

	*kept = 0;
	listen_rx[p->n] = NULL;			/* one request per listen */
	if (rx == NULL || len == NULL || room < head ||
	    room % wb_spi_word_bytes(p) != 0u)
		return slave_fail(s, WB_SPI_ERR_ARG, false, WB_SPI_STAGE_ARG, 0u);
	/* the first part: the header */
	rc = dma_wait(p, ch, 0u, (uint64_t)start_us * p->t->cycles_per_us, s,
		      &ended, &stage);
	if (rc != WB_SPI_OK)
		return slave_fail(s, rc, false,
				  stage == WB_SPI_STAGE_DMA_STALL ?
				  WB_SPI_STAGE_FIRST_WORD : stage, dma_left(ch));
	wb_mmr_write32(DMA_STAT(ch), DMA_STAT_IRQDONE);
	arr_copy(a);
	*kept = head;
	/* the master is held by the ready line (S-13b.044) while this runs */
	total = len(ctx, rx, &tail);
	if (total < head || total % wb_spi_word_bytes(p) != 0u ||
	    tail % wb_spi_word_bytes(p) != 0u)
		return slave_fail(s, WB_SPI_ERR_ARG, false, WB_SPI_STAGE_REFUSED,
				  0u);
	if (total == head && tail == 0u)
		return request_end(s);
	/* the rest: what is kept, what is dropped (S-09.037), then the
	 * further bytes (S-09.021), each in descriptors of their own, in
	 * parts of less than 0x40000 bytes (S-13b.083): a longer discarded
	 * payload is taken part after part into the one dropped buffer, the
	 * master held by the ready line between them (S-13b.044). The
	 * further bytes are never split between two parts. */
	keep = total < room ? (size_t)total : room;
	drop = total - keep;
	if (keep - head > SLAVE_PART_MAX || tail > SLAVE_PART_MAX)
		return slave_fail(s, WB_SPI_ERR_ARG, false, WB_SPI_STAGE_REFUSED,
				  0u);
	for (;;) {
		uint64_t space = SLAVE_PART_MAX, d;
		uint32_t t = 0u;
		unsigned tail_at;

		arr_reset(a);
		if (first && !arr_recv(a, rx + head, keep - head))
			return slave_fail(s, WB_SPI_ERR_ARG, false,
					  WB_SPI_STAGE_REFUSED, 0u);
		if (first)
			space -= keep - head;
		d = drop < space ? drop : space;
		if (!arr_drop(a, d))
			return slave_fail(s, WB_SPI_ERR_ARG, false,
					  WB_SPI_STAGE_REFUSED, 0u);
		drop -= d;
		space -= d;
		tail_at = a->n;
		if (drop == 0u && tail <= space) {
			t = tail;
			tail = 0u;
			if (!arr_drop(a, t))
				return slave_fail(s, WB_SPI_ERR_ARG, false,
						  WB_SPI_STAGE_REFUSED, 0u);
		}
		arr_done(a);
		wb_dma_array_start(ch, a->d, WB_DMA_ARRAY_RECV);	/* nothing else */
		rc = dma_wait(p, ch, 0u, 0u, s, &ended, &stage);
		/* S-09.021: a session the master ended in the further bytes,
		 * the channel at their descriptor, carries the whole request */
		if (rc != WB_SPI_OK && ended && t != 0u &&
		    wb_mmr_read32(DMA_DSCPTR_CUR(ch)) >
			    (uint32_t)(uintptr_t)&a->d[tail_at])
			rc = WB_SPI_OK;
		if (rc != WB_SPI_OK)
			return slave_fail(s, rc, false,
					  stage == WB_SPI_STAGE_DMA_STALL ?
					  WB_SPI_STAGE_NEXT_WORD : stage, dma_left(ch));
		wb_mmr_write32(DMA_STAT(ch), DMA_STAT_IRQDONE);
		if (first) {
			arr_copy(a);
			*kept = keep;
			first = false;
		}
		if (drop == 0u && tail == 0u)
			return request_end(s);
	}
}

#if defined(OPXY_TEST_OS)
#include "../../testos/spi_slave_q11.inc"
#endif

/*
 * S-13b.044's reply, fed by DMA8 (S-13b.083): flow control on the transmit
 * FIFO, this TXCTL, two words queued behind the last, which SPI_TWC does not
 * count and the master never clocks, so the FIFO does not run empty while
 * the last word goes out.
 *
 * S-13b.083 gives a single-I/O reply two ends. The TF end: REN clear, so
 * nothing is received; SPI_IMSK.TF unmasked once DMA8's done status has been
 * seen, and the end SPI_ILAT.TF, read here in place of a handler (S-13b.083
 * fixes the registers and their order, not how the events are taken). Quad
 * I/O (the full quad option, off) ends the same way. The count end
 * (S-13b.045): what comes in counts the words that went out, and a received
 * word reaches the FIFO at its end; the core reads them, two at once when
 * the receive FIFO is full, so that none is lost. One lost all the same
 * (SPI_STAT.ROR) leaves the count short of the end, and the reply then ends
 * from the transmit side: SPI_TWC 0 (the last word has left the transmit
 * FIFO) and no word received for SPI_REPLY_QUIET_US.
 *
 * Either end: TUR or TC before the end gives the session up at once
 * (S-09.013), and so does PB15 reading high once it has read low. No give-up
 * on time alone within the master's T_REPLY: the slave's own bound is twice
 * its word bound, from the last thing moved. SPI_TWC and the DMA move as
 * DMA8 fills the transmit FIFO, before any clock, so only PB15 low or a
 * received word says that the master has begun.
 */
struct reply_mark {
	uint32_t twc, addr, tfs;
};

/* Anything of the reply moved since the last look: SPI_TWC, and with no
 * received word to count (tf), DMA8's current address and the transmit
 * FIFO's level. */
static bool reply_moved(const struct wb_spi *p, unsigned ch, uint32_t stat,
			bool tf, struct reply_mark *k)
{
	const struct reply_mark now = {
		wb_mmr_read32(SPI_TWC(p->n)),
		tf ? wb_mmr_read32(DMA_ADDR_CUR(ch)) : 0u,
		tf ? stat & SPI_STAT_TFS_MASK : 0u,
	};
	const bool moved = now.twc != k->twc || now.addr != k->addr ||
			   now.tfs != k->tfs;

	*k = now;
	return moved;
}

/* S-13b.083, the TF end: the ready pin back first, then TF masked,
 * TEN cleared, TF acknowledged and DMA8 disabled; EN stays set. */
static int reply_tf_end(const struct wb_spi_slave *s, unsigned ch)
{
	const struct wb_spi *p = &s->spi;

	(void)wb_gpio_gpio(s->ready_pin);
	wb_mmr_sync();
	wb_mmr_write32(SPI_IMSK_CLR(p->n), SPI_STAT_TF);
	wb_mmr_write32(SPI_TXCTL(p->n), SPI_TXCTL_REPLY & ~SPI_XCTL_EN);
	wb_mmr_write32(SPI_STAT(p->n), SPI_STAT_TF);
	wb_mmr_write32(SPI_ILAT_CLR(p->n), SPI_STAT_TF);
	wb_dma_stop(ch);
	wb_mmr_sync();
	return WB_SPI_OK;
}

int wb_spi_slave_send(const struct wb_spi_slave *s, const uint8_t *tx,
		      size_t n, unsigned flags, uint32_t start_us)
{
	const struct wb_spi *p = &s->spi;
	const unsigned ch = WB_DMA_SPI0_TX + 2u * p->n;
	struct wb_spi_array *a = &wb_spi_tx_array;
	const unsigned w = wb_spi_word_bytes(p);
	const bool multi = (p->ctl & SPI_CTL_MIOM_MASK) != 0;
	const bool tf_end = multi || reply_end[p->n] != WB_SPI_END_COUNT;
	bool started = false, overrun = false, done = false;
	size_t in = 0;
	uint32_t stage = WB_SPI_STAGE_NONE;
	struct reply_mark mark = { (uint32_t)(n / w), 0u, 0u };
	uint64_t last, limit;
	const uint64_t bound = 2u * word_cycles(p);
	const uint64_t quiet = (uint64_t)SPI_REPLY_QUIET_US * p->t->cycles_per_us;
	int rc;

	if (!words_ok(p, n) || n / w > SPI_TWC_MAX ||
	    n + sizeof queued >= WB_SPI_BRIDGE_MAX)
		return refused(p);
	OPXY_TESTOS(if (testos_taken(s, tx, n, flags, start_us, &rc)) return rc;)
	arr_reset(a);
	if (!arr_put(a, WB_DMA_ARRAY_SEND, (uint8_t *)(uintptr_t)tx, NULL, n,
		     false) ||
	    !arr_put(a, WB_DMA_ARRAY_SEND, (uint8_t *)queued, NULL,
		     sizeof queued, false))
		return refused(p);
	arr_done(a);
	wb_dma_cache_send((uint32_t)(uintptr_t)tx, (uint32_t)n);
	/* S-13b.044's order, with EN clear (which also empties both FIFOs) */
	if (flags & WB_SPI_SLAVE_SESSION) {
		en_off(p);
		wb_mmr_write32(SPI_CTL(p->n), dir_ctl(p, true));
	}
	fault_open(p->n);
	wb_mmr_write32(SPI_IMSK_CLR(p->n), SPI_STAT_TF);	/* TF masked until DMA8 is done */
	wb_mmr_write32(SPI_ILAT_CLR(p->n), SPI_STAT_TF);
	wb_mmr_write32(SPI_STAT(p->n), 0xFFFFFFFFu);		/* 1 */
	wb_mmr_write32(DMA_STAT(ch), DMA_W1C);			/* 2 */
	wb_dma_array_start(ch, a->d, WB_DMA_ARRAY_SEND);
	wb_mmr_write32(SPI_TWC(p->n), (uint32_t)(n / w));	/* 3 */
	wb_mmr_write32(SPI_TXCTL(p->n), SPI_TXCTL_REPLY);	/* 4 */
	/* 5: REN set for the count end only; quad has no receive channel */
	wb_mmr_write32(SPI_RXCTL(p->n), multi ? 0u :
			       tf_end ? SLAVE_RXCTL & ~SPI_XCTL_EN : SPI_XCTL_EN);
	wb_mmr_write32(SPI_CTL(p->n), dir_ctl(p, true) | SPI_CTL_EN);	/* 6 */
	wb_mmr_sync();
	rc = hand_over(s, flags, start_us);
	if (rc != WB_SPI_OK)
		stage = WB_SPI_STAGE_MASTER_READY;
	last = wb_core_cycles();
	limit = (uint64_t)start_us * p->t->cycles_per_us;
	while (rc == WB_SPI_OK) {
		const uint32_t stat = wb_mmr_read32(SPI_STAT(p->n));
		bool moved;

		if (stat & (SPI_STAT_TUR | SPI_STAT_TC)) {
			rc = WB_SPI_ERR_HW;	/* S-13b.045: given up */
			stage = WB_SPI_STAGE_TX_ERR;
			break;
		}
		if (!done) {
			const uint32_t dstat = wb_mmr_read32(DMA_STAT(ch));

			if (dstat & DMA_STAT_IRQERR) {
				rc = WB_SPI_ERR_HW;
				stage = WB_SPI_STAGE_DMA_ERR |
					((dstat & DMA_STAT_ERRC_MASK) >> DMA_STAT_ERRC_SHIFT)
						<< WB_SPI_STAGE_ERRC_SHIFT;
				break;
			}
			if (dstat & DMA_STAT_IRQDONE) {
				done = true;
				if (tf_end) {
					/* S-13b.044: TF unmasked here, and nowhere else */
					wb_mmr_write32(DMA_STAT(ch), DMA_STAT_IRQDONE);
					wb_mmr_write32(SPI_IMSK_SET(p->n), SPI_STAT_TF);
					wb_mmr_sync();
				}
			}
		} else if (tf_end && (wb_mmr_read32(SPI_ILAT(p->n)) & SPI_STAT_TF)) {
			return reply_tf_end(s, ch);
		}
		moved = reply_moved(p, ch, stat, tf_end, &mark);
		if (!tf_end && !(stat & SPI_STAT_RFE)) {
			(void)wb_mmr_read32(SPI_RFIFO(p->n));
			in += w;
			if ((stat & SPI_STAT_RFS_MASK) == SPI_STAT_RFS_FULL) {
				(void)wb_mmr_read32(SPI_RFIFO(p->n));
				in += w;
			}
			if (in >= n)
				break;
			moved = started = true;	/* a word came: the master began */
		}
		if (!tf_end && (stat & SPI_STAT_ROR))
			overrun = true;
		if (moved) {
			/* the bound runs from the last thing moved, twice the
			 * word bound once the master has begun */
			if (!started && !wb_gpio_get(s->master_pin))
				started = true;
			last = wb_core_cycles();
			if (started)
				limit = bound;
			continue;
		}
		/* a received word lost: the end from the transmit side, the
		 * last word gone from the transmit FIFO and none received for
		 * a quiet time */
		if (overrun && mark.twc == 0u && wb_core_cycles() - last >= quiet) {
			(void)fault_note(p->n, WB_SPI_ERR_HW, stat,
					 (uint32_t)(in / w) << 16 | (uint32_t)(n / w),
					 WB_SPI_STAGE_REPLY_OVERRUN);
			return slave_fail(s, WB_SPI_OK, true, WB_SPI_STAGE_NONE, 0u);
		}
		/* S-09.013: PB15 high after it was low ends the session */
		if (!wb_gpio_get(s->master_pin)) {
			started = true;
		} else if (started) {
			rc = WB_SPI_TIMEOUT;
			stage = WB_SPI_STAGE_MASTER_END;
		}
		if (rc == WB_SPI_OK && wb_core_cycles() - last >= limit) {
			rc = WB_SPI_TIMEOUT;
			stage = WB_SPI_STAGE_REPLY_BOUND;
		}
	}
	/* S-13b.083: on the TF end, a reply with every word fed (DMA8
	 * done) and no TF, given up because the master ended the session
	 * (PB15 high) or the bound ran out, whichever came first, changes the
	 * rest of the run to the count end, once. With this bootloader's
	 * master it is the master's T_REPLY that comes first. */
	if (tf_end && !multi && done && rc == WB_SPI_TIMEOUT &&
	    (stage == WB_SPI_STAGE_REPLY_BOUND || stage == WB_SPI_STAGE_MASTER_END)) {
		reply_end[p->n] = (uint8_t)WB_SPI_END_COUNT;
		reply_changed[p->n] = 1u;
	}
	/* the words the master has not clocked: counted on the count end, the
	 * transmit count else (S-13b.045) */
	return slave_fail(s, rc, true, stage,
			  rc == WB_SPI_OK ? 0u :
			  tf_end ? wb_mmr_read32(SPI_TWC(p->n)) :
				   (uint32_t)((n - in) / w));
}

/* ---- a developer's quad reply test (S-09.080, S-13b.084) ---- */

static uint32_t qt_since(uint64_t from, uint64_t now)
{
	return (uint32_t)(now - from);
}

/* The core feed: words while the transmit FIFO has room, then the two
 * uncounted ones (zeros). True once every word has been written. */
static bool qt_feed(const struct wb_spi *p, const uint8_t *tx, size_t n,
		    size_t *out, unsigned *pads)
{
	while ((*out < n || *pads != 0u) &&
	       !(wb_mmr_read32(SPI_STAT(p->n)) & SPI_STAT_TFF)) {
		if (*out < n) {
			wb_mmr_write32(SPI_TFIFO(p->n), get_word(tx + *out, 4u));
			*out += 4u;
		} else {
			wb_mmr_write32(SPI_TFIFO(p->n), 0u);
			(*pads)--;
		}
	}
	return *out == n && *pads == 0u;
}

/* SPIF's falls and rises between two reads of SPI_STAT; for the SPIF
 * candidate (times) its last fall and rise are the feed's and the flag's. */
static void qt_spif(struct wb_spi_qt *q, uint32_t was, uint32_t stat,
		    uint32_t at, bool times)
{
	if (!((was ^ stat) & SPI_STAT_SPIF))
		return;
	if (stat & SPI_STAT_SPIF) {
		q->spif += 1u << 16;
		if (times)
			q->t_flag = at;
	} else {
		q->spif += 1u;
		if (times)
			q->t_feed = at;
	}
}

/* The TF candidates: the feed, TF unmasked once it is whole, the end
 * SPI_ILAT.TF; with REN each received word read and counted. The bound
 * runs from the master's first word, and again from the last feed, so a
 * slow core feed is not taken for a missing flag. */
static uint32_t qt_tf_loop(const struct wb_spi_slave *s, const uint8_t *tx,
			   size_t n, bool dma, bool ren, uint64_t hand,
			   uint64_t start, uint64_t flag, size_t *out,
			   unsigned *pads, struct wb_spi_qt *q)
{
	const struct wb_spi *p = &s->spi;
	const unsigned ch = WB_DMA_SPI0_TX + 2u * p->n;
	uint32_t was = wb_mmr_read32(SPI_STAT(p->n));
	uint64_t began = 0;
	bool begun = false;

	for (;;) {
		const uint32_t stat = wb_mmr_read32(SPI_STAT(p->n));
		const uint64_t now = wb_core_cycles();
		const uint32_t at = qt_since(hand, now);

		q->stat_or |= stat;
		qt_spif(q, was, stat, at, false);
		was = stat;
		if (stat & SPI_STAT_TF)
			q->end |= WB_SPI_QF_TF_STAT;
		if (stat & (SPI_STAT_TUR | SPI_STAT_TC))
			return WB_SPI_QE_TX_ERR;
		if (ren && !(stat & SPI_STAT_RFE)) {
			/* two at once when the receive FIFO is full */
			(void)wb_mmr_read32(SPI_RFIFO(p->n));
			q->rx++;
			if ((stat & SPI_STAT_RFS_MASK) == SPI_STAT_RFS_FULL) {
				(void)wb_mmr_read32(SPI_RFIFO(p->n));
				q->rx++;
			}
		}
		if (!(q->end & WB_SPI_QF_FED)) {
			bool fed;

			if (dma) {
				const uint32_t dstat = wb_mmr_read32(DMA_STAT(ch));

				if (dstat & DMA_STAT_IRQERR)
					return WB_SPI_QE_DMA_ERR;
				fed = (dstat & DMA_STAT_IRQDONE) != 0u;
				if (fed)
					wb_mmr_write32(DMA_STAT(ch), DMA_STAT_IRQDONE);
			} else {
				fed = qt_feed(p, tx, n, out, pads);
			}
			if (fed) {
				/* S-13b.044: TF unmasked here, and nowhere else;
				 * the flag's bound runs from the last feed on */
				q->end |= WB_SPI_QF_FED;
				q->t_feed = qt_since(hand, wb_core_cycles());
				if (begun)
					began = now;
				wb_mmr_write32(SPI_IMSK_SET(p->n), SPI_STAT_TF);
				wb_mmr_sync();
			}
		} else if (wb_mmr_read32(SPI_ILAT(p->n)) & SPI_STAT_TF) {
			q->end |= WB_SPI_QF_TF_ILAT;
			q->t_flag = qt_since(hand, wb_core_cycles());
			return WB_SPI_QE_FLAG;
		}
		if (!wb_gpio_get(s->master_pin)) {
			if (!begun) {
				begun = true;
				began = now;
				q->end |= WB_SPI_QF_BEGAN;
				q->t_start = at;
			}
		} else if (begun) {
			return WB_SPI_QE_MASTER;	/* S-09.013: the master ended it */
		}
		if (!begun && now - hand >= start)
			return WB_SPI_QE_NO_START;
		if (begun && now - began >= flag) {
			q->stat_bound = stat;
			return WB_SPI_QE_BOUND;
		}
	}
}

/* The SPIF candidate: once the master has begun, a loop that reads
 * SPI_STAT and the cycle counter alone; the end is SPIF set again after a
 * fall with SPI_STAT unchanged for `quiet`. */
static uint32_t qt_spif_loop(const struct wb_spi_slave *s, uint64_t hand,
			     uint64_t start, uint64_t flag, uint64_t quiet,
			     struct wb_spi_qt *q)
{
	const struct wb_spi *p = &s->spi;
	uint64_t began, changed;
	uint32_t was;
	bool fell = false;

	while (wb_gpio_get(s->master_pin))
		if (wb_core_cycles() - hand >= start)
			return WB_SPI_QE_NO_START;
	began = changed = wb_core_cycles();
	q->end |= WB_SPI_QF_BEGAN;
	q->t_start = qt_since(hand, began);
	was = wb_mmr_read32(SPI_STAT(p->n));
	for (;;) {
		const uint32_t stat = wb_mmr_read32(SPI_STAT(p->n));
		const uint64_t now = wb_core_cycles();

		q->stat_or |= stat;
		if (stat & SPI_STAT_TF)
			q->end |= WB_SPI_QF_TF_STAT;
		if (stat != was) {
			qt_spif(q, was, stat, qt_since(hand, now), true);
			fell = fell || !(stat & SPI_STAT_SPIF);
			changed = now;
			was = stat;
		}
		if (stat & (SPI_STAT_TUR | SPI_STAT_TC))
			return WB_SPI_QE_TX_ERR;
		if (fell && (stat & SPI_STAT_SPIF) && now - changed >= quiet)
			return WB_SPI_QE_FLAG;
		if (now - began >= flag) {
			q->stat_bound = stat;
			return WB_SPI_QE_BOUND;
		}
	}
}

int wb_spi_slave_qtest(const struct wb_spi_slave *s, const uint8_t *tx,
		       size_t n, unsigned cand, bool ren, uint32_t start_us,
		       uint32_t flag_us, uint32_t quiet_us, struct wb_spi_qt *q)
{
	static const uint32_t stages[] = {
		[WB_SPI_QE_BOUND] = WB_SPI_STAGE_REPLY_BOUND,
		[WB_SPI_QE_MASTER] = WB_SPI_STAGE_MASTER_END,
		[WB_SPI_QE_TX_ERR] = WB_SPI_STAGE_TX_ERR,
		[WB_SPI_QE_DMA_ERR] = WB_SPI_STAGE_DMA_ERR,
		[WB_SPI_QE_NO_START] = WB_SPI_STAGE_MASTER_READY,
	};
	const struct wb_spi *p = &s->spi;
	const unsigned ch = WB_DMA_SPI0_TX + 2u * p->n;
	/* S-13b.044's reply value, single I/O and quad */
	const uint32_t single = dir_ctl(p, true) & ~SPI_CTL_MIOM_MASK;
	const uint32_t ctl = single | SPI_CTL_MIOM_QUAD;
	const bool dma = cand != WB_SPI_QT_TF_CORE;
	const uint64_t cpu = p->t->cycles_per_us;
	struct wb_spi_array *a = &wb_spi_tx_array;
	uint32_t end = WB_SPI_QE_NO_START;
	uint64_t hand, until;
	size_t out = 0u;
	unsigned pads = 2u;
	int rc;

	*q = (struct wb_spi_qt){ 0 };
	q->t_start = q->t_feed = q->t_flag = q->t_end = WB_SPI_QT_NONE;
	q->end = WB_SPI_QE_REFUSED;
	if (!words_ok(p, n) || wb_spi_word_bytes(p) != 4u || n / 4u > SPI_TWC_MAX ||
	    n + sizeof queued >= WB_SPI_BRIDGE_MAX || cand < WB_SPI_QT_TF_DMA ||
	    cand > WB_SPI_QT_SPIF || (ren && cand == WB_SPI_QT_SPIF))
		return refused(p);
	if (dma) {
		arr_reset(a);
		if (!arr_put(a, WB_DMA_ARRAY_SEND, (uint8_t *)(uintptr_t)tx, NULL, n,
			     false) ||
		    !arr_put(a, WB_DMA_ARRAY_SEND, (uint8_t *)queued, NULL,
			     sizeof queued, false))
			return refused(p);
		arr_done(a);
		wb_dma_cache_send((uint32_t)(uintptr_t)tx, (uint32_t)n);
	}
	q->end = WB_SPI_QE_NONE;
	/* S-13b.044's order, EN clear throughout: MIOM by a write that finds EN clear */
	en_off(p);
	wb_mmr_write32(SPI_CTL(p->n), ctl);
	fault_open(p->n);
	wb_mmr_write32(SPI_IMSK_CLR(p->n), SPI_STAT_TF);	/* TF masked until fed */
	wb_mmr_write32(SPI_ILAT_CLR(p->n), SPI_STAT_TF);
	wb_mmr_write32(SPI_STAT(p->n), 0xFFFFFFFFu);		/* 1 */
	if (dma) {
		wb_mmr_write32(DMA_STAT(ch), DMA_W1C);		/* 2 */
		wb_dma_array_start(ch, a->d, WB_DMA_ARRAY_SEND);
	}
	wb_mmr_write32(SPI_TWC(p->n), (uint32_t)(n / 4u));	/* 3 */
	wb_mmr_write32(SPI_TXCTL(p->n), SPI_TXCTL_REPLY);	/* 4 */
	wb_mmr_write32(SPI_RXCTL(p->n), ren ? SPI_XCTL_EN : 0u);	/* 5 */
	wb_mmr_write32(SPI_CTL(p->n), ctl | SPI_CTL_EN);	/* 6: EN last */
	wb_mmr_sync();
	if (!dma)
		(void)qt_feed(p, tx, n, &out, &pads);	/* the FIFO's first words */
	rc = hand_over(s, WB_SPI_SLAVE_SESSION, start_us);
	hand = wb_core_cycles();
	q->hand = (uint32_t)hand;
	if (rc == WB_SPI_OK && cand == WB_SPI_QT_SPIF)
		end = qt_spif_loop(s, hand, start_us * cpu, flag_us * cpu,
				   quiet_us * cpu, q);
	else if (rc == WB_SPI_OK)
		end = qt_tf_loop(s, tx, n, dma, ren, hand, start_us * cpu,
				 flag_us * cpu, &out, &pads, q);
	q->t_end = qt_since(hand, wb_core_cycles());
	/* S-13b.043: the ready pin back first, then the registers as they are */
	(void)wb_gpio_gpio(s->ready_pin);
	wb_mmr_sync();
	q->stat = wb_mmr_read32(SPI_STAT(p->n));
	q->dma = dma ? wb_mmr_read32(DMA_STAT(ch)) : 0u;
	q->twc = wb_mmr_read32(SPI_TWC(p->n));
	if (dma && (q->dma & DMA_STAT_IRQDONE))
		q->end |= WB_SPI_QF_FED;
	wb_mmr_write32(SPI_IMSK_CLR(p->n), SPI_STAT_TF);
	if (end == WB_SPI_QE_FLAG) {
		/* as a reply's end (S-13b.083): TEN cleared, TF acknowledged,
		 * the DMA disabled; EN stays set until the session is over */
		wb_mmr_write32(SPI_TXCTL(p->n), SPI_TXCTL_REPLY & ~SPI_XCTL_EN);
		wb_mmr_write32(SPI_STAT(p->n), SPI_STAT_TF);
		wb_mmr_write32(SPI_ILAT_CLR(p->n), SPI_STAT_TF);
		if (dma)
			wb_dma_stop(ch);
		q->rc = WB_SPI_OK;
	} else {
		/* given up (S-09.013, S-13b.045): recorded, then stopped */
		q->rc = end == WB_SPI_QE_TX_ERR || end == WB_SPI_QE_DMA_ERR ?
			WB_SPI_ERR_HW : WB_SPI_TIMEOUT;
		q->stage = end < sizeof stages / sizeof stages[0] ? stages[end] : 0u;
		(void)fault_note(p->n, q->rc, q->stat, q->twc, q->stage);
		wb_mmr_write32(SPI_TXCTL(p->n), 0u);
		wb_mmr_write32(SPI_RXCTL(p->n), 0u);
		if (dma)
			wb_dma_stop_idle(ch, p->t);
		en_off(p);
	}
	wb_mmr_sync();
	/* the session's end: the master raises its select once the line is back */
	until = wb_time_deadline(p->t, start_us);
	while (!wb_gpio_get(s->master_pin) && !wb_time_passed(until))
		;
	if (wb_gpio_get(s->master_pin))
		q->end |= WB_SPI_QF_ENDED;
	/* single I/O again: EN cleared with MIOM kept, then MIOM cleared by a
	 * write that finds EN clear (S-13b.040); disabled until the next set-up */
	en_off(p);
	wb_mmr_write32(SPI_CTL(p->n), single);
	wb_mmr_write32(SPI_TXCTL(p->n), 0u);
	wb_mmr_write32(SPI_RXCTL(p->n), 0u);
	wb_mmr_write32(SPI_IMSK_CLR(p->n), SPI_STAT_TF);
	wb_mmr_write32(SPI_ILAT_CLR(p->n), 0xFFFFFFFFu);
	wb_mmr_write32(SPI_STAT(p->n), SPI_STAT_W1C);
	if (dma)
		wb_dma_stop_idle(ch, p->t);
	wb_mmr_sync();
	if (wb_mmr_read32(SPI_CTL(p->n)) == single)
		q->end |= WB_SPI_QF_SINGLE;
	q->end |= end;
	return q->rc;
}

int wb_spi_qtest_recv(const struct wb_spi *s, uint32_t ctl, uint32_t clk,
		      uint8_t *buf, size_t n, uint32_t out[4])
{
	const unsigned ch = WB_DMA_SPI0_RX + 2u * s->n;
	const struct wb_spi_config c = { ctl, clk, 0u, 0u };
	struct wb_spi_array *a = &wb_spi_rx_array;
	uint32_t stage = WB_SPI_STAGE_NONE;
	uint64_t t0;
	int rc;

	out[0] = out[1] = out[2] = out[3] = 0u;
	if (!counted_ok(s, n) || n >= WB_SPI_BRIDGE_MAX ||
	    wb_spi_check(s->n, &c) != WB_SPI_OK ||
	    ((ctl ^ s->ctl) & SPI_CTL_SIZE_MASK) != 0u ||
	    (ctl & SPI_CTL_MIOM_MASK) == 0u)
		return refused(s);
	arr_reset(a);
	if (!arr_recv(a, buf, n))
		return refused(s);
	arr_done(a);
	fresh[s->n] = true;
	fault_open(s->n);
	rx_open[s->n] = false;
	last_clk[s->n] = wb_mmr_read32(SPI_CLK(s->n));
	wb_mmr_write32(SPI_CTL(s->n), 0u);
	wb_mmr_write32(SPI_CLK(s->n), clk);
	wb_mmr_write32(SPI_CTL(s->n), ctl);	/* EN clear: the write that changes MIOM */
	wb_mmr_write32(SPI_TXCTL(s->n), 0u);	/* one channel alone in quad I/O */
	wb_mmr_write32(SPI_STAT(s->n), SPI_STAT_W1C);
	wb_mmr_write32(DMA_STAT(ch), DMA_W1C);
	wb_dma_array_start(ch, a->d, WB_DMA_ARRAY_RECV);
	wb_mmr_write32(SPI_RWC(s->n), (uint32_t)(n / wb_spi_word_bytes(s)));
	wb_mmr_write32(SPI_RXCTL(s->n), MASTER_RXCTL);
	t0 = wb_core_cycles();
	wb_mmr_write32(SPI_CTL(s->n), ctl | SPI_CTL_EN);	/* EN last */
	wb_mmr_sync();
	rc = dma_wait(s, ch, SPI_RWC(s->n), 0u, NULL, NULL, &stage);
	if (rc != WB_SPI_OK)
		return master_fail(s, rc, stage == WB_SPI_STAGE_DMA_STALL ?
				   WB_SPI_STAGE_RX_WORD : stage, true, ctl, true);
	out[2] = (uint32_t)(wb_core_cycles() - t0);
	out[0] = wb_mmr_read32(SPI_STAT(s->n));
	out[1] = wb_mmr_read32(SPI_RWC(s->n));
	out[3] = wb_mmr_read32(DMA_STAT(ch));
	wb_mmr_write32(DMA_STAT(ch), DMA_STAT_IRQDONE);
	wb_mmr_write32(SPI_RXCTL(s->n), MASTER_RXCTL & ~SPI_XCTL_EN);
	wb_mmr_write32(SPI_CTL(s->n), ctl);	/* EN cleared, MIOM kept */
	wb_dma_stop(ch);
	wb_mmr_write32(SPI_STAT(s->n), out[0] & SPI_STAT_W1C);
	wb_mmr_write32(SPI_CTL(s->n), 0u);
	wb_mmr_write32(SPI_CLK(s->n), last_clk[s->n]);
	wb_mmr_sync();
	arr_copy(a);
	if (out[0] & (SPI_STAT_ROR | SPI_STAT_MF))
		return fault_note(s->n, WB_SPI_ERR_HW, out[0], 0u,
				  WB_SPI_STAGE_OVERRUN);
	return WB_SPI_OK;
}

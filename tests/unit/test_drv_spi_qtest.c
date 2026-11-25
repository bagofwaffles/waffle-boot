/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The quad reply test's two ends in the SPI driver (src/drivers/bf70x/spi.c,
 * S-09.080, S-13b.084), against a model of the other end in the MMR fake.
 *
 * The slave: wb_spi_slave_qtest() with SPI2, PA4, PB15 and DMA8 under a
 * modeled master that clocks the reply in quad I/O while PA6 (PA4) is low.
 * Each candidate when its flag comes, TF after the last word or, early, as
 * the last word starts (then the slave's end cuts it, and the master's last
 * word reads wrong: what S-13b Q-11 (2) asks of a unit); TF never coming
 * (the bound, the give-up's registers, the record); REN set; a transmit
 * underrun; a master that never begins and one that ends first. In every
 * case: S-13b.044's order with EN clear (MIOM written with EN clear, EN set
 * last, then PA4), TF unmasked only once fed, PA4 taken back first, and SPI2
 * left disabled in single I/O with the session over.
 *
 * The master: wb_spi_qtest_recv() with SPI0 and DMA5: the set-up with MIOM
 * written with EN clear and EN last, the words, then EN cleared with MIOM
 * kept, SPI_CTL 0 and the clock put back; and a receive that never ends.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x/dma.h"
#include "waffle/drivers/bf70x/port.h"
#include "waffle/drivers/bf70x/spi.h"

static const struct wb_time cclk400 = { 400u };

#define N        2u
#define PA4      WB_PIN(WB_PORT_A, 4)
#define PB15     WB_PIN(WB_PORT_B, 15)
#define PRE_CTL  0x2510u		/* S-13b.042 */
#define FIFO     2u
#define REPLY_SINGLE 0x00006510u	/* S-13b.044, without EN */
#define REPLY_QUAD   0x00206510u
#define MAX_WORDS 1024u

static const struct wb_spi_config pre = { PRE_CTL, 0, 0, 0 };

enum { TF_AT_END, TF_NEVER, TF_AT_START };

/* The master of S-09.010 in quad I/O against SPI2, one word per `pace`
 * register accesses while PA6 is low; a word in flight takes `pace`
 * accesses and completes whatever the line does, but reads all ones when
 * the slave's transmit stopped meanwhile (TEN or EN cleared). */
static struct {
	unsigned words, started, done, pace, ticks, flight;
	bool armed, cs, never, quit;	/* never: it never selects; quit: it gives up */
	unsigned quit_after;
	uint32_t miso[MAX_WORDS];
	uint32_t cur;			/* the word in flight */
	bool cut;			/* ... its transmit stopped */
	unsigned tf_mode;
	bool force_tur;
	/* SPI2 */
	uint32_t ctl, rxctl, txctl, stat, twc, imsk, ilat;
	uint32_t tf[FIFO], rf[FIFO];
	unsigned ntf, nrf, rx_words;
	bool fer;
	/* DMA8 */
	bool dma_on, dma_run;
	unsigned dma_at;
	uint32_t dma_off, dma_stat;
	unsigned tf_writes;
} m;

static bool pa6_low(void)
{
	return m.fer && (m.ctl & SPI_CTL_EN) && m.ntf > 0u;
}

static void dma_tick(void)
{
	const struct wb_spi_array *a = &wb_spi_tx_array;
	const uint8_t *p;

	if (!m.dma_on || !m.dma_run || m.ntf == FIFO)
		return;
	while (m.dma_at < a->n && m.dma_off == a->bytes[m.dma_at]) {
		m.dma_at++;
		m.dma_off = 0;
	}
	if (m.dma_at == a->n) {
		m.dma_stat |= DMA_STAT_IRQDONE;
		m.dma_run = false;
		return;
	}
	p = a->mem[m.dma_at] + m.dma_off;
	m.tf[m.ntf++] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
			(uint32_t)p[3] << 24;
	m.dma_off += 4u;
	if (m.dma_off == a->bytes[m.dma_at] && m.dma_at + 1u == a->n) {
		m.dma_stat |= DMA_STAT_IRQDONE;
		m.dma_run = false;
	}
}

static bool tx_on(void)
{
	return (m.ctl & SPI_CTL_EN) && (m.txctl & SPI_XCTL_EN);
}

static void word_start(void)
{
	if (m.ntf == 0u || m.force_tur) {
		m.stat |= SPI_STAT_TUR;
		m.cur = 0u;
	} else {
		m.cur = m.tf[0];
		m.tf[0] = m.tf[1];
		m.ntf--;
	}
	m.cut = false;
	m.flight = m.pace;
	m.stat &= ~SPI_STAT_SPIF;
	if (m.twc != 0u && --m.twc == 0u && m.tf_mode == TF_AT_START)
		m.stat |= SPI_STAT_TF;
	m.started++;
}

static void word_end(void)
{
	m.miso[m.done++] = m.cut ? 0xFFFFFFFFu : m.cur;
	m.stat |= SPI_STAT_SPIF;
	if (m.rxctl & SPI_XCTL_EN) {
		if (m.nrf == FIFO)
			m.stat |= SPI_STAT_ROR;
		else
			m.rf[m.nrf++] = 0u;
	}
	if (m.done == m.words && m.tf_mode == TF_AT_END)
		m.stat |= SPI_STAT_TF;
}

static void tick(void)
{
	dma_tick();
	if (!tx_on() && m.flight)
		m.cut = true;
	if (!m.cs) {
		if (m.armed && !m.never && pa6_low()) {
			m.cs = true;
			m.armed = false;
		}
		return;
	}
	if (m.flight && --m.flight == 0u)
		word_end();
	if (m.quit && m.done == m.quit_after && !m.flight) {
		m.cs = false;
		m.quit = false;
		return;
	}
	if (m.started < m.words) {
		if (!m.flight && pa6_low() && ++m.ticks % m.pace == 0u)
			word_start();
	} else if (!m.flight && !pa6_low()) {
		m.cs = false;			/* the session is over */
	}
}

static uint32_t stat_now(void)
{
	uint32_t s = m.stat;

	if (m.nrf == 0u)
		s |= SPI_STAT_RFE;
	if (m.ntf == FIFO)
		s |= SPI_STAT_TFF;
	s |= (m.ntf == 0u ? 4u : m.ntf == 1u ? 2u : 0u) << 16;
	s |= (m.nrf == 0u ? 0u : m.nrf == 1u ? 2u : 4u) << 12;	/* RFS */
	return s;
}

static bool hook(void *ctx, enum fake_mmr_op op, unsigned width, wb_mmr_addr a,
		 uint32_t *v)
{
	const uint32_t bit = 1u << 4;

	(void)ctx;
	(void)width;
	tick();
	if ((m.stat & SPI_STAT_TF) && (m.imsk & SPI_STAT_TF))
		m.ilat |= SPI_STAT_TF;
	if (op == FAKE_MMR_READ) {
		if (a == SPI_STAT(N))
			*v = stat_now();
		else if (a == SPI_ILAT(N))
			*v = m.ilat;
		else if (a == SPI_TWC(N))
			*v = m.twc;
		else if (a == SPI_CTL(N))
			*v = m.ctl;
		else if (a == DMA_STAT(8))
			*v = m.dma_stat;
		else if (a == SPI_RFIFO(N)) {
			*v = m.rf[0];
			m.rf[0] = m.rf[1];
			if (m.nrf)
				m.nrf--;
			m.rx_words++;
		} else if (a == PORT_DATA(WB_PORT_B))
			*v = m.cs ? 0u : 1u << 15;
		return true;
	}
	if (op != FAKE_MMR_WRITE)
		return true;
	if (a == SPI_CTL(N)) {
		if (!(*v & SPI_CTL_EN))
			m.nrf = m.ntf = 0;
		m.ctl = *v;
	} else if (a == SPI_RXCTL(N)) {
		m.rxctl = *v;
	} else if (a == SPI_TXCTL(N)) {
		m.txctl = *v;
	} else if (a == SPI_IMSK_SET(N)) {
		m.imsk |= *v;
	} else if (a == SPI_IMSK_CLR(N)) {
		m.imsk &= ~*v;
	} else if (a == SPI_ILAT_CLR(N)) {
		m.ilat &= ~*v;
	} else if (a == SPI_TWC(N)) {
		m.twc = *v;
	} else if (a == SPI_STAT(N)) {
		m.stat &= ~(*v & SPI_STAT_W1C);
	} else if (a == SPI_TFIFO(N)) {
		if (m.ntf < FIFO)
			m.tf[m.ntf++] = *v;
		m.tf_writes++;
	} else if (a == DMA_CFG(8)) {
		m.dma_on = (*v & DMA_CFG_EN) != 0u;
		m.dma_run = m.dma_on;
		if (m.dma_on) {
			m.dma_at = 0;
			m.dma_off = 0;
		}
	} else if (a == DMA_STAT(8)) {
		m.dma_stat &= ~*v;
	} else if (a == PORT_FER_SET(WB_PORT_A) && (*v & bit)) {
		m.fer = true;
	} else if (a == PORT_FER_CLR(WB_PORT_A) && (*v & bit)) {
		m.fer = false;
	}
	return true;
}

static void up(struct wb_spi_slave *s)
{
	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(400);		/* 1 us a clock read */
	memset(&m, 0, sizeof m);
	m.pace = 3;
	m.stat = SPI_STAT_SPIF;
	fake_mmr_set_hook(hook, NULL);
	CHECK(wb_spi_slave_init(s, N, &pre, PA4, PB15, &cclk400, 100) == WB_SPI_OK);
	fake_mmr_clear_log();
}

/* The index of the first write to addr (with `val` unless any), from `from`. */
static size_t write_at(wb_mmr_addr addr, uint32_t val, bool any, size_t from)
{
	const struct fake_mmr_access *l = fake_mmr_log();

	for (size_t i = from; i < fake_mmr_count(); i++)
		if (l[i].op == FAKE_MMR_WRITE && l[i].addr == addr && (any || l[i].value == val))
			return i;
	return SIZE_MAX;
}

/* S-13b.044's order with EN clear, EN last, then PA4; MIOM written only by
 * a write that found EN clear; the end: PA4 back before any other write,
 * and SPI2 left disabled in single I/O. */
static bool order_ok(bool dma, uint32_t rxctl)
{
	const struct fake_mmr_access *l = fake_mmr_log();
	const size_t miom = write_at(SPI_CTL(N), REPLY_QUAD, false, 0);
	const size_t stat = write_at(SPI_STAT(N), 0xFFFFFFFFu, false, miom);
	const size_t dcfg = write_at(DMA_CFG(8), WB_DMA_ARRAY_SEND, false, miom);
	const size_t twc = write_at(SPI_TWC(N), 0, true, miom);
	const size_t txctl = write_at(SPI_TXCTL(N), SPI_TXCTL_REPLY, false, miom);
	const size_t rx = write_at(SPI_RXCTL(N), rxctl, false, txctl);
	const size_t en = write_at(SPI_CTL(N), REPLY_QUAD | SPI_CTL_EN, false, 0);
	const size_t give = write_at(PORT_FER_SET(WB_PORT_A), 0, true, 0);
	const size_t back = write_at(PORT_FER_CLR(WB_PORT_A), 0, true, give);
	const size_t single = write_at(SPI_CTL(N), REPLY_SINGLE, false, back);

	if (miom == SIZE_MAX || stat == SIZE_MAX || twc == SIZE_MAX || txctl == SIZE_MAX ||
	    rx == SIZE_MAX || en == SIZE_MAX || give == SIZE_MAX || back == SIZE_MAX ||
	    single == SIZE_MAX)
		return false;
	if (dma && (dcfg == SIZE_MAX || dcfg < stat || dcfg > twc))
		return false;
	if (!(stat < twc && twc < txctl && txctl < rx && rx < en && en < give))
		return false;
	/* MIOM changes only with EN found clear */
	{
		uint32_t ctl = PRE_CTL | SPI_CTL_EN;	/* as wb_spi_slave_init() left it */
		bool en_now = true;

		for (size_t i = 0; i < fake_mmr_count(); i++) {
			if (l[i].op != FAKE_MMR_WRITE || l[i].addr != SPI_CTL(N))
				continue;
			if (((l[i].value ^ ctl) & SPI_CTL_MIOM_MASK) && en_now)
				return false;
			ctl = l[i].value;
			en_now = (ctl & SPI_CTL_EN) != 0u;
		}
	}
	/* TF unmasked once, after the hand-over; PA4 back before the end's writes */
	{
		const size_t unmask = write_at(SPI_IMSK_SET(N), SPI_STAT_TF, false, 0);

		if (unmask != SIZE_MAX && (unmask < give || unmask > back ||
					   write_at(SPI_IMSK_SET(N), 0, true, unmask + 1u) != SIZE_MAX))
			return false;
		for (size_t i = give + 1u; i < back; i++)
			if (l[i].op == FAKE_MMR_WRITE &&
			    (l[i].addr == SPI_TXCTL(N) || l[i].addr == SPI_CTL(N) ||
			     l[i].addr == DMA_CFG(8)))
				return false;
	}
	return m.ctl == REPLY_SINGLE && m.txctl == 0u && m.rxctl == 0u && !m.fer &&
	       !m.dma_on && m.imsk == 0u;
}

static uint8_t tx[MAX_WORDS * 4u];

static void reply_words(unsigned words)
{
	for (unsigned i = 0; i < words * 4u; i++)
		tx[i] = (uint8_t)(i * 7u + 1u);
	m.words = words;
	m.armed = true;
}

static uint32_t tx_word(unsigned k)
{
	return (uint32_t)tx[4u * k] | (uint32_t)tx[4u * k + 1u] << 8 |
	       (uint32_t)tx[4u * k + 2u] << 16 | (uint32_t)tx[4u * k + 3u] << 24;
}

static bool all_words(unsigned words)
{
	for (unsigned k = 0; k < words; k++)
		if (m.miso[k] != tx_word(k))
			return false;
	return m.done == words;
}

/* Each candidate, its flag after the last word: every word whole, the
 * record's times in order. */
static void test_flag(void)
{
	static const unsigned cands[] = { WB_SPI_QT_TF_DMA, WB_SPI_QT_TF_CORE, WB_SPI_QT_SPIF };
	struct wb_spi_slave s;
	struct wb_spi_qt q;

	for (unsigned k = 0; k < 3u; k++) {
		const unsigned c = cands[k];

		up(&s);
		reply_words(16);
		CHECK(wb_spi_slave_qtest(&s, tx, 64, c, false, 1000, 5000, 20, &q) == WB_SPI_OK);
		CHECK((q.end & 0xFFu) == WB_SPI_QE_FLAG);
		CHECK((q.end & (WB_SPI_QF_BEGAN | WB_SPI_QF_ENDED | WB_SPI_QF_SINGLE)) ==
		      (WB_SPI_QF_BEGAN | WB_SPI_QF_ENDED | WB_SPI_QF_SINGLE));
		CHECK(all_words(16));
		CHECK(!(m.stat & SPI_STAT_TUR));
		CHECK(order_ok(c != WB_SPI_QT_TF_CORE, 0u));
		CHECK(q.t_start != WB_SPI_QT_NONE && q.t_feed != WB_SPI_QT_NONE &&
		      q.t_flag != WB_SPI_QT_NONE && q.t_start <= q.t_end && q.t_feed <= q.t_flag &&
		      q.t_flag <= q.t_end);
		CHECK(q.rc == WB_SPI_OK && q.stage == 0u);
		if (c == WB_SPI_QT_SPIF) {
			/* SPI_STAT alone: no TF unmask, no ILAT read; SPIF fell and rose */
			CHECK(write_at(SPI_IMSK_SET(N), 0, true, 0) == SIZE_MAX);
			CHECK((q.spif & 0xFFFFu) >= 1u && (q.spif >> 16) >= 1u);
			CHECK(q.end & WB_SPI_QF_FED);
		} else {
			CHECK((q.end & (WB_SPI_QF_TF_ILAT | WB_SPI_QF_FED)) ==
			      (WB_SPI_QF_TF_ILAT | WB_SPI_QF_FED));
			CHECK(write_at(SPI_IMSK_SET(N), SPI_STAT_TF, false, 0) != SIZE_MAX);
			/* the end: TEN cleared, TF acknowledged, then single I/O */
			CHECK(write_at(SPI_TXCTL(N), SPI_TXCTL_REPLY & ~SPI_XCTL_EN, false, 0) !=
			      SIZE_MAX);
		}
		if (c == WB_SPI_QT_TF_CORE)
			CHECK(m.tf_writes == 18u);	/* the words and two more */
		CHECK(fake_mmr_errors() == 0u);
	}
}

/* TF as the last word starts: the slave's end stops the transmit under
 * it, and the master's last word reads wrong (S-13b Q-11 (2)). */
static void test_tf_early(void)
{
	struct wb_spi_slave s;
	struct wb_spi_qt q;

	up(&s);
	m.tf_mode = TF_AT_START;
	m.pace = 40;
	reply_words(8);
	CHECK(wb_spi_slave_qtest(&s, tx, 32, WB_SPI_QT_TF_DMA, false, 1000, 5000, 20, &q) ==
	      WB_SPI_OK);
	CHECK((q.end & 0xFFu) == WB_SPI_QE_FLAG);
	CHECK(m.done == 8u && m.miso[6] == tx_word(6) && m.miso[7] != tx_word(7));
	CHECK(order_ok(true, 0u));
}

/* No TF: the bound, the give-up's registers recorded, then single I/O. */
static void test_no_flag(void)
{
	struct wb_spi_slave s;
	struct wb_spi_qt q;

	up(&s);
	m.tf_mode = TF_NEVER;
	reply_words(4);
	CHECK(wb_spi_slave_qtest(&s, tx, 16, WB_SPI_QT_TF_DMA, false, 1000, 300, 20, &q) ==
	      WB_SPI_TIMEOUT);
	CHECK((q.end & 0xFFu) == WB_SPI_QE_BOUND);
	CHECK((q.end & WB_SPI_QF_FED) && !(q.end & WB_SPI_QF_TF_ILAT));
	CHECK(q.t_flag == WB_SPI_QT_NONE && q.stage == WB_SPI_STAGE_REPLY_BOUND);
	CHECK(q.stat_bound != 0u && q.t_end >= 300u * 400u);
	CHECK(all_words(4));
	CHECK(order_ok(true, 0u));
	{
		struct wb_spi_fault f;

		wb_spi_slave_last_fault(&s, &f);
		CHECK(f.rc == WB_SPI_TIMEOUT && f.stage == WB_SPI_STAGE_REPLY_BOUND);
	}
}

/* REN set: each received word read and counted; refused for SPIF. */
static void test_ren(void)
{
	struct wb_spi_slave s;
	struct wb_spi_qt q;

	up(&s);
	reply_words(6);
	CHECK(wb_spi_slave_qtest(&s, tx, 24, WB_SPI_QT_TF_CORE, true, 1000, 5000, 20, &q) ==
	      WB_SPI_OK);
	CHECK(all_words(6));
	CHECK(q.rx == 6u && m.rx_words == 6u && !(m.stat & SPI_STAT_ROR));
	CHECK(order_ok(false, SPI_XCTL_EN));

	up(&s);
	CHECK(wb_spi_slave_qtest(&s, tx, 24, WB_SPI_QT_SPIF, true, 1000, 5000, 20, &q) ==
	      WB_SPI_ERR_ARG);
	CHECK((q.end & 0xFFu) == WB_SPI_QE_REFUSED && fake_mmr_count() == 0u);
	CHECK(wb_spi_slave_qtest(&s, tx, 10, WB_SPI_QT_TF_DMA, false, 1000, 5000, 20, &q) ==
	      WB_SPI_ERR_ARG);
	CHECK(wb_spi_slave_qtest(&s, tx, 12, 0, false, 1000, 5000, 20, &q) == WB_SPI_ERR_ARG);
}

/* A transmit underrun, a master that never begins and one that ends first:
 * each given up, SPI2 single after. */
static void test_give_up(void)
{
	struct wb_spi_slave s;
	struct wb_spi_qt q;

	up(&s);
	m.force_tur = true;
	reply_words(4);
	CHECK(wb_spi_slave_qtest(&s, tx, 16, WB_SPI_QT_TF_DMA, false, 1000, 5000, 20, &q) ==
	      WB_SPI_ERR_HW);
	CHECK((q.end & 0xFFu) == WB_SPI_QE_TX_ERR && q.stage == WB_SPI_STAGE_TX_ERR);
	CHECK(order_ok(true, 0u));

	up(&s);
	m.never = true;
	reply_words(4);
	CHECK(wb_spi_slave_qtest(&s, tx, 16, WB_SPI_QT_SPIF, false, 200, 5000, 20, &q) ==
	      WB_SPI_TIMEOUT);
	CHECK((q.end & 0xFFu) == WB_SPI_QE_NO_START && !(q.end & WB_SPI_QF_BEGAN));
	CHECK(order_ok(true, 0u));

	up(&s);
	m.quit = true;
	m.quit_after = 2;
	m.tf_mode = TF_NEVER;
	reply_words(8);
	CHECK(wb_spi_slave_qtest(&s, tx, 32, WB_SPI_QT_TF_CORE, false, 1000, 5000, 20, &q) ==
	      WB_SPI_TIMEOUT);
	CHECK((q.end & 0xFFu) == WB_SPI_QE_MASTER && q.stage == WB_SPI_STAGE_MASTER_END);
	CHECK(order_ok(false, 0u));
}

/* ---- the master's end ---- */

#define M_CTL 0x00002592u
static struct {
	bool on;
	unsigned at, stall;
	uint32_t off, stat, rwc, k;
} dm;

static bool m_hook(void *ctx, enum fake_mmr_op op, unsigned width, wb_mmr_addr a,
		   uint32_t *v)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_WRITE && a == DMA_CFG(5)) {
		dm.on = (*v & DMA_CFG_EN) != 0u;
		dm.at = 0;
		dm.off = 0;
	} else if (op == FAKE_MMR_WRITE && a == DMA_STAT(5)) {
		dm.stat &= ~*v;
	} else if (op == FAKE_MMR_WRITE && a == SPI_RWC(0)) {
		dm.rwc = *v;
	} else if (op == FAKE_MMR_READ && a == SPI_RWC(0)) {
		*v = dm.rwc;
	} else if (op == FAKE_MMR_READ && a == SPI_STAT(0)) {
		*v = dm.rwc == 0u && dm.on ? SPI_STAT_RF | SPI_STAT_RFE : SPI_STAT_RFE;
	} else if (op == FAKE_MMR_READ && a == DMA_STAT(5)) {
		const struct wb_spi_array *arr = &wb_spi_rx_array;

		/* a word a look, into the descriptors' memory */
		if (dm.on && !dm.stall && !(dm.stat & DMA_STAT_IRQDONE) && dm.at < arr->n) {
			uint8_t *p = arr->mem[dm.at] + dm.off;
			const uint32_t w = 0xC0DE0000u + dm.k++;

			p[0] = (uint8_t)w;
			p[1] = (uint8_t)(w >> 8);
			p[2] = (uint8_t)(w >> 16);
			p[3] = (uint8_t)(w >> 24);
			dm.off += 4u;
			dm.rwc--;
			if (dm.off == arr->bytes[dm.at]) {
				dm.at++;
				dm.off = 0;
			}
			if (dm.at == arr->n)
				dm.stat |= DMA_STAT_IRQDONE;
		}
		*v = dm.stat;
	} else if (op == FAKE_MMR_READ && a == DMA_XCNT_CUR(5)) {
		*v = dm.rwc;
	}
	return true;
}

static void test_master(void)
{
	static _Alignas(32) uint8_t buf[256];
	struct wb_spi s;
	uint32_t out[4];
	size_t miom, en, clr, zero, clk;

	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(400);
	memset(&dm, 0, sizeof dm);
	fake_mmr_set_hook(m_hook, NULL);
	CHECK(wb_spi_init(&s, 0, &(struct wb_spi_config){ M_CTL, 3u, 0u, 0u }, &cclk400, 50) ==
	      WB_SPI_OK);
	fake_mmr_clear_log();
	CHECK(wb_spi_qtest_recv(&s, M_CTL | SPI_CTL_MIOM_QUAD | SPI_CTL_FMODE, 1u, buf, 256,
				out) == WB_SPI_OK);
	for (unsigned k = 0; k < 64u; k++) {
		const uint8_t *p = buf + 4u * k;

		CHECK(((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
		       (uint32_t)p[3] << 24) == 0xC0DE0000u + k);
	}
	CHECK((out[0] & SPI_STAT_RF) && out[1] == 0u && (out[3] & DMA_STAT_IRQDONE));
	/* SPI_CTL 0, the clock, the control value with MIOM (EN clear), the
	 * set-up, EN last; after: EN clear with MIOM, then 0 and the clock */
	miom = write_at(SPI_CTL(0), M_CTL | SPI_CTL_MIOM_QUAD | SPI_CTL_FMODE, false, 0);
	en = write_at(SPI_CTL(0), M_CTL | SPI_CTL_MIOM_QUAD | SPI_CTL_FMODE | SPI_CTL_EN, false, 0);
	clr = write_at(SPI_CTL(0), M_CTL | SPI_CTL_MIOM_QUAD | SPI_CTL_FMODE, false, en);
	zero = write_at(SPI_CTL(0), 0u, false, clr);
	clk = write_at(SPI_CLK(0), 3u, false, zero);
	CHECK(write_at(SPI_CTL(0), 0u, false, 0) < miom && write_at(SPI_CLK(0), 1u, false, 0) < miom);
	CHECK(miom < write_at(SPI_RWC(0), 64u, false, 0) &&
	      write_at(SPI_RWC(0), 64u, false, 0) < write_at(SPI_RXCTL(0), 0x0001001Du, false, 0) &&
	      write_at(SPI_RXCTL(0), 0x0001001Du, false, 0) < en &&
	      write_at(DMA_CFG(5), WB_DMA_ARRAY_RECV, false, 0) < en &&
	      write_at(SPI_TXCTL(0), 0u, false, miom) < en);
	CHECK(en < clr && clr < zero && zero < clk && clk != SIZE_MAX);
	CHECK(write_at(SPI_RXCTL(0), 0x0001001Cu, false, en) < clr);

	/* a receive that never ends: given up, SPI_CTL 0 and the clock back */
	memset(&dm, 0, sizeof dm);
	dm.stall = 1;
	fake_mmr_clear_log();
	CHECK(wb_spi_qtest_recv(&s, M_CTL | SPI_CTL_MIOM_QUAD, 3u, buf, 16, out) == WB_SPI_TIMEOUT);
	en = write_at(SPI_CTL(0), M_CTL | SPI_CTL_MIOM_QUAD | SPI_CTL_EN, false, 0);
	clr = write_at(SPI_CTL(0), M_CTL | SPI_CTL_MIOM_QUAD, false, en);
	zero = write_at(SPI_CTL(0), 0u, false, clr);
	CHECK(en != SIZE_MAX && clr != SIZE_MAX && zero != SIZE_MAX &&
	      write_at(SPI_CLK(0), 3u, false, zero) != SIZE_MAX);
	{
		struct wb_spi_fault f;

		wb_spi_last_fault(&s, &f);
		CHECK(f.rc == WB_SPI_TIMEOUT && f.stage == WB_SPI_STAGE_RX_WORD);
	}
	/* single I/O and no MIOM are refused, touching nothing */
	fake_mmr_clear_log();
	CHECK(wb_spi_qtest_recv(&s, M_CTL, 3u, buf, 16, out) == WB_SPI_ERR_ARG);
	CHECK(wb_spi_qtest_recv(&s, M_CTL | SPI_CTL_MIOM_QUAD, 3u, buf, 10, out) == WB_SPI_ERR_ARG);
	CHECK(fake_mmr_count() == 0u);
}

int main(void)
{
	RUN(test_flag);
	RUN(test_tf_early);
	RUN(test_no_flag);
	RUN(test_ren);
	RUN(test_give_up);
	RUN(test_master);
	return check_summary("test_drv_spi_qtest");
}

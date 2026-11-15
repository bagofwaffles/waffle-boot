/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tests for the SPI slave end (src/drivers/bf70x/spi.c, S-13b.042-.045,
 * S-13b.083, S-09.010, S-09.011, S-09.013): SPI2 with PA4 and PB15, and its
 * DMA channels DMA8 and DMA9, against a model of the bridge master in the
 * MMR fake; each session's register trace judged by check_bridge_dma
 * (s13b_check.h).
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/bf70x_spi.h"

#include "s13b_check.h"
#include "s13b_trace.h"

static const struct wb_time cclk400 = { 400u };

#define N        2u
#define PA4      WB_PIN(WB_PORT_A, 4)
#define PB15     WB_PIN(WB_PORT_B, 15)
#define PRE_CTL  0x2510u		/* S-13b.042 before the link */
#define POST_CTL (PRE_CTL | SPI_CTL_MIOM_QUAD)
#define FIFO     2u			/* 32-bit words (HRM SPI_TFIFO) */
/* S-13b.044: the session values, with EN */
#define REPLY_PRE    (0x00006510u | SPI_CTL_EN)
#define REPLY_POST   (0x00206510u | SPI_CTL_EN)
#define REQUEST_PRE  (0x00022510u | SPI_CTL_EN)
#define REQUEST_POST (0x00222510u | SPI_CTL_EN)

static const struct wb_spi_config pre = { PRE_CTL, 0, 0, 0 };

/* a dropped payload over two parts, and more (a plain start's read is 16387) */
#define MAX_WORDS (0x40040u / 4u + 16u)

/*
 * The master of S-09.010 with SPI2 of the slave. A session starts when
 * PA6 (the slave's PA4) reads low: PC11, the slave's PB15, goes low. One
 * word moves per register access while PA6 is low, so up to the session's
 * length; then, once PA6 is high, PB15 goes high again. PA6 is low while
 * PA4 belongs to SPI2 (FER) and SPI2 is enabled with, under flow control
 * on RX (FCCH 0), its receive FIFO below the watermark (half full with
 * FCWM 2, else full), or on TX (FCCH 1, FCWM 0) its transmit FIFO not
 * empty; or while PA4 is a GPIO driven low. TF is set when TWC words
 * have gone with TWCEN; SPI_ILAT.TF latches while SPI_STAT.TF is set and
 * SPI_IMSK.TF unmasked (HRM, SPI_ILAT). With twc_ahead SPI_TWC counts a word
 * waiting in the transmit FIFO as gone, as a slave's SPI_TWC counts the
 * word loaded for the next clock (S-13b.045). With free_run it clocks at its
 * pace once the session has started whatever PA6 says, as a master no flow
 * control holds; a word that meets a full receive FIFO is lost (ROR).
 * SPI_STAT carries RFS. Each word's start and end, and what the slave does
 * with PA4, its status and its transmit, go to check_slave_tx's trace.
 */
static struct {
	/* the session the master runs */
	uint32_t mosi[MAX_WORDS];
	uint32_t miso[MAX_WORDS];
	unsigned words, done;
	bool armed, cs;		/* a session waits; PC11 low */
	bool hold_cs;		/* PB15 stays low (an earlier session) */
	bool stall;		/* the master stops clocking */
	unsigned quit;		/* stalled, it ends the session after this many accesses */
	unsigned stalled;
	bool force_ror, force_tur;
	unsigned pace, ticks;	/* one word per pace accesses */
	unsigned latency;	/* a word begun lands this many accesses later */
	unsigned flight;	/* accesses until the word under way lands */
	unsigned sessions;	/* ended */
	unsigned lag;		/* PC11 goes low this many accesses after PA6 read low */
	unsigned lagging;
	unsigned n_ticks, fer_clr_tick;
	unsigned arm_at;	/* the session is armed at this access */
	bool free_run;		/* once started, it clocks whatever PA6 says */
	unsigned rors;		/* words that met a full receive FIFO */
	unsigned top;		/* the most words the receive FIFO held */
	bool lose;		/* the received word of word `lost` is lost (ROR) */
	unsigned lost;
	unsigned quit_after;	/* the master gives up (PC11 high) after this many */
	bool no_tf;		/* TF never sets (S-13b.045) */
	bool twc_ahead;		/* SPI_TWC counts the word waiting to go out */
	uint64_t let_go_us;	/* PA6 still low this long after the last word: PC11 high */
	uint32_t imsk, ilat;	/* SPI_IMSK and SPI_ILAT, TF alone */
	uint64_t rx_read_at;	/* the core's clock at the last SPI_RFIFO read */
	unsigned rx_reads;	/* SPI_RFIFO reads since the hand-over */
	uint64_t pa4_given_at;	/* the core's clock as PA4 was last handed over */
	unsigned tf_writes;	/* words written to the transmit FIFO */
	uint64_t last_word_at;	/* the core's clock as the session's last word ended */
	uint64_t pa4_back_at;	/* and as PA4 was last taken back */
	/* SPI2 */
	uint32_t ctl, rxctl, txctl, stat, twc;
	uint32_t rf[FIFO], tf[FIFO];
	unsigned nrf, ntf;
	/* PA4 */
	bool fer, dir, data, inen;
	unsigned bad_gpio;	/* PA4 a GPIO but not an output driven high */
	unsigned pa6_low_ticks;
	/* DMA8 (c 0) and DMA9 (c 1): enabled, the descriptor under way and
	 * its bytes moved, the status; a word each access while there is one
	 * to move. DMA8 fills the transmit FIFO, DMA9 empties the receive
	 * FIFO, into each descriptor's memory (one place with a modify of 0). */
	bool dma_on[2], dma_run[2];	/* enabled; its array not through */
	unsigned dma_at[2];
	uint32_t dma_off[2], dma_stat[2];
	uint32_t dma_addr[2], dma_xcnt[2];	/* as the last word moved left them */
	bool dma_stall;		/* DMA9 takes nothing */
} m;

static struct s13b_trace tr;

/* check_slave_tx's trace of the reply under way (s13b_check.h). */
#define TX_TRACE_MAX 200000u
static struct {
	struct s13b_txe e[TX_TRACE_MAX];
	size_t n;
	bool on, full, handed, began, after;	/* began: PB15 read low since the hand-over */
} txt;

static int64_t now_us(void)
{
	return (int64_t)(fake_core_peek_cycles() / 400u);
}

static void tx_push(enum s13b_txev ev, uint32_t word, unsigned flags, int64_t rx_words,
		    uint32_t quiet_us, int64_t t_us)
{
	if (!txt.on)
		return;
	/* a status polled the same: the one event, its quiet time the latest */
	if (ev == S13B_TX_STATUS && txt.n != 0u && txt.e[txt.n - 1u].ev == S13B_TX_STATUS &&
	    txt.e[txt.n - 1u].flags == flags && txt.e[txt.n - 1u].rx_words == rx_words) {
		txt.e[txt.n - 1u].quiet_us = quiet_us;
		return;
	}
	if (txt.n == TX_TRACE_MAX) {
		txt.full = true;
		return;
	}
	txt.e[txt.n++] = (struct s13b_txe){ ev, word, flags, rx_words, quiet_us, t_us };
}

static void tx_start(void)
{
	txt.n = 0;
	txt.on = true;
	txt.full = false;
	txt.handed = false;
	txt.began = false;
	txt.after = false;
}

static bool pa6_low(void)
{
	unsigned mark = (m.ctl & SPI_CTL_FCWM_MASK) == SPI_CTL_FCWM_RX_HALF ?
				FIFO / 2u : FIFO;

	if (!m.fer)
		return m.dir && !m.data;
	if (!(m.ctl & SPI_CTL_EN))
		return false;
	return (m.ctl & SPI_CTL_FCCH) ? m.ntf > 0u : m.nrf < mark;
}

static void word(void)
{
	uint32_t in = m.mosi[m.done], out = 0;

	if (!m.latency)
		tx_push(S13B_TX_START, m.done, 0u, -1, 0u, -1);

	if (m.rxctl & SPI_XCTL_EN) {
		if (m.nrf == FIFO || (m.lose && m.done == m.lost)) {
			m.stat |= SPI_STAT_ROR;
			m.rors++;
		} else {
			m.rf[m.nrf++] = in;
		}
		if (m.nrf > m.top)
			m.top = m.nrf;
		if (m.force_ror)
			m.stat |= SPI_STAT_ROR;
	}
	if (m.txctl & SPI_XCTL_EN) {
		if (m.ntf == 0 || m.force_tur) {
			m.stat |= SPI_STAT_TUR;
		} else {
			out = m.tf[0];
			m.tf[0] = m.tf[1];
			m.ntf--;
		}
		if (m.twc && --m.twc == 0u && (m.txctl & SPI_XCTL_WCEN) && !m.no_tf)
			m.stat |= SPI_STAT_TF;
	}
	tx_push(S13B_TX_END, m.done, 0u, -1, 0u, now_us());
	m.miso[m.done++] = out;
	m.stat |= SPI_STAT_SPIF;
	if (m.done == m.words)
		m.last_word_at = fake_core_peek_cycles();
}

static void dma_tick(unsigned c)
{
	const struct wb_spi_array *a = c == 0u ? &wb_spi_tx_array : &wb_spi_rx_array;
	unsigned i;
	uint8_t *p;

	if (!m.dma_on[c] || !m.dma_run[c] || (c == 1u && m.dma_stall))
		return;
	while (m.dma_at[c] < a->n && m.dma_off[c] == a->bytes[m.dma_at[c]]) {
		m.dma_at[c]++;
		m.dma_off[c] = 0;
	}
	if (m.dma_at[c] == a->n) {
		m.dma_stat[c] |= DMA_STAT_IRQDONE;
		m.dma_run[c] = false;
		return;
	}
	i = m.dma_at[c];
	p = a->mem[i] + (a->d[i].mod != 0u ? m.dma_off[c] : 0u);
	if (c == 0u) {
		if (m.ntf == FIFO)
			return;
		m.tf[m.ntf++] = (uint32_t)p[0] | (uint32_t)p[1] << 8 |
				(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
		m.tf_writes++;
	} else {
		if (m.nrf == 0u)
			return;
		p[0] = (uint8_t)m.rf[0];
		p[1] = (uint8_t)(m.rf[0] >> 8);
		p[2] = (uint8_t)(m.rf[0] >> 16);
		p[3] = (uint8_t)(m.rf[0] >> 24);
		m.rf[0] = m.rf[1];
		m.nrf--;
	}
	m.dma_off[c] += 4u;
	m.dma_addr[c] = (uint32_t)(uintptr_t)p + 4u;
	m.dma_xcnt[c] = (uint32_t)((a->bytes[i] - m.dma_off[c]) / 4u);
	if (m.dma_off[c] == a->bytes[i] && i + 1u == a->n) {
		m.dma_stat[c] |= DMA_STAT_IRQDONE;
		m.dma_run[c] = false;
	}
}

static void tick(void)
{
	m.n_ticks++;
	dma_tick(0u);
	dma_tick(1u);
	if (m.arm_at && m.n_ticks == m.arm_at)
		m.armed = true;
	if (pa6_low())
		m.pa6_low_ticks++;
	if (!m.cs) {
		/* S-09.010 step 1: PA6 read low, then PC11 low (m.lag
		 * accesses later), whatever PA6 does in between */
		if (m.lagging && --m.lagging == 0u) {
			m.cs = true;
			m.armed = false;
		} else if (!m.lagging && m.armed && !m.hold_cs && pa6_low()) {
			m.lagging = m.lag;
			if (m.lag == 0u) {
				m.cs = true;
				m.armed = false;
			}
		}
		return;
	}
	/* a word under way completes whatever the ready line does now
	 * (S-13b.040: flow control stops only the next word) */
	if (m.flight && --m.flight == 0u)
		word();
	if (m.quit_after != 0u && m.done == m.quit_after && !m.flight) {
		m.cs = false;			/* the master gives up */
		m.words = m.done;
		m.quit_after = 0u;
		m.last_word_at = fake_core_peek_cycles();
		return;
	}
	if (m.done + (m.flight ? 1u : 0u) < m.words) {
		if (m.stall && m.quit && ++m.stalled == m.quit) {
			m.cs = false;		/* the master gives up */
			m.stall = false;
			m.words = m.done;
		} else if (!m.stall && !m.flight && (pa6_low() || m.free_run) &&
			   ++m.ticks % m.pace == 0u) {
			if (m.latency) {
				m.flight = m.latency;
				tx_push(S13B_TX_START, m.done, 0u, -1, 0u, -1);
			} else
				word();
		}
	} else if (m.done == m.words && !pa6_low()) {
		m.cs = false;
		m.sessions++;
	} else if (m.done == m.words && m.let_go_us != 0u &&
		   fake_core_peek_cycles() - m.last_word_at >= m.let_go_us * 400u) {
		m.cs = false;		/* S-09.013: the master lets the session go */
		m.sessions++;
	}
}

static void check_pa4(void)
{
	if (!m.fer && (!m.dir || m.inen || !m.data))
		m.bad_gpio++;
}

static uint32_t stat_now(void)
{
	uint32_t s = m.stat;

	if (m.nrf == 0)
		s |= SPI_STAT_RFE;
	s |= (m.nrf == 0u ? 0u : m.nrf == 1u ? 2u : 4u) << 12;	/* RFS */
	if (m.ntf == FIFO)
		s |= SPI_STAT_TFF;
	s |= (m.ntf == 0 ? 4u : m.ntf == 1u ? 2u : 0u) << 16;
	return s;
}

/* SPI_TWC as the slave reads it: a word counts once it has left the
 * transmit FIFO, the one under way already; with twc_ahead the next one
 * waiting too (S-13b.045). */
static uint32_t twc_now(void)
{
	uint32_t left = m.twc - (m.flight != 0u && m.twc != 0u ? 1u : 0u);

	if (m.twc_ahead && left != 0u && m.ntf != 0u && m.flight == 0u)
		left--;
	return left;
}

/* A status read, as check_slave_tx's event: the received-word count only
 * while the receive channel is on. */
static void tx_status(uint32_t v)
{
	unsigned f = 0;

	f |= (v & SPI_STAT_TF) ? S13B_TXF_TF : 0u;
	f |= (v & SPI_STAT_TUR) ? S13B_TXF_TUR : 0u;
	f |= (v & SPI_STAT_TC) ? S13B_TXF_TC : 0u;
	f |= (v & SPI_STAT_ROR) ? S13B_TXF_ROR : 0u;
	f |= (v & SPI_STAT_SPIF) ? S13B_TXF_SPIF : 0u;
	f |= m.ntf == 0u ? S13B_TXF_TFIFO_EMPTY : 0u;
	f |= twc_now() == 0u ? S13B_TXF_TWC_ZERO : 0u;
	tx_push(S13B_TX_STATUS, 0u, f, (m.rxctl & SPI_XCTL_EN) ? (int64_t)m.rx_reads : -1,
		(uint32_t)((fake_core_peek_cycles() - m.rx_read_at) / 400u), -1);
}

static bool hook(void *ctx, enum fake_mmr_op op, unsigned width,
		 wb_mmr_addr a, uint32_t *v)
{
	const uint32_t bit = 1u << 4;

	unsigned c;

	(void)ctx;
	(void)width;
	tick();
	if ((m.stat & SPI_STAT_TF) && (m.imsk & SPI_STAT_TF))
		m.ilat |= SPI_STAT_TF;
	for (c = 0; c < 2u; c++) {
		const unsigned ch = 8u + c;
		const struct wb_spi_array *arr = c == 0u ? &wb_spi_tx_array : &wb_spi_rx_array;

		if (op == FAKE_MMR_WRITE && a == DMA_CFG(ch)) {
			if (c == 0u && !(*v & DMA_CFG_EN) && txt.after)
				tx_push(S13B_TX_STOP, 0u, 0u, -1, 0u, -1);
			m.dma_on[c] = (*v & DMA_CFG_EN) != 0u;
			m.dma_run[c] = m.dma_on[c];
			if (m.dma_on[c]) {
				m.dma_at[c] = 0;
				m.dma_off[c] = 0;
			}
		} else if (op == FAKE_MMR_WRITE && a == DMA_STAT(ch)) {
			m.dma_stat[c] &= ~*v;
		} else if (op == FAKE_MMR_READ && a == DMA_STAT(ch)) {
			*v = m.dma_stat[c];
		} else if (op == FAKE_MMR_READ && a == DMA_XCNT_CUR(ch)) {
			*v = m.dma_xcnt[c];
		} else if (op == FAKE_MMR_READ && a == DMA_ADDR_CUR(ch)) {
			*v = m.dma_addr[c];
		} else if (op == FAKE_MMR_READ && a == DMA_DSCPTR_CUR(ch)) {
			/* past the descriptor under way, as the channel leaves it */
			*v = (uint32_t)(uintptr_t)&arr->d[m.dma_at[c] < arr->n ? m.dma_at[c] + 1u :
									 arr->n];
		}
	}
	if (op == FAKE_MMR_READ) {
		if (a == SPI_STAT(N)) {
			*v = stat_now();
			tx_status(*v);
		} else if (a == SPI_ILAT(N)) {
			*v = m.ilat;
			if (m.ilat & SPI_STAT_TF)
				tx_push(S13B_TX_STATUS, 0u, S13B_TXF_TF, -1, 0u, -1);
		} else if (a == DMA_STAT(8) || a == DMA_STAT(9) || a == DMA_XCNT_CUR(8) ||
			   a == DMA_XCNT_CUR(9) || a == DMA_DSCPTR_CUR(8) ||
			   a == DMA_DSCPTR_CUR(9) || a == DMA_ADDR_CUR(8) ||
			   a == DMA_ADDR_CUR(9)) {
			;
		} else if (a == SPI_RFIFO(N)) {
			*v = m.rf[0];
			m.rf[0] = m.rf[1];
			if (m.nrf)
				m.nrf--;
			m.rx_reads++;
			m.rx_read_at = fake_core_peek_cycles();
			tx_push(S13B_TX_STATUS, 0u, 0u, m.rx_reads, 0u, -1);
		} else if (a == PORT_DATA(WB_PORT_B)) {
			*v = m.cs || m.hold_cs ? 0u : 1u << 15;
			if (!*v && txt.handed) {
				txt.began = true;
			} else if (*v && txt.began) {
				tx_push(S13B_TX_PB15_HIGH, 0u, 0u, -1, 0u, -1);
				txt.began = false;
			}
		} else if (a == SPI_TWC(N)) {
			*v = twc_now();
		}
		s13b_access(&tr, op, a, *v);
		return true;
	}
	if (op != FAKE_MMR_WRITE)
		return true;
	if (a == SPI_CTL(N)) {
		if (!(*v & SPI_CTL_EN)) {
			m.nrf = m.ntf = 0;	/* FIFOs reset on disable */
			if (txt.after)
				tx_push(S13B_TX_STOP, 0u, 0u, -1, 0u, -1);
		}
		m.ctl = *v;
	} else if (a == SPI_RXCTL(N)) {
		m.rxctl = *v;
	} else if (a == SPI_TXCTL(N)) {
		m.txctl = *v;
		if (!(*v & SPI_XCTL_EN) && txt.after)
			tx_push(S13B_TX_STOP, 0u, 0u, -1, 0u, -1);
	} else if (a == SPI_IMSK_SET(N)) {
		m.imsk |= *v;
		if ((m.stat & SPI_STAT_TF) && (m.imsk & SPI_STAT_TF))
			m.ilat |= SPI_STAT_TF;
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
	} else if (a == PORT_FER_SET(WB_PORT_A) && (*v & bit)) {
		m.fer = true;
		m.pa4_given_at = fake_core_peek_cycles();
		m.rx_reads = 0;
		m.rx_read_at = m.pa4_given_at;
		txt.handed = true;
		txt.after = true;
		txt.began = false;
		tx_push(S13B_TX_HAND, 0u, 0u, -1, 0u, -1);
	} else if (a == PORT_FER_CLR(WB_PORT_A) && (*v & bit)) {
		m.fer = false;
		m.fer_clr_tick = m.n_ticks;
		m.pa4_back_at = fake_core_peek_cycles();
		tx_push(S13B_TX_TAKE, 0u, 0u, -1, 0u, now_us());
		txt.handed = false;
	} else if (a == PORT_DIR_SET(WB_PORT_A) && (*v & bit)) {
		m.dir = true;
	} else if (a == PORT_DIR_CLR(WB_PORT_A) && (*v & bit)) {
		m.dir = false;
	} else if (a == PORT_DATA_SET(WB_PORT_A) && (*v & bit)) {
		m.data = true;
	} else if (a == PORT_DATA_CLR(WB_PORT_A) && (*v & bit)) {
		m.data = false;
	} else if (a == PORT_INEN_SET(WB_PORT_A) && (*v & bit)) {
		m.inen = true;
	} else if (a == PORT_INEN_CLR(WB_PORT_A) && (*v & bit)) {
		m.inen = false;
	}
	if (m.dir || m.fer)
		check_pa4();
	s13b_access(&tr, op, a, *v);
	return true;
}

static void reset(void)
{
	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(400);		/* 1 us per time read */
	memset(&m, 0, sizeof m);
	m.inen = true;				/* as the pinmux may leave it */
	m.pace = 4;
	tr.on = false;
	txt.on = false;
	txt.handed = false;
	fake_mmr_set_hook(hook, NULL);
}

static void session(unsigned words, const uint32_t *mosi)
{
	m.words = words;
	m.done = 0;
	m.armed = true;
	memset(m.mosi, 0, sizeof m.mosi);
	if (mosi)
		memcpy(m.mosi, mosi, words * 4u);
}

static void up(struct wb_spi_slave *s)
{
	reset();
	CHECK(wb_spi_slave_init(s, N, &pre, PA4, PB15, &cclk400, 100) ==
	      WB_SPI_OK);
	CHECK(m.dir && m.data && !m.inen && !m.fer);
	CHECK(m.ctl == (PRE_CTL | SPI_CTL_EN));
}

/*
 * S-13b.043 over the log: PA4's FER bit is set only with SPI2 enabled and
 * the last PB15 read high (check.py's check_pa4).
 */
static bool handover_ok(void)
{
	const struct fake_mmr_access *l = fake_mmr_log();
	bool pb15 = false, en = false;
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++) {
		const struct fake_mmr_access *e = &l[i];

		if (e->op == FAKE_MMR_READ && e->addr == PORT_DATA(WB_PORT_B))
			pb15 = (e->value & (1u << 15)) != 0u;
		if (e->op != FAKE_MMR_WRITE)
			continue;
		if (e->addr == SPI_CTL(N))
			en = (e->value & SPI_CTL_EN) != 0u;
		if (e->addr == PORT_FER_SET(WB_PORT_A) && (e->value & 0x10u) &&
		    (!en || !pb15))
			return false;
	}
	return m.bad_gpio == 0u;
}

static void test_init(void)
{
	struct wb_spi_slave s;
	struct wb_spi_config c = pre;

	up(&s);
	CHECK(handover_ok());
	/* PA4's input is off before it is driven */
	CHECK(fake_mmr_log()[0].addr == PORT_INEN_CLR(WB_PORT_A));

	reset();
	c.ctl = PRE_CTL | SPI_CTL_MSTR;
	CHECK(wb_spi_slave_init(&s, N, &c, PA4, PB15, &cclk400, 100) ==
	      WB_SPI_ERR_ARG);
	c.ctl = PRE_CTL & ~SPI_CTL_FCEN;
	CHECK(wb_spi_slave_init(&s, N, &c, PA4, PB15, &cclk400, 100) ==
	      WB_SPI_ERR_ARG);
	c = pre;
	c.sel = 1;
	CHECK(wb_spi_slave_init(&s, N, &c, PA4, PB15, &cclk400, 100) ==
	      WB_SPI_ERR_ARG);
	CHECK(wb_spi_slave_init(&s, N, &pre, PA4, PA4, &cclk400, 100) ==
	      WB_SPI_ERR_ARG);
	CHECK(fake_mmr_count() == 0u);
}

/* The bytes the master's words make, in memory order. */
static uint8_t want_byte(size_t i)
{
	return (uint8_t)(m.mosi[i / 4u] >> (8u * (i % 4u)));
}

static void fill_words(unsigned words)
{
	static uint32_t w[MAX_WORDS];
	unsigned i;

	for (i = 0; i < words; i++)
		w[i] = 0x9E3779B9u * (i + 1u) ^ (i << 7);
	session(words, w);
}

/*
 * A request session for wb_spi_slave_request(): the header's second word is
 * the session's length in bytes, its third the further bytes'; the callback
 * spends `cb_accesses` register accesses (time the master runs on) and counts
 * its calls.
 */
static unsigned cb_accesses, cb_calls;

static uint64_t hdr_len(void *ctx, const uint8_t *hdr, uint32_t *tail)
{
	unsigned k;

	(void)ctx;
	cb_calls++;
	for (k = 0; k < cb_accesses; k++)
		(void)wb_mmr_read32(SPI_CTL(N));
	*tail = (uint32_t)hdr[8] | (uint32_t)hdr[9] << 8 |
		(uint32_t)hdr[10] << 16 | (uint32_t)hdr[11] << 24;
	return (uint32_t)hdr[4] | (uint32_t)hdr[5] << 8 |
	       (uint32_t)hdr[6] << 16 | (uint32_t)hdr[7] << 24;
}

/* The master's session: `words` words, the header saying total and tail. */
static void req(unsigned words, uint32_t total, uint32_t tail)
{
	fill_words(words);
	m.mosi[1] = total;
	m.mosi[2] = tail;
}

/* A slave's room as the board has it: a request's header 20 bytes into a
 * 32-byte line (rxbuf, S09_DMA_LEAD), so that its payload starts a line. */
static _Alignas(32) uint8_t room[0x10000u + 128u];
#define RX (room + 20u)

/* Listen (quad or single) for a header into RX, the trace from here; wait
 * to hear the first word. */
static void listen_heard(const struct wb_spi_slave *s, bool quad)
{
	unsigned i;

	s13b_start(&tr);
	CHECK(wb_spi_slave_listen(s, RX, 12u, quad, 1000) == WB_SPI_OK);
	for (i = 0; i < 1000u && !wb_spi_slave_heard(s); i++)
		;
	CHECK(wb_spi_slave_heard(s));
}

static bool slave_fault_is(const struct wb_spi_slave *s, int32_t rc,
			   uint32_t stage, uint32_t left)
{
	struct wb_spi_fault f;

	wb_spi_slave_last_fault(s, &f);
	if (f.rc == rc && f.stage == stage && f.left == left)
		return true;
	printf("  fault: rc %ld stage %lu left %lu stat 0x%08lx\n", (long)f.rc,
	       (unsigned long)f.stage, (unsigned long)f.left,
	       (unsigned long)f.stat);
	return false;
}

static bool rx_is(size_t n)
{
	size_t j;

	for (j = 0; j < n; j++)
		if (RX[j] != want_byte(j))
			return false;
	return true;
}

/*
 * S-13b.083, S-09.011: a request in two parts, the header and then the rest
 * its length gives, by DMA9 with PA4 handed over throughout, the master held
 * between them by the receive FIFO's watermark; at the end PA4 back, EN
 * cleared, REN cleared. Payloads of every size a request has, from the
 * header alone; the trace check_bridge_dma's slave_receive.
 */
static void test_request(void)
{
	static const uint32_t lens[] = { 12u, 16u, 20u, 76u, 12u + 4096u,
					 12u + 0x8000u + 8u, 12u + 0x10000u + 4u };
	char name[64];
	size_t kept = 0, i;

	for (i = 0; i < ARRAY_LEN(lens); i++) {
		struct wb_spi_slave s;
		const uint32_t parts[2] = { 12u, lens[i] - 12u };

		up(&s);
		req(lens[i] / 4u, lens[i], 0u);
		cb_accesses = 0;
		memset(room, 0xCC, sizeof(room));
		listen_heard(&s, false);
		CHECK(m.fer && m.ctl == REQUEST_PRE && m.rxctl == 0x00010015u);
		CHECK(wb_spi_slave_request(&s, sizeof(room) - 20u, hdr_len, NULL, &kept,
					   1000) == WB_SPI_OK);
		CHECK(kept == lens[i] && rx_is(kept));
		CHECK(RX[kept] == 0xCC && room[19] == 0xCC);
		CHECK(!m.fer && !(m.ctl & SPI_CTL_EN) && !(m.rxctl & SPI_XCTL_EN) && !m.dma_on[1]);
		CHECK(m.rors == 0u && m.done == lens[i] / 4u);
		snprintf(name, sizeof name, "slave_receive_%lu", (unsigned long)lens[i]);
		CHECK(s13b_ok(&tr, name, S13B_SLAVE_RECEIVE, 0u, parts, lens[i] > 12u ? 2u : 1u));
		CHECK(slave_fault_is(&s, 0, 0u, 0u));
		(void)wb_mmr_read32(SPI_STAT(N));
		CHECK(m.sessions == 1u && !m.cs);
		CHECK(handover_ok());
	}
}

/*
 * The one gap inside a session, the length callback after the header: the
 * master, clocking as fast as it may, is held by the receive FIFO's
 * watermark while DMA9 waits for the rest's array, and nothing is lost. The
 * same gap against a master nothing holds overruns the FIFO: the gap the
 * watermark covers. In quad I/O as in single.
 */
static void test_request_gap(void)
{
	struct wb_spi_slave s;
	size_t kept = 0;
	unsigned q;

	for (q = 0; q < 2u; q++) {
		up(&s);
		req(3u + 64u, 12u + 256u, 0u);
		m.pace = 1;
		cb_accesses = 300;
		cb_calls = 0;
		listen_heard(&s, q != 0u);
		CHECK(wb_spi_slave_request(&s, 512u, hdr_len, NULL, &kept, 1000) == WB_SPI_OK);
		CHECK(cb_calls == 1u && kept == 12u + 256u && rx_is(kept));
		CHECK(m.rors == 0u && m.top <= FIFO);
		CHECK(s13b_ok(&tr, q ? "slave_receive_gap_quad" : "slave_receive_gap",
			      S13B_SLAVE_RECEIVE, 0u, (const uint32_t[]){ 12u, 256u }, 2u));
	}

	up(&s);
	req(3u + 64u, 12u + 256u, 0u);
	m.pace = 2;
	m.free_run = true;
	cb_accesses = 300;
	listen_heard(&s, false);
	CHECK(wb_spi_slave_request(&s, 512u, hdr_len, NULL, &kept, 1000) != WB_SPI_OK);
	CHECK(m.rors > 0u);
	CHECK(!m.fer && !m.dma_on[1]);
	cb_accesses = 0;
}

/*
 * The rest past the room, received into one place and dropped (S-09.037);
 * S-09.021's further bytes, sent or ended by the master within them (the
 * request whole), or the master's end before them (not whole); a refused
 * length.
 */
static void test_request_rest(void)
{
	struct wb_spi_slave s;
	size_t kept = 0;

	/* longer than the room: kept, then dropped */
	up(&s);
	req(3u + 40u, 12u + 160u, 0u);
	m.pace = 1;
	memset(room, 0xCC, sizeof(room));
	listen_heard(&s, true);
	CHECK(wb_spi_slave_request(&s, 64u, hdr_len, NULL, &kept, 1000) == WB_SPI_OK);
	CHECK(kept == 64u && m.done == 43u && rx_is(kept) && RX[64] == 0xCC);
	CHECK(s13b_ok(&tr, "slave_receive_dropped", S13B_SLAVE_RECEIVE, 0u,
		      (const uint32_t[]){ 12u, 160u }, 2u));

	/* the further bytes sent: the rest's last part */
	up(&s);
	req(3u + 4u + 2u, 12u + 16u, 8u);
	listen_heard(&s, false);
	CHECK(wb_spi_slave_request(&s, 512u, hdr_len, NULL, &kept, 1000) == WB_SPI_OK);
	CHECK(kept == 28u && m.done == 9u && rx_is(kept));
	CHECK(s13b_ok(&tr, "slave_receive_further", S13B_SLAVE_RECEIVE, 0u,
		      (const uint32_t[]){ 12u, 24u }, 2u));
	(void)wb_mmr_read32(SPI_STAT(N));
	CHECK(m.sessions == 1u);

	/* the master ends the session in them: the request stands, at once */
	reset();
	CHECK(wb_spi_slave_init(&s, N, &pre, PA4, PB15, &cclk400, 1000000u) == WB_SPI_OK);
	req(3u + 4u + 2u, 12u + 16u, 8u);
	m.quit_after = 7u;
	listen_heard(&s, false);
	CHECK(wb_spi_slave_request(&s, 512u, hdr_len, NULL, &kept, 1000) == WB_SPI_OK);
	CHECK(kept == 28u && rx_is(kept) && !m.fer && !m.dma_on[1]);
	CHECK(fake_core_peek_cycles() < 400u * 10000u);
	CHECK(slave_fault_is(&s, 0, 0u, 0u));

	/* ... before them: not whole */
	reset();
	CHECK(wb_spi_slave_init(&s, N, &pre, PA4, PB15, &cclk400, 1000000u) == WB_SPI_OK);
	req(3u + 4u + 2u, 12u + 16u, 8u);
	m.quit_after = 6u;
	listen_heard(&s, false);
	CHECK(wb_spi_slave_request(&s, 512u, hdr_len, NULL, &kept, 1000) == WB_SPI_TIMEOUT);
	CHECK(kept == 12u && !m.fer);
	CHECK(fake_core_peek_cycles() < 400u * 10000u);
	CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_MASTER_END, 1u));

	/* a dropped payload longer than a part (S-13b.083): the kept bytes
	 * and the dropped ones to the limit, then the rest in a part of its
	 * own, into the same place */
	up(&s);
	req(0x40040u / 4u, 0x40040u, 0u);
	m.pace = 1;
	memset(room, 0xCC, sizeof(room));
	listen_heard(&s, false);
	CHECK(wb_spi_slave_request(&s, 64u, hdr_len, NULL, &kept, 1000) == WB_SPI_OK);
	CHECK(kept == 64u && m.done == 0x40040u / 4u && rx_is(kept) && RX[64] == 0xCC);
	CHECK(s13b_ok(&tr, "slave_receive_two_drops", S13B_SLAVE_RECEIVE, 0u,
		      (const uint32_t[]){ 12u, 0x3FFE0u, 0x54u }, 3u));
	CHECK(wb_spi_rx_array.n == 2u && wb_spi_rx_array.d[0].mod == 0u &&
	      wb_spi_rx_array.bytes[0] + wb_spi_rx_array.bytes[1] == 0x54u);
	(void)wb_mmr_read32(SPI_STAT(N));
	CHECK(m.sessions == 1u && m.rors == 0u);

	/* a malformed write's length, near 4 GiB (S-09.037): its first part
	 * the kept bytes and dropped ones to the limit, into one place; the
	 * master that sends a little of it and ends the session ends it */
	up(&s);
	req(3u + 40u, 0xFFFFFFF0u, 0u);
	m.quit_after = 43u;
	listen_heard(&s, false);
	CHECK(wb_spi_slave_request(&s, 64u, hdr_len, NULL, &kept, 1000) == WB_SPI_TIMEOUT);
	{
		struct wb_spi_fault f;

		wb_spi_slave_last_fault(&s, &f);
		CHECK(f.rc == WB_SPI_TIMEOUT && f.stage == WB_SPI_STAGE_MASTER_END);
	}
	/* the kept 52 bytes (a whole line, then 20 through their own
	 * buffer), 12 dropped a word at a time, then 32 bytes a word to the
	 * part's limit */
	CHECK(wb_spi_rx_array.n == 4u && wb_spi_rx_array.bytes[0] == 32u &&
	      wb_spi_rx_array.bytes[1] == 20u && wb_spi_rx_array.d[2].mod == 0u &&
	      wb_spi_rx_array.bytes[2] == 12u && wb_spi_rx_array.d[3].mod == 0u &&
	      wb_spi_rx_array.bytes[3] == 0x3FFE0u - 52u - 12u &&
	      (wb_spi_rx_array.d[3].cfg & DMA_CFG_MSIZE_MASK) == 5u << DMA_CFG_MSIZE_SHIFT);
	CHECK(kept == 12u && !m.fer);

	/* a length shorter than the header: refused, PA4 back */
	up(&s);
	req(3u, 8u, 0u);
	listen_heard(&s, false);
	CHECK(wb_spi_slave_request(&s, 64u, hdr_len, NULL, &kept, 1000) == WB_SPI_ERR_ARG);
	CHECK(slave_fault_is(&s, WB_SPI_ERR_ARG, WB_SPI_STAGE_REFUSED, 0u));
	CHECK(!m.fer && kept == 12u && !m.dma_on[1] && !(m.ctl & SPI_CTL_EN));

	/* arguments */
	up(&s);
	CHECK(wb_spi_slave_listen(&s, RX, 10u, false, 1000) == WB_SPI_ERR_ARG);
	CHECK(!m.fer);
}

/*
 * A request that does not come whole (S-09.013): the master stops within the
 * header or the rest and ends the session (PB15 high: at once), or stops and
 * holds PC11 low (a word's bound); each with PA4 back, DMA9 disabled, EN
 * clear, the words that came, and its record.
 */
static void test_request_fails(void)
{
	struct wb_spi_slave s;
	size_t kept = 0;
	unsigned q;

	for (q = 0; q < 2u; q++) {
		reset();
		CHECK(wb_spi_slave_init(&s, N, &pre, PA4, PB15, &cclk400, 500000u) == WB_SPI_OK);
		req(3u + 64u, 12u + 256u, 0u);
		m.quit_after = 20u;
		listen_heard(&s, q != 0u);
		CHECK(wb_spi_slave_request(&s, 512u, hdr_len, NULL, &kept, 500000u) ==
		      WB_SPI_TIMEOUT);
		CHECK((m.pa4_back_at - m.last_word_at) / 400u <= 10u);
		CHECK(kept == 12u && !m.fer && !m.dma_on[1] && !(m.ctl & SPI_CTL_EN));
		CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_MASTER_END, 47u));
		CHECK(handover_ok());
	}

	/* within the header */
	up(&s);
	req(3u, 12u, 0u);
	m.quit_after = 2u;
	listen_heard(&s, false);
	CHECK(wb_spi_slave_request(&s, 64u, hdr_len, NULL, &kept, 1000) == WB_SPI_TIMEOUT);
	CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_MASTER_END, 1u));
	CHECK(kept == 0u && !m.fer);

	/* the master stops and holds PC11 low: a word's bound (100 us) */
	up(&s);
	req(3u + 16u, 12u + 64u, 0u);
	m.words = 10u;
	listen_heard(&s, false);
	m.hold_cs = true;		/* PC11 stays low */
	fake_core_set_cycles(0);
	CHECK(wb_spi_slave_request(&s, 128u, hdr_len, NULL, &kept, 1000000u) ==
	      WB_SPI_TIMEOUT);
	CHECK(fake_core_peek_cycles() < 400u * 1000u);
	CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_NEXT_WORD, 9u));
	CHECK(kept == 12u && !m.fer);

	/* no word of the header after the first: the header's own bound */
	up(&s);
	req(3u, 12u, 0u);
	listen_heard(&s, false);
	m.dma_stall = true;
	CHECK(wb_spi_slave_request(&s, 64u, hdr_len, NULL, &kept, 50u) == WB_SPI_TIMEOUT);
	CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_FIRST_WORD, 2u));

	/* the DMA's error */
	up(&s);
	req(3u, 12u, 0u);
	listen_heard(&s, false);
	m.dma_stat[1] |= DMA_STAT_IRQERR | 3u << DMA_STAT_ERRC_SHIFT;
	CHECK(wb_spi_slave_request(&s, 64u, hdr_len, NULL, &kept, 1000) == WB_SPI_ERR_HW);
	{
		struct wb_spi_fault f;

		wb_spi_slave_last_fault(&s, &f);
		CHECK(f.rc == WB_SPI_ERR_HW &&
		      f.stage == (WB_SPI_STAGE_DMA_ERR | 3u << WB_SPI_STAGE_ERRC_SHIFT));
	}
}

/*
 * S-13b.043, S-09.011: a listen hands PA4 to SPI2 for a request and keeps it
 * there however long the master takes, and however late it comes. With PB15
 * low it hands nothing over.
 */
static void test_listen(void)
{
	struct wb_spi_slave s;
	size_t kept = 0;
	unsigned i, lag;

	up(&s);
	CHECK(wb_spi_slave_listen(&s, RX, 12u, false, 1000) == WB_SPI_OK);
	CHECK(m.fer && m.ctl == REQUEST_PRE && m.dma_on[1]);
	for (i = 0; i < 20000u; i++) {
		CHECK(!wb_spi_slave_heard(&s));
		(void)wb_time_us(&cclk400);
	}
	CHECK(m.fer && pa6_low() && m.bad_gpio == 0u);
	req(3u + 2u, 20u, 0u);
	for (i = 0; i < 100u && !wb_spi_slave_heard(&s); i++)
		;
	CHECK(wb_spi_slave_heard(&s));
	CHECK(wb_spi_slave_request(&s, 64u, hdr_len, NULL, &kept, 1000) == WB_SPI_OK);
	CHECK(kept == 20u && rx_is(kept));
	(void)wb_mmr_read32(SPI_STAT(N));
	CHECK(m.sessions == 1u && !m.cs && !m.fer);
	CHECK(handover_ok());

	/* PB15 low: no hand-over, PA4 a GPIO driven high, DMA9 disabled */
	up(&s);
	m.hold_cs = true;
	CHECK(wb_spi_slave_listen(&s, RX, 12u, false, 50) == WB_SPI_TIMEOUT);
	CHECK(!m.fer && m.pa6_low_ticks == 0u && !m.dma_on[1]);
	CHECK(handover_ok());

	/* every lag of the master after a long idle */
	for (lag = 0; lag <= 16u; lag++) {
		up(&s);
		CHECK(wb_spi_slave_listen(&s, RX, 12u, false, 1000) == WB_SPI_OK);
		for (i = 0; i < 5000u; i++)
			(void)wb_spi_slave_heard(&s);
		m.lag = lag;
		req(3u, 12u, 0u);
		for (i = 0; i < 1000u && !wb_spi_slave_heard(&s); i++)
			;
		CHECK(wb_spi_slave_request(&s, 64u, hdr_len, NULL, &kept, 1000) == WB_SPI_OK);
		(void)wb_mmr_read32(SPI_STAT(N));
		CHECK(m.sessions == 1u && !m.cs);
		CHECK(handover_ok());
	}
}

/*
 * S-13b.040, S-13b.042 over the log since it was cleared, with SPI2_CTL at
 * ctl0 before: MIOM changes only by a write that finds EN clear and leaves
 * it clear. Prints the first fault with its log entry.
 */
static bool miom_ok(uint32_t ctl0)
{
	const struct fake_mmr_access *l = fake_mmr_log();
	uint32_t ctl = ctl0;
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++) {
		const struct fake_mmr_access *e = &l[i];

		if (e->op != FAKE_MMR_WRITE || e->addr != SPI_CTL(N))
			continue;
		if (((e->value ^ ctl) & SPI_CTL_MIOM_MASK) &&
		    ((ctl | e->value) & SPI_CTL_EN)) {
			printf("  MIOM: log[%zu] SPI2_CTL 0x%08lx after 0x%08lx\n",
			       i, (unsigned long)e->value, (unsigned long)ctl);
			return false;
		}
		ctl = e->value;
	}
	return true;
}

/* The index in the log of the first write of v to a (v any if all), or
 * SIZE_MAX. */
static size_t first_write(wb_mmr_addr a, uint32_t v, bool all)
{
	const struct fake_mmr_access *l = fake_mmr_log();
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (l[i].op == FAKE_MMR_WRITE && l[i].addr == a &&
		    (all || l[i].value == v))
			return i;
	return SIZE_MAX;
}

/*
 * S-13b.042, S-13b.040: a quad listen writes 0x00222510 with EN clear,
 * only the receive channel, EN last, then hands PA4 over. The request
 * comes; the next reply's set-up clears EN with MIOM kept before it writes
 * the single-I/O value.
 */
static void test_listen_quad(void)
{
	static const uint8_t tx[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	struct wb_spi_slave s;
	size_t quad, en, fer, kept = 0;
	unsigned i;

	up(&s);
	fake_mmr_clear_log();
	s13b_start(&tr);
	CHECK(wb_spi_slave_listen(&s, RX, 12u, true, 1000) == WB_SPI_OK);
	CHECK(m.fer && m.ctl == REQUEST_POST && m.txctl == 0u);
	quad = first_write(SPI_CTL(N), REQUEST_POST & ~SPI_CTL_EN, false);
	en = first_write(SPI_CTL(N), REQUEST_POST, false);
	fer = first_write(PORT_FER_SET(WB_PORT_A), 0u, true);
	CHECK(quad != SIZE_MAX && en != SIZE_MAX && fer != SIZE_MAX);
	CHECK(quad < first_write(SPI_RXCTL(N), 0u, true) &&
	      quad < first_write(SPI_TXCTL(N), 0u, true) &&
	      first_write(SPI_RXCTL(N), 0u, true) < en && en < fer);
	for (i = (unsigned)quad + 1u; i < en; i++)
		CHECK(!(fake_mmr_log()[i].op == FAKE_MMR_WRITE &&
			fake_mmr_log()[i].addr == SPI_CTL(N)));
	CHECK(miom_ok(PRE_CTL | SPI_CTL_EN));
	CHECK(handover_ok());

	req(3u + 2u, 20u, 0u);
	for (i = 0; i < 100u && !wb_spi_slave_heard(&s); i++)
		;
	CHECK(wb_spi_slave_request(&s, 64u, hdr_len, NULL, &kept, 1000) == WB_SPI_OK);
	CHECK(kept == 20u && rx_is(kept));
	CHECK(s13b_ok(&tr, "slave_receive_quad", S13B_SLAVE_RECEIVE, 0u,
		      (const uint32_t[]){ 12u, 8u }, 2u));
	(void)wb_mmr_read32(SPI_STAT(N));
	CHECK(m.sessions == 1u && !m.fer);

	/* the reply, single I/O */
	fake_mmr_clear_log();
	session(2, NULL);
	CHECK(wb_spi_slave_send(&s, tx, sizeof tx, WB_SPI_SLAVE_SESSION, 1000) == WB_SPI_OK);
	CHECK(miom_ok(REQUEST_POST & ~SPI_CTL_EN));
	CHECK(m.ctl == REPLY_PRE && m.done == 2u);
	CHECK(m.miso[0] == 0x04030201u && m.miso[1] == 0x08070605u);
	CHECK(handover_ok());
}

/* A listen given up before a word came: PA4 taken back high, DMA9
 * disabled, SPI2 disabled with its channels off, MIOM left for the next
 * set-up, which changes it with EN clear. */
static void test_unlisten(void)
{
	static const uint8_t tx[8];
	struct wb_spi_slave s;
	unsigned q;

	for (q = 0; q < 2u; q++) {
		up(&s);
		CHECK(wb_spi_slave_listen(&s, RX, 12u, q != 0u, 1000) == WB_SPI_OK);
		fake_mmr_clear_log();
		wb_spi_slave_unlisten(&s);
		CHECK(!m.fer && m.dir && m.data && !m.dma_on[1]);
		CHECK(!(m.ctl & SPI_CTL_EN) && m.rxctl == 0u && m.txctl == 0u);
		CHECK(handover_ok());
		session(2, NULL);
		CHECK(wb_spi_slave_send(&s, tx, sizeof tx, WB_SPI_SLAVE_SESSION, 1000) ==
		      WB_SPI_OK);
		CHECK(miom_ok(q ? REQUEST_POST : REQUEST_PRE));
		CHECK(m.ctl == REPLY_PRE);
	}
}

/*
 * check_reply_setup's set-up of the reply in tr: the writes up to the
 * hand-over of PA4, by S-13b.044's names, then DMA8's done status and the
 * unmask of TF as they came; the values as written.
 */
static enum s13b_step setup_order[64];

static struct s13b_reply_setup setup_of(uint32_t words, enum s13b_link link, bool tf_end)
{
	struct s13b_reply_setup s = { link, 0u, 0u, 0u, 0u, words, 0u, tf_end, setup_order, 0u };
	bool handed = false, done = false, unmasked = false;
	size_t i;

	for (i = 0; i < tr.n && s.norder < ARRAY_LEN(setup_order); i++) {
		const struct s13b_ev *e = &tr.e[i];
		int k = -1;

		if (e->op == S13B_DESC && e->ch == 8u)
			s.queued = (uint32_t)((e->bytes - 4u * (uint64_t)words) / 4u);
		if (e->op == S13B_W && strcmp(e->reg, "PORTA_FER_SET") == 0 && (e->val & S13B_PA4))
			handed = true;
		if (!handed && e->op == S13B_W) {
			if (strcmp(e->reg, "SPI2_STAT") == 0) {
				k = S13B_ST_STAT;
			} else if (strcmp(e->reg, "DMA8_CFG") == 0 && (e->val & 1u)) {
				k = S13B_ST_DMA;
			} else if (strcmp(e->reg, "SPI2_TWC") == 0) {
				k = S13B_ST_TWC;
				s.twc = e->val;
			} else if (strcmp(e->reg, "SPI2_TXCTL") == 0 && (e->val & 1u)) {
				k = S13B_ST_TXCTL;
				s.txctl = e->val;
			} else if (strcmp(e->reg, "SPI2_RXCTL") == 0) {
				k = S13B_ST_RXCTL;
				s.rxctl = e->val;
			} else if (strcmp(e->reg, "SPI2_CTL") == 0 && (e->val & 1u)) {
				k = S13B_ST_EN;
				s.ctl = e->val;
			} else if (strcmp(e->reg, "SPI2_IMSK_SET") == 0 && (e->val & S13B_TF)) {
				k = S13B_ST_TF_UNMASK;
			}
		} else if (handed) {
			if (!done && e->op == S13B_R && strcmp(e->reg, "DMA8_STAT") == 0 &&
			    (e->val & 1u)) {
				k = S13B_ST_DMA_DONE;
				done = true;
			} else if (!unmasked && e->op == S13B_W &&
				   strcmp(e->reg, "SPI2_IMSK_SET") == 0 && (e->val & S13B_TF)) {
				k = S13B_ST_TF_UNMASK;
				unmasked = true;
			}
		}
		if (k >= 0)
			setup_order[s.norder++] = (enum s13b_step)k;
	}
	return s;
}

/* The reply just sent, by check_reply_setup and check_slave_tx in C, and
 * dumped for check.py's own. */
static bool reply_checked(const char *name, uint32_t words, enum s13b_link link, bool tf_end)
{
	const struct s13b_reply_setup s = setup_of(words, link, tf_end);
	const bool quad = link == S13B_LINK_QUAD;
	const unsigned got = s13b_reply_setup(&s);
	const unsigned tx_got = s13b_slave_tx(words, txt.e, txt.n, quad, tf_end && !quad);

	s13b_dump_setup(name, &s);
	s13b_dump_tx(name, words, txt.e, txt.n, quad, tf_end && !quad);
	if (got != 0u || tx_got != 0u || txt.full)
		printf("  %s: check_reply_setup 0x%x, check_slave_tx 0x%x over %zu events\n", name,
		       got, tx_got, txt.n);
	return got == 0u && tx_got == 0u && !txt.full;
}

static void trace_reply(void)
{
	fake_mmr_clear_log();
	s13b_start(&tr);
	tx_start();
}

/*
 * S-13b.044, S-13b.083: a reply fed by DMA8, its array the reply's words and
 * the two queued ones, set up with EN clear in S-13b.044's order, EN last,
 * then PA4. On the TF end REN is clear, TF is unmasked once DMA8 is
 * done, and the end is SPI_ILAT.TF: PA4 back first, TF masked, TEN cleared,
 * TF acknowledged, DMA8 disabled, EN left set. On the count end REN is set
 * and the received words are counted. Each trace check_bridge_dma's
 * slave_reply, check_reply_setup's and check_slave_tx's.
 */
static void send_case(enum wb_spi_reply_end end)
{
	static const uint8_t tx_bytes[16] = {
		0x11, 0x01, 0x04, 0x00, 0x00, 0x10, 0x00, 0x00,
		0, 0, 0, 0, 0xEF, 0xBE, 0xAD, 0xDE,
	};
	static const wb_mmr_addr steps[] = {
		SPI_STAT(N), DMA_DSCPTR_CUR(8), SPI_TWC(N), SPI_TXCTL(N), SPI_RXCTL(N),
	};
	const bool fact = end == WB_SPI_END_TF;
	struct wb_spi_slave s;
	size_t at, k;

	up(&s);
	CHECK(wb_spi_slave_reply_end_set(&s, end));
	session(4, NULL);
	trace_reply();
	CHECK(wb_spi_slave_send(&s, tx_bytes, sizeof tx_bytes, WB_SPI_SLAVE_SESSION, 1000) ==
	      WB_SPI_OK);
	CHECK(m.done == 4u && !m.fer);
	CHECK(m.miso[0] == 0x00040111u && m.miso[3] == 0xDEADBEEFu);
	CHECK(!(m.stat & SPI_STAT_TUR) && m.tf_writes == 6u);
	CHECK(m.ctl == REPLY_PRE && !(m.txctl & SPI_XCTL_EN) && !m.dma_on[0]);
	CHECK(s13b_ok(&tr, fact ? "slave_reply_factory" : "slave_reply_count", S13B_SLAVE_REPLY,
		      4u, NULL, 0u));
	CHECK(reply_checked(fact ? "reply_factory" : "reply_count", 4u, S13B_LINK_PRE, fact));
	/* S-13b.044's order: the set-up with EN clear, EN last, then PA4 */
	at = first_write(SPI_CTL(N), REPLY_PRE & ~SPI_CTL_EN, false);
	for (k = 0; k < ARRAY_LEN(steps); k++) {
		const size_t w = first_write(steps[k], 0u, true);

		CHECK(w != SIZE_MAX && w > at);
		at = w;
	}
	CHECK(first_write(DMA_CFG(8), WB_DMA_ARRAY_SEND, false) <
	      first_write(SPI_TWC(N), 0u, true));
	CHECK(fake_mmr_log()[first_write(SPI_TXCTL(N), 0u, true)].value == SPI_TXCTL_REPLY);
	CHECK(fake_mmr_log()[first_write(SPI_RXCTL(N), 0u, true)].value ==
	      (fact ? 0x00010014u : SPI_XCTL_EN));
	CHECK(first_write(SPI_CTL(N), REPLY_PRE, false) > at &&
	      first_write(PORT_FER_SET(WB_PORT_A), 0u, true) >
	      first_write(SPI_CTL(N), REPLY_PRE, false));
	if (fact) {
		/* TF unmasked once, after DMA8's done status; the end's order */
		const size_t give = first_write(PORT_FER_SET(WB_PORT_A), 0u, true);
		const size_t unmask = first_write(SPI_IMSK_SET(N), 0u, true);
		const size_t back = first_write(PORT_FER_CLR(WB_PORT_A), 0u, true);
		const size_t mask = first_write(SPI_IMSK_CLR(N), SPI_STAT_TF, false);
		size_t later_mask = SIZE_MAX, ten = SIZE_MAX, ack = SIZE_MAX, latch = SIZE_MAX,
		       off = SIZE_MAX, i, unmasks = 0;
		const struct fake_mmr_access *l = fake_mmr_log();

		for (i = 0; i < fake_mmr_count(); i++) {
			if (l[i].op != FAKE_MMR_WRITE || i < back)
				continue;
			if (l[i].addr == SPI_IMSK_CLR(N) && later_mask == SIZE_MAX)
				later_mask = i;
			if (l[i].addr == SPI_TXCTL(N) && ten == SIZE_MAX)
				ten = i;
			if (l[i].addr == SPI_STAT(N) && ack == SIZE_MAX)
				ack = i;
			if (l[i].addr == SPI_ILAT_CLR(N) && latch == SIZE_MAX)
				latch = i;
			if (l[i].addr == DMA_CFG(8) && off == SIZE_MAX)
				off = i;
		}
		for (i = 0; i < fake_mmr_count(); i++)
			if (l[i].op == FAKE_MMR_WRITE && l[i].addr == SPI_IMSK_SET(N))
				unmasks++;
		CHECK(unmasks == 1u && unmask > give && l[unmask].value == SPI_STAT_TF);
		CHECK(mask < give);	/* masked through the set-up */
		CHECK(back > unmask && later_mask > back && ten > later_mask && ack > ten &&
		      latch > ten && off > ack && off > latch && off != SIZE_MAX);
		CHECK(l[ten].value == (SPI_TXCTL_REPLY & ~SPI_XCTL_EN));
		CHECK(l[ack].value == SPI_STAT_TF && m.imsk == 0u && m.ilat == 0u);
		CHECK(m.rx_reads == 0u && !(m.rxctl & SPI_XCTL_EN));
	} else {
		CHECK(m.rx_reads == 4u);
	}
	(void)wb_mmr_read32(SPI_STAT(N));
	CHECK(m.sessions == 1u);
	CHECK(handover_ok());

	/* the arrays: the reply's buffer, then the two queued words */
	CHECK(wb_spi_tx_array.n == 2u && wb_spi_tx_array.bytes[0] == 16u &&
	      wb_spi_tx_array.bytes[1] == 8u);

	/* arguments */
	CHECK(wb_spi_slave_send(&s, tx_bytes, 0, WB_SPI_SLAVE_SESSION, 200) == WB_SPI_ERR_ARG);
	CHECK(wb_spi_slave_send(&s, tx_bytes, 6, WB_SPI_SLAVE_SESSION, 200) == WB_SPI_ERR_ARG);
}

static void test_send(void)
{
	struct wb_spi_slave s;
	bool changed = true;

	/* the build's end, the TF end unless REPLY_END=count */
	up(&s);
	CHECK(wb_spi_slave_reply_end(&s, &changed) == WB_SPI_REPLY_END_BUILD && !changed);
	send_case(WB_SPI_END_TF);
	send_case(WB_SPI_END_COUNT);
}

/*
 * A master slow to begin, with SPI_TWC counting the word the DMA has
 * loaded before any clock (S-13b.045): the reply waits for the master on
 * either end; only PB15 low or a received word says it has begun.
 */
static void test_send_slow_master(void)
{
	static const uint8_t tx_bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	unsigned e;

	for (e = 0; e < 2u; e++) {
		struct wb_spi_slave s;

		up(&s);
		CHECK(wb_spi_slave_reply_end_set(&s, (enum wb_spi_reply_end)e));
		session(2, NULL);
		m.twc_ahead = true;
		m.lag = 200;
		trace_reply();
		CHECK(wb_spi_slave_send(&s, tx_bytes, sizeof tx_bytes, WB_SPI_SLAVE_SESSION,
					100000) == WB_SPI_OK);
		CHECK(m.done == 2u && m.miso[0] == 0x04030201u && m.miso[1] == 0x08070605u);
		CHECK(reply_checked(e == 0u ? "reply_factory_slow" : "reply_count_slow", 2u,
				    S13B_LINK_PRE, e == 0u));
		CHECK(slave_fault_is(&s, 0, 0u, 0u));
	}
}

/* After the link: quad I/O (S-13b.042), no receive channel, the end TF
 * once DMA8 has reported done, on either end's setting. */
static void test_send_quad(void)
{
	uint8_t tx[24];
	struct wb_spi_slave s;
	unsigned i;

	for (i = 0; i < sizeof tx; i++)
		tx[i] = (uint8_t)(i * 7u);
	up(&s);
	CHECK(wb_spi_reconfigure(&s.spi, POST_CTL, 0) == WB_SPI_OK);
	CHECK(wb_spi_slave_reply_end_set(&s, WB_SPI_END_COUNT));
	session(6, NULL);
	m.pace = 1;
	trace_reply();
	CHECK(wb_spi_slave_send(&s, tx, sizeof tx, WB_SPI_SLAVE_SESSION, 1000) == WB_SPI_OK);
	CHECK(m.done == 6u && !m.fer && !(m.stat & SPI_STAT_TUR));
	for (i = 0; i < 6u; i++)
		CHECK(m.miso[i] == ((uint32_t)tx[4 * i] | (uint32_t)tx[4 * i + 1] << 8 |
				    (uint32_t)tx[4 * i + 2] << 16 | (uint32_t)tx[4 * i + 3] << 24));
	CHECK(!(m.rxctl & SPI_XCTL_EN) && m.rxctl == 0u && m.ctl == REPLY_POST);
	CHECK(s13b_ok(&tr, "slave_reply_quad", S13B_SLAVE_REPLY, 6u, NULL, 0u));
	CHECK(reply_checked("reply_quad", 6u, S13B_LINK_QUAD, false));
	CHECK(handover_ok() && m.imsk == 0u && m.ilat == 0u);

	/* S-13b.045: an underrun gives the session up at once */
	up(&s);
	CHECK(wb_spi_reconfigure(&s.spi, POST_CTL, 0) == WB_SPI_OK);
	session(6, NULL);
	m.force_tur = true;
	CHECK(wb_spi_slave_send(&s, tx, sizeof tx, WB_SPI_SLAVE_SESSION, 1000) == WB_SPI_ERR_HW);
	CHECK(!m.fer && m.ntf == 0u && m.done < 6u && !m.dma_on[0]);
	CHECK(m.ctl == (REPLY_POST & ~SPI_CTL_EN));	/* disabled, emptied */
}

/* The slave's fault record at each failure exit, cleared by a session's
 * start. */
static void test_slave_faults(void)
{
	static const uint8_t tx[12];
	struct wb_spi_slave s;
	struct wb_spi_fault f;
	size_t kept;

	/* PB15 never high */
	up(&s);
	m.hold_cs = true;
	CHECK(wb_spi_slave_listen(&s, RX, 12u, false, 50) == WB_SPI_TIMEOUT);
	CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_MASTER_READY, 0u));
	m.hold_cs = false;
	CHECK(wb_spi_slave_listen(&s, RX, 12u, false, 50) == WB_SPI_OK);
	CHECK(slave_fault_is(&s, 0, 0u, 0u));
	wb_spi_slave_unlisten(&s);
	CHECK(slave_fault_is(&s, 0, 0u, 0u));

	/* a request with no listen before it */
	up(&s);
	CHECK(wb_spi_slave_request(&s, 64u, hdr_len, NULL, &kept, 50) == WB_SPI_ERR_ARG);

	/* a reply's underrun: on the count end the words not counted, on the
	 * factory's SPI_TWC */
	up(&s);
	CHECK(wb_spi_slave_reply_end_set(&s, WB_SPI_END_COUNT));
	session(3, NULL);
	m.force_tur = true;
	CHECK(wb_spi_slave_send(&s, tx, sizeof(tx), WB_SPI_SLAVE_SESSION, 1000) == WB_SPI_ERR_HW);
	CHECK(slave_fault_is(&s, WB_SPI_ERR_HW, WB_SPI_STAGE_TX_ERR, 3u));
	up(&s);
	session(3, NULL);
	m.force_tur = true;
	CHECK(wb_spi_slave_send(&s, tx, sizeof(tx), WB_SPI_SLAVE_SESSION, 1000) == WB_SPI_ERR_HW);
	wb_spi_slave_last_fault(&s, &f);
	CHECK(f.rc == WB_SPI_ERR_HW && f.stage == WB_SPI_STAGE_TX_ERR && f.left < 3u);
	CHECK(!m.fer && !m.dma_on[0] && !(m.ctl & SPI_CTL_EN) && m.imsk == 0u);

	/* a reply the master gives up */
	reset();
	CHECK(wb_spi_slave_init(&s, N, &pre, PA4, PB15, &cclk400, 1000000u) == WB_SPI_OK);
	session(4, NULL);
	m.stall = true;
	m.quit = 40;
	CHECK(wb_spi_slave_send(&s, tx, 12, WB_SPI_SLAVE_SESSION, 1000) == WB_SPI_TIMEOUT);
	CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_MASTER_END, 3u));
	CHECK(!m.fer && !m.dma_on[0] && !(m.ctl & SPI_CTL_EN));

	/* a reply that runs out its bound */
	up(&s);
	session(4, NULL);
	m.stall = true;
	CHECK(wb_spi_slave_send(&s, tx, 12, WB_SPI_SLAVE_SESSION, 1000) == WB_SPI_TIMEOUT);
	CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_REPLY_BOUND, 3u));

	/* refused */
	CHECK(wb_spi_slave_send(&s, tx, 6, WB_SPI_SLAVE_SESSION, 200) == WB_SPI_ERR_ARG);
	CHECK(slave_fault_is(&s, WB_SPI_ERR_ARG, WB_SPI_STAGE_ARG, 0u));
}

#define REPLY_WORDS 16387u	/* a plain start's 128-sector read */
#define T_REPLY_US  500000u	/* the bridge's word bound, S09_T_REPLY_US */

/*
 * A reply of `words` words under S-13b.044's set-up, the slave's bounds as
 * on the bridge, to the master of the model at its pace; with lose, the
 * received word of word `lost` is lost. Every word must reach the master
 * whole. Returns how long PA4 stayed low after the master's last word (or
 * its giving up), in microseconds.
 */
static uint64_t reply_hold_on(struct wb_spi_slave *s, enum wb_spi_reply_end end, bool no_tf,
			      unsigned words, bool lose, unsigned lost, int *rc);
static uint64_t reply_again(const struct wb_spi_slave *s, bool no_tf, unsigned words, bool lose,
			    unsigned lost, int *rc);

static uint64_t reply_hold(struct wb_spi_slave *s, unsigned words, bool lose,
			   unsigned lost, int *rc)
{
	return reply_hold_on(s, WB_SPI_END_COUNT, true, words, lose, lost, rc);
}

/* reply_hold() on the end given, a fresh slave; TF never sets with no_tf. */
static uint64_t reply_hold_on(struct wb_spi_slave *s, enum wb_spi_reply_end end, bool no_tf,
			      unsigned words, bool lose, unsigned lost, int *rc)
{
	reset();
	CHECK(wb_spi_slave_init(s, N, &pre, PA4, PB15, &cclk400, T_REPLY_US) ==
	      WB_SPI_OK);
	CHECK(wb_spi_slave_reply_end_set(s, end));
	return reply_again(s, no_tf, words, lose, lost, rc);
}

/* A reply on the slave as it is, its end as it stands. */
static uint64_t reply_again(const struct wb_spi_slave *s, bool no_tf, unsigned words, bool lose,
			    unsigned lost, int *rc)
{
	static uint8_t tx[MAX_WORDS * 4u];
	bool whole = true;
	unsigned i;

	for (i = 0; i < words * 4u; i++)
		tx[i] = (uint8_t)(i * 7u + (i >> 9));
	session(words, NULL);
	m.no_tf = no_tf;
	m.lose = lose;
	m.lost = lost;
	trace_reply();
	*rc = wb_spi_slave_send(s, tx, words * 4u, WB_SPI_SLAVE_SESSION,
				T_REPLY_US);
	for (i = 0; i < m.done; i++)
		whole = whole && m.miso[i] == ((uint32_t)tx[4u * i] |
					       (uint32_t)tx[4u * i + 1u] << 8 |
					       (uint32_t)tx[4u * i + 2u] << 16 |
					       (uint32_t)tx[4u * i + 3u] << 24);
	CHECK(whole);
	CHECK(handover_ok());
	CHECK(!m.fer && m.pa4_back_at >= m.last_word_at);
	return (m.pa4_back_at - m.last_word_at) / 400u;
}

/*
 * S-13b.045, S-09.011: a reply ends at its received-word count. A received
 * word lost to an overrun leaves the count short; the reply then ends from
 * the transmit side, a quiet time after its last word has left the
 * transmit FIFO, well inside the master's T_REPLY for PA6 to rise, and the
 * record says so. A clean reply ends as soon as its last word is in, with
 * the two queued words fed and never clocked.
 */
static void test_reply_ends(void)
{
	static const unsigned at[] = { 0u, REPLY_WORDS / 2u, REPLY_WORDS - 1u };
	struct wb_spi_slave s;
	struct wb_spi_fault f;
	uint64_t us;
	unsigned k;
	int rc;

	us = reply_hold(&s, REPLY_WORDS, false, 0u, &rc);
	printf("  reply of %u words, clean: PA4 low %lu us after the last word\n",
	       REPLY_WORDS, (unsigned long)us);
	CHECK(rc == WB_SPI_OK && m.done == REPLY_WORDS && us <= 10u);
	CHECK(m.tf_writes == REPLY_WORDS + 2u && m.rors == 0u);
	CHECK(slave_fault_is(&s, 0, 0u, 0u));

	for (k = 0; k < ARRAY_LEN(at); k++) {
		us = reply_hold(&s, REPLY_WORDS, true, at[k], &rc);
		printf("  reply of %u words, received word %u lost: PA4 low %lu us after the last word\n",
		       REPLY_WORDS, at[k], (unsigned long)us);
		CHECK(rc == WB_SPI_OK && m.done == REPLY_WORDS);
		CHECK(us >= SPI_REPLY_QUIET_US && us <= SPI_REPLY_QUIET_US + 100u);
		CHECK(us < T_REPLY_US / 100u);
		wb_spi_slave_last_fault(&s, &f);
		CHECK(f.rc == WB_SPI_ERR_HW && f.stage == WB_SPI_STAGE_REPLY_OVERRUN &&
		      f.left == ((REPLY_WORDS - 1u) << 16 | REPLY_WORDS) &&
		      (f.stat & SPI_STAT_ROR));
		CHECK(m.tf_writes == REPLY_WORDS + 2u);
	}

	/* a short reply the same */
	us = reply_hold(&s, 2u, true, 1u, &rc);
	CHECK(rc == WB_SPI_OK && m.done == 2u && us <= SPI_REPLY_QUIET_US + 100u);

	/* a master that gives up part-way (PC11 high): at once */
	reset();
	CHECK(wb_spi_slave_init(&s, N, &pre, PA4, PB15, &cclk400, T_REPLY_US) ==
	      WB_SPI_OK);
	CHECK(wb_spi_slave_reply_end_set(&s, WB_SPI_END_COUNT));
	session(REPLY_WORDS, NULL);
	m.no_tf = true;
	m.quit_after = 1000u;
	{
		static uint8_t tx[REPLY_WORDS * 4u];

		CHECK(wb_spi_slave_send(&s, tx, sizeof(tx), WB_SPI_SLAVE_SESSION,
					T_REPLY_US) == WB_SPI_TIMEOUT);
	}
	us = (m.pa4_back_at - m.last_word_at) / 400u;
	printf("  reply given up by the master after 1000 words: PA4 low %lu us after\n",
	       (unsigned long)us);
	CHECK(us <= 10u && m.done == 1000u);
	CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_MASTER_END,
			     REPLY_WORDS - 1000u));

	/* a master that stops clocking and keeps PC11 low: the slave's own
	 * bound, twice T_REPLY from the last word; no overrun */
	reset();
	CHECK(wb_spi_slave_init(&s, N, &pre, PA4, PB15, &cclk400, T_REPLY_US) ==
	      WB_SPI_OK);
	CHECK(wb_spi_slave_reply_end_set(&s, WB_SPI_END_COUNT));
	session(64u, NULL);
	m.no_tf = true;
	m.words = 40u;
	{
		static uint8_t tx[64u * 4u];

		CHECK(wb_spi_slave_send(&s, tx, sizeof(tx), WB_SPI_SLAVE_SESSION,
					T_REPLY_US) == WB_SPI_TIMEOUT);
	}
	us = (m.pa4_back_at - m.last_word_at) / 400u;
	printf("  reply the master stops clocking after 40 of 64 words: PA4 low %lu us after\n",
	       (unsigned long)us);
	CHECK(us >= 2u * T_REPLY_US && us <= 2u * T_REPLY_US + 100u);
	CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_REPLY_BOUND, 24u));

	/* a master that never starts: the start bound from the hand-over */
	reset();
	CHECK(wb_spi_slave_init(&s, N, &pre, PA4, PB15, &cclk400, T_REPLY_US) ==
	      WB_SPI_OK);
	CHECK(wb_spi_slave_reply_end_set(&s, WB_SPI_END_COUNT));
	session(4u, NULL);
	m.no_tf = true;
	m.stall = true;
	{
		static uint8_t tx[16];

		CHECK(wb_spi_slave_send(&s, tx, sizeof(tx), WB_SPI_SLAVE_SESSION,
					T_REPLY_US) == WB_SPI_TIMEOUT);
	}
	us = (m.pa4_back_at - m.pa4_given_at) / 400u;
	printf("  reply the master never starts: PA4 low %lu us after the hand-over\n",
	       (unsigned long)us);
	CHECK(us >= T_REPLY_US && us <= T_REPLY_US + 100u);
	CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_REPLY_BOUND, 4u));
}

/*
 * S-13b.083, the TF end: a reply of a plain start's read ends on TF
 * within microseconds of its last word, no received word read. Given up as
 * the count end's would be: a master that stops part-way or stops clocking.
 */
static void test_reply_factory(void)
{
	struct wb_spi_slave s;
	bool changed = true;
	uint64_t us;
	int rc;

	us = reply_hold_on(&s, WB_SPI_END_TF, false, REPLY_WORDS, false, 0u, &rc);
	printf("  TF end, reply of %u words: PA4 low %lu us after the last word\n",
	       REPLY_WORDS, (unsigned long)us);
	CHECK(rc == WB_SPI_OK && m.done == REPLY_WORDS && us <= 10u);
	CHECK(m.tf_writes == REPLY_WORDS + 2u && m.rx_reads == 0u && m.rors == 0u);
	CHECK(reply_checked("reply_factory_long", REPLY_WORDS, S13B_LINK_PRE, true));
	CHECK(slave_fault_is(&s, 0, 0u, 0u));
	CHECK(wb_spi_slave_reply_end(&s, &changed) == WB_SPI_END_TF && !changed);

	/* the same on the count end */
	us = reply_hold_on(&s, WB_SPI_END_COUNT, false, REPLY_WORDS, false, 0u, &rc);
	CHECK(rc == WB_SPI_OK && m.done == REPLY_WORDS && us <= 10u);
	CHECK(m.rx_reads == REPLY_WORDS);
	CHECK(reply_checked("reply_count_long", REPLY_WORDS, S13B_LINK_PRE, false));

	/* a master that gives up part-way: at once, the end kept (DMA8 not done) */
	reset();
	CHECK(wb_spi_slave_init(&s, N, &pre, PA4, PB15, &cclk400, T_REPLY_US) == WB_SPI_OK);
	m.quit_after = 1000u;
	(void)reply_again(&s, false, REPLY_WORDS, false, 0u, &rc);
	CHECK(rc == WB_SPI_TIMEOUT && m.done == 1000u);
	CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_MASTER_END, REPLY_WORDS - 1000u));
	CHECK(wb_spi_slave_reply_end(&s, &changed) == WB_SPI_END_TF && !changed);

	/* a master that stops clocking: the bound, the end kept */
	reset();
	CHECK(wb_spi_slave_init(&s, N, &pre, PA4, PB15, &cclk400, T_REPLY_US) == WB_SPI_OK);
	session(64u, NULL);
	m.words = 40u;
	{
		static uint8_t tx[64u * 4u];

		CHECK(wb_spi_slave_send(&s, tx, sizeof(tx), WB_SPI_SLAVE_SESSION,
					T_REPLY_US) == WB_SPI_TIMEOUT);
	}
	us = (m.pa4_back_at - m.last_word_at) / 400u;
	CHECK(us >= 2u * T_REPLY_US && us <= 2u * T_REPLY_US + 100u);
	CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, WB_SPI_STAGE_REPLY_BOUND, 24u));
	CHECK(wb_spi_slave_reply_end(&s, &changed) == WB_SPI_END_TF && !changed);
	CHECK(m.imsk == 0u && m.ilat == 0u && !(m.ctl & SPI_CTL_EN));
}

/*
 * S-13b.083's change: a reply on the TF end whose words were all fed
 * and that saw no TF changes the rest of the run to the count end, once.
 * The master has every word; PA6 rises only at the slave's give-up, which
 * the master meets as a session that did not end (S-09.060's T_REPLY): it
 * lets the session go, and the slave reads PB15 high. A master that waits
 * longer meets the slave's own bound. Either way the next reply is on the
 * count end, and nothing brings the TF end back but a new run.
 */
static void test_reply_change(void)
{
	struct wb_spi_slave s;
	bool changed = false;
	int rc;
	unsigned k;

	for (k = 0; k < 2u; k++) {
		uint64_t us;

		reset();
		CHECK(wb_spi_slave_init(&s, N, &pre, PA4, PB15, &cclk400, T_REPLY_US) ==
		      WB_SPI_OK);
		if (k == 0u)
			m.let_go_us = T_REPLY_US;
		(void)reply_again(&s, true, 131u, false, 0u, &rc);
		us = (m.pa4_back_at - m.last_word_at) / 400u;
		printf("  TF end, no TF, %s: PA4 low %lu us after the last word\n",
		       k == 0u ? "the master lets go at T_REPLY" : "the master waits",
		       (unsigned long)us);
		CHECK(rc == WB_SPI_TIMEOUT && m.done == 131u);
		CHECK(slave_fault_is(&s, WB_SPI_TIMEOUT, k == 0u ? WB_SPI_STAGE_MASTER_END :
				     WB_SPI_STAGE_REPLY_BOUND, 0u));
		CHECK(k == 0u ? us >= T_REPLY_US && us <= T_REPLY_US + 100u :
				us >= 2u * T_REPLY_US && us <= 2u * T_REPLY_US + 100u);
		/* the words all went; the end was not seen */
		CHECK(s13b_slave_tx(131u, txt.e, txt.n, false, true) ==
		      (S13B_TX_END_NO_COUNT | S13B_TX_END_LATE));
		CHECK(wb_spi_slave_reply_end(&s, &changed) == WB_SPI_END_COUNT && changed);
		CHECK(!wb_spi_slave_reply_end_set(&s, WB_SPI_END_TF));
		CHECK(wb_spi_slave_reply_end_set(&s, WB_SPI_END_COUNT));

		/* the next reply: the count end, REN set, words counted */
		m.let_go_us = 0u;
		(void)reply_again(&s, true, 131u, false, 0u, &rc);
		CHECK(rc == WB_SPI_OK && m.done == 131u && m.rx_reads == 131u);
		CHECK(reply_checked(k == 0u ? "reply_after_change" : "reply_after_change_bound",
				    131u, S13B_LINK_PRE, false));

		/* a reply that fails on the count end changes nothing back */
		m.quit_after = 10u;
		(void)reply_again(&s, true, 131u, false, 0u, &rc);
		CHECK(rc == WB_SPI_TIMEOUT);
		CHECK(wb_spi_slave_reply_end(&s, &changed) == WB_SPI_END_COUNT && changed);
		m.quit_after = 0u;
		(void)reply_again(&s, false, 131u, false, 0u, &rc);
		CHECK(rc == WB_SPI_OK && m.rx_reads == 131u);
		CHECK(wb_spi_slave_reply_end(&s, &changed) == WB_SPI_END_COUNT && changed);
	}

	/* a new run (the driver's init) starts on the build's end again */
	CHECK(wb_spi_slave_init(&s, N, &pre, PA4, PB15, &cclk400, T_REPLY_US) == WB_SPI_OK);
	CHECK(wb_spi_slave_reply_end(&s, &changed) == WB_SPI_REPLY_END_BUILD && !changed);
	CHECK(wb_spi_slave_reply_end_set(&s, WB_SPI_END_TF));

	/* a quad reply that sees no TF changes nothing: quad has TF alone */
	reset();
	CHECK(wb_spi_slave_init(&s, N, &pre, PA4, PB15, &cclk400, T_REPLY_US) == WB_SPI_OK);
	CHECK(wb_spi_reconfigure(&s.spi, POST_CTL, 0) == WB_SPI_OK);
	m.let_go_us = T_REPLY_US;
	(void)reply_again(&s, true, 8u, false, 0u, &rc);
	CHECK(rc == WB_SPI_TIMEOUT);
	CHECK(wb_spi_slave_reply_end(&s, &changed) == WB_SPI_END_TF && !changed);
}

/* A request, its reply, the next request: each set up afresh, the reply's
 * queued words never seen as a request's. */
static void test_sessions(void)
{
	static const uint8_t tx[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
	struct wb_spi_slave s;
	size_t kept = 0;
	unsigned k;

	up(&s);
	for (k = 0; k < 4u; k++) {
		req(3u + k, 12u + 4u * k, 0u);
		listen_heard(&s, k % 2u != 0u);
		CHECK(wb_spi_slave_request(&s, 64u, hdr_len, NULL, &kept, 1000) == WB_SPI_OK);
		CHECK(kept == 12u + 4u * k && rx_is(kept));
		(void)wb_mmr_read32(SPI_STAT(N));
		session(2, NULL);
		CHECK(wb_spi_slave_send(&s, tx, sizeof tx, WB_SPI_SLAVE_SESSION, 1000) ==
		      WB_SPI_OK);
		CHECK(m.miso[0] == 0x06070809u && m.miso[1] == 0x02030405u);
		(void)wb_mmr_read32(SPI_STAT(N));
		CHECK(m.sessions == 2u * k + 2u);
	}
	CHECK(m.rors == 0u && handover_ok() && fake_mmr_errors() == 0u);
}

int main(void)
{
	RUN(test_init);
	RUN(test_request);
	RUN(test_request_gap);
	RUN(test_request_rest);
	RUN(test_request_fails);
	RUN(test_listen);
	RUN(test_listen_quad);
	RUN(test_unlisten);
	RUN(test_send);
	RUN(test_send_slow_master);
	RUN(test_send_quad);
	RUN(test_slave_faults);
	RUN(test_reply_ends);
	RUN(test_reply_factory);
	RUN(test_reply_change);
	RUN(test_sessions);
	return check_summary("test_drv_spi_slave");
}

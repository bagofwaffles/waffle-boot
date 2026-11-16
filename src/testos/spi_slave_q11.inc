/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The test OS's slave end for S-13b Q-11: an armed listen's request header
 * and its lapse, the replies under a set-up the monitor chose, and the armed
 * replies, which wb_spi_slave_send() takes.
 *
 * Part of src/drivers/bf70x/spi.c in the test OS's build (waffle/testos.h,
 * wiki/Test-OS.md), included there where it stands rather than compiled
 * alone: it uses the driver's static functions and state, and the order of
 * the driver's functions places the test OS image's code.
 */

bool wb_spi_testos_req_header(int rc, const uint8_t *hdr)
{
	unsigned k;

	if (!testos_armed_listen)
		return false;
	testos_armed_listen = false;
	wb_spi_testos_reqstat[OPXY_REQ_RC] = (uint32_t)rc;
	for (k = 0; k < 3u; k++)
		wb_spi_testos_reqstat[OPXY_REQ_HDR0 + k] = get_word(hdr + 4u * k, 4u);
	if (rc == WB_SPI_OK && testos_armed_quad)
		wb_spi_testos_reqstat[OPXY_REQ_WHOLE]++;
	return true;
}

bool wb_spi_testos_listen_lapsed(const struct wb_spi_slave *s)
{
	if (!testos_armed_listen || !testos_armed_quad || !wb_time_passed(testos_listen_end))
		return false;
	testos_armed_listen = false;
	wb_spi_testos_req = 0u;
	wb_spi_testos_reqstat[OPXY_REQ_FALLBACKS]++;
	(void)slave_end(s, WB_SPI_TIMEOUT, false);	/* PA4 back, then single I/O */
	return true;
}

/* A Q-11 measurement's own DMA feed: one work unit in stop mode, the
 * controller's word size, the interrupt asked for (IRQDONE needs it), SYNC
 * on a transmit. Not the bridge's sessions' set-up (S-13b.083). */
static void dma_xfer(const struct wb_spi *s, struct wb_dma_xfer *x,
		     const uint8_t *buf, size_t n, bool to_mem)
{
	*x = (struct wb_dma_xfer){ 0 };
	x->addr = (uint32_t)(uintptr_t)buf;
	x->msize = (uint8_t)word_log2(s);
	x->psize = x->msize;
	x->count = (uint32_t)(n >> x->msize);
	x->to_mem = to_mem;
	x->irq = true;
	x->sync = !to_mem;
}

#define TESTOS_RXCTL_FACTORY 0x00010014u	/* S-13b.044: REN clear */
#define TESTOS_DMA_MAX 256u

/* A reply under the set-up the monitor chose: S-13b Q-11's measurements 1 and 5. */
static int testos_send(const struct wb_spi_slave *s, const uint8_t *tx,
		       size_t n, unsigned flags, uint32_t start_us)
{
	static _Alignas(32) uint32_t dmabuf[TESTOS_DMA_MAX / 4u];
	const struct wb_spi *p = &s->spi;
	const uint32_t mode = wb_spi_testos_mode;
	const bool multi = (p->ctl & SPI_CTL_MIOM_MASK) != 0;
	const bool ren = !(mode & OPXY_TESTOS_REPLY_NO_REN) && !multi;
	const bool dma = (mode & OPXY_TESTOS_REPLY_DMA) != 0 && n + 8u <= TESTOS_DMA_MAX;
	const unsigned ch = WB_DMA_SPI0_TX + 2u * p->n;
	unsigned pads = (mode & OPXY_TESTOS_REPLY_QUEUE) ? 2u : 0u;
	uint32_t last_seen = 0xFFFFFFFFu;
	size_t out = 0, in = 0;
	bool low = false;
	uint64_t end;
	uint32_t t0;
	int rc;
	unsigned k;

	if (n == 0u || n % 4u != 0u)
		return WB_SPI_ERR_ARG;
	for (k = 0; k < 12u; k++)
		wb_spi_testos_send[k] = 0u;
	wb_spi_testos_trace[0] = 0u;
	session_ctl(s, true, flags);
	begin(p);
	wb_mmr_write32(SPI_ILAT_CLR(p->n), 0xFFFFFFFFu);
	wb_mmr_write32(SPI_IMSK_SET(p->n), SPI_STAT_TF);
	wb_mmr_write32(SPI_TWC(p->n), (uint32_t)(n / 4u));
	wb_mmr_write32(SPI_RXCTL(p->n), ren ? SPI_XCTL_EN :
		       (mode & OPXY_TESTOS_REPLY_NO_REN) ? TESTOS_RXCTL_FACTORY : 0u);
	if (dma) {
		struct wb_dma_xfer x;

		memset(dmabuf, 0, sizeof(dmabuf));
		memcpy(dmabuf, tx, n);
		wb_dcache_flush_range((uint32_t)(uintptr_t)dmabuf, sizeof(dmabuf));
		dma_xfer(p, &x, (const uint8_t *)dmabuf, n + 4u * pads, false);
		(void)wb_dma_start(ch, &x);
		out = n;
		pads = 0u;
	}
	wb_mmr_write32(SPI_TXCTL(p->n), (mode & OPXY_TESTOS_REPLY_TXCTL) ?
		       SPI_TXCTL_REPLY : SPI_XCTL_EN | SPI_XCTL_WCEN);
	wb_spi_testos_send[6] = wb_mmr_read32(SPI_TWC(p->n));
	rc = hand_over(s, flags, start_us);
	t0 = (uint32_t)wb_core_cycles();
	end = wb_time_deadline(p->t, start_us);
	while (rc == WB_SPI_OK) {
		const uint32_t stat = wb_mmr_read32(SPI_STAT(p->n));
		const uint32_t now = (uint32_t)wb_core_cycles() - t0;
		const bool master_low = !wb_gpio_get(s->master_pin);

		{
			const uint32_t seen = stat | (master_low ? 1u << 31 : 0u);

			if (seen != last_seen && wb_spi_testos_trace[0] < OPXY_TESTOS_TRACE_MAX) {
				const uint32_t at = wb_spi_testos_trace[0];

				wb_spi_testos_trace[1u + 2u * at] = now;
				wb_spi_testos_trace[2u + 2u * at] = seen;
				wb_spi_testos_trace[0] = at + 1u;
			}
			last_seen = seen;
		}
		wb_spi_testos_send[0] |= stat;
		wb_spi_testos_send[1] = stat;
		wb_spi_testos_send[7] |= wb_mmr_read32(SPI_ILAT(p->n));
		if (master_low && !low) {
			low = true;
			wb_spi_testos_send[8] = now;
		}
		if (wb_spi_testos_send[9] == 0u && wb_mmr_read32(SPI_TWC(p->n)) == 0u)
			wb_spi_testos_send[9] = now;
		if (wb_spi_testos_send[10] == 0u && (stat & SPI_STAT_TF))
			wb_spi_testos_send[10] = now;
		if (stat & (SPI_STAT_TUR | SPI_STAT_TC)) {
			rc = WB_SPI_ERR_HW;
			break;
		}
		if (!(stat & SPI_STAT_TFF)) {
			if (out < n) {
				wb_mmr_write32(SPI_TFIFO(p->n), get_word(tx + out, 4u));
				out += 4u;
			} else if (pads != 0u) {
				wb_mmr_write32(SPI_TFIFO(p->n), 0u);
				pads--;
			}
		}
		if (ren && !(stat & SPI_STAT_RFE)) {
			(void)wb_mmr_read32(SPI_RFIFO(p->n));
			in += 4u;
		}
		if (out == n && ((stat & SPI_STAT_TF) || (ren && in == n)))
			break;
		if (low && !master_low) {
			rc = WB_SPI_TIMEOUT;	/* S-09.013: the master ended it */
			break;
		}
		if (wb_time_passed(end))
			rc = WB_SPI_TIMEOUT;
	}
	wb_spi_testos_send[2] = (uint32_t)rc;
	wb_spi_testos_send[3] = (uint32_t)out;
	wb_spi_testos_send[4] = (uint32_t)in;
	wb_spi_testos_send[5] = wb_mmr_read32(SPI_TWC(p->n));
	wb_spi_testos_send[11] = wb_mmr_read32(SPI_TXCTL(p->n));
	if (dma)
		wb_dma_stop(ch);
	wb_mmr_write32(SPI_IMSK_CLR(p->n), SPI_STAT_TF);
	wb_mmr_write32(SPI_ILAT_CLR(p->n), 0xFFFFFFFFu);
	return slave_end(s, rc, true);
}

#define TESTOS_FIFO_BYTES 8u		/* S-13b.044: the transmit FIFO, two words */
#define TESTOS_QUIET_US   2000u		/* S-13b Q-11 (1d) */
#define TESTOS_WATCH_US   10000u	/* PB15 watched after a reply ended on TF */
#define TESTOS_TXCTL_TDU  (1u << 8)	/* TDU 1 */
#define TESTOS_TXCTL_TUWM (1u << 16)	/* TUWM 1 */
#define TESTOS_TXCTL      (SPI_XCTL_EN | SPI_XCTL_TI | SPI_XCTL_WCEN | SPI_XCTL_DR_NE | \
			   TESTOS_TXCTL_TDU | TESTOS_TXCTL_TUWM)
_Static_assert(TESTOS_TXCTL == SPI_TXCTL_REPLY, "S-13b.044's SPI2_TXCTL");

/*
 * S-13b Q-11 (1d): SPI_STAT and the cycle counter alone, until SPI_STAT has not
 * changed for TESTOS_QUIET_US since SPIF first changed, or the bound. The
 * results are stored once the loop has ended. Returns the last SPI_STAT.
 */
static uint32_t testos_tight(const struct wb_spi *p, uint32_t t0, uint32_t bound,
			     uint32_t *or_stat)
{
	volatile uint32_t *const q = wb_spi_testos_q11;
	const uint32_t quiet = TESTOS_QUIET_US * p->t->cycles_per_us;
	uint32_t prev = wb_mmr_read32(SPI_STAT(p->n)), seen = prev, changed = 0;
	uint32_t falls = 0, rises = 0, first = OPXY_TESTOS_Q11_NONE;
	uint32_t last = OPXY_TESTOS_Q11_NONE, tf = OPXY_TESTOS_Q11_NONE;
	bool spif = false;

	for (;;) {
		const uint32_t stat = wb_mmr_read32(SPI_STAT(p->n));
		const uint32_t now = (uint32_t)wb_core_cycles() - t0;

		seen |= stat;
		if (stat != prev) {
			if ((stat ^ prev) & SPI_STAT_SPIF) {
				if (stat & SPI_STAT_SPIF)
					rises++;
				else
					falls++;
				if (!spif)
					first = now;
				spif = true;
				last = now;
			}
			prev = stat;
			changed = now;
		}
		if ((stat & SPI_STAT_TF) && tf == OPXY_TESTOS_Q11_NONE)
			tf = now;
		if ((spif && now - changed >= quiet) || now >= bound)
			break;
	}
	*or_stat |= seen;
	q[OPXY_Q11_SPIF_COUNT] = (falls & 0xFFFFu) | (rises & 0xFFFFu) << 16;
	q[OPXY_Q11_SPIF_FIRST] = first;
	q[OPXY_Q11_SPIF_LAST] = last;
	q[OPXY_Q11_TF_STAT] = tf;
	return prev;
}

/*
 * An armed reply (waffle/testos.h): S-13b Q-11's measurements (1a) to (1f). Bits it
 * cannot use are dropped from the mode it records: the DMA past
 * TESTOS_DMA_MAX, TF_LATE without the DMA, TIGHT when the feed does not fit
 * the FIFO before the hand-over. Without EN_LAST the order is testos_send()'s.
 * Ends on TF, on the count received (single I/O with REN), on PB15 high after
 * low, on TUR or TC, or at the bound.
 */
static void testos_dma_start(const struct wb_spi *p, unsigned ch, const uint8_t *tx,
			     size_t n, unsigned pads)
{
	static _Alignas(32) uint32_t buf[TESTOS_DMA_MAX / 4u];
	struct wb_dma_xfer x;

	memset(buf, 0, sizeof(buf));
	memcpy(buf, tx, n);
	dma_xfer(p, &x, (const uint8_t *)buf, n + 4u * pads, false);
	(void)wb_dma_start(ch, &x);	/* writes the buffer back from the cache */
}

static int testos_q11_send(const struct wb_spi_slave *s, const uint8_t *tx,
			   size_t n, unsigned flags, uint32_t start_us, uint32_t want)
{
	const struct wb_spi *p = &s->spi;
	volatile uint32_t *const q = wb_spi_testos_q11;
	const unsigned ch = WB_DMA_SPI0_TX + 2u * p->n;
	const bool dma = (want & OPXY_TESTOS_REPLY_DMA) != 0u && n + 8u <= TESTOS_DMA_MAX;
	const bool en_last = (want & OPXY_TESTOS_REPLY_EN_LAST) != 0u;
	unsigned pads = (want & OPXY_TESTOS_REPLY_QUEUE) ? 2u : 0u;
	/* the feed is whole before the hand-over: the DMA's, or a core feed
	 * that fits the FIFO, written there once EN is set */
	const bool tight = (want & OPXY_TESTOS_REPLY_TIGHT) != 0u &&
			   (dma || (en_last && n + 4u * pads <= TESTOS_FIFO_BYTES));
	const uint32_t mode = want & ~(dma ? 0u : OPXY_TESTOS_REPLY_DMA | OPXY_TESTOS_REPLY_TF_LATE) &
			      ~(tight ? 0u : OPXY_TESTOS_REPLY_TIGHT);
	const bool quad = (mode & OPXY_TESTOS_REPLY_QUAD) != 0u;
	const bool ren = !quad && !(mode & OPXY_TESTOS_REPLY_NO_REN);
	/* S-13b.044: 0x00006510, 0x00206510 in quad I/O */
	const uint32_t single = (p->ctl & ~(SPI_CTL_FCCH | SPI_CTL_FCWM_MASK | SPI_CTL_MIOM_MASK)) |
		((mode & OPXY_TESTOS_REPLY_FCCH) ? SPI_CTL_FCCH | SPI_CTL_FCWM_TX_EMPTY : 0u);
	const uint32_t ctl = single | (quad ? SPI_CTL_MIOM_QUAD : 0u);
	const uint32_t txctl = ((mode & OPXY_TESTOS_REPLY_TXCTL) ? TESTOS_TXCTL : SPI_XCTL_EN | SPI_XCTL_WCEN) &
		~((mode & OPXY_TESTOS_REPLY_NO_TWCEN) ? SPI_XCTL_WCEN : 0u) &
		~((mode & OPXY_TESTOS_REPLY_NO_TTI) ? SPI_XCTL_TI : 0u);
	const uint32_t rxctl = quad ? 0u : ren ? SPI_XCTL_EN : TESTOS_RXCTL_FACTORY;
	uint32_t last_seen = 0xFFFFFFFFu, or_stat = 0u, unmask_at = 0u, ended, stat, t0;
	bool low = false, unmasked = false, dma_done = false;
	size_t out = 0, in = 0;
	uint64_t end;
	unsigned k;
	int rc;

	if (n == 0u || n % 4u != 0u)
		return WB_SPI_ERR_ARG;
	for (k = 0; k < 12u; k++)
		wb_spi_testos_send[k] = 0u;
	wb_spi_testos_trace[0] = 0u;
	for (k = 0; k < OPXY_Q11_SLOTS; k++)
		q[k] = OPXY_TESTOS_Q11_NONE;
	q[OPXY_Q11_MODE] = mode;
	q[OPXY_Q11_WORD0] = get_word(tx, 4u);
	q[OPXY_Q11_SPIF_COUNT] = 0u;

	/* S-13b.040: EN cleared with MIOM kept, then the write that may change MIOM */
	wb_mmr_write32(SPI_CTL(p->n), wb_mmr_read32(SPI_CTL(p->n)) & ~SPI_CTL_EN);
	wb_mmr_write32(SPI_CTL(p->n), ctl);
	if (!en_last)
		wb_mmr_write32(SPI_CTL(p->n), ctl | SPI_CTL_EN);
	begin(p);
	wb_mmr_write32(SPI_ILAT_CLR(p->n), 0xFFFFFFFFu);
	if (!(mode & OPXY_TESTOS_REPLY_TF_LATE)) {
		unmask_at = (uint32_t)wb_core_cycles();
		wb_mmr_write32(SPI_IMSK_SET(p->n), SPI_STAT_TF);
		unmasked = true;
	}
	if (dma && en_last) {
		testos_dma_start(p, ch, tx, n, pads);
		out = n;
		pads = 0u;
	}
	wb_mmr_write32(SPI_TWC(p->n), (uint32_t)(n / 4u));
	if (en_last) {
		/* S-13b.044's order: TXCTL, RXCTL, then EN last */
		wb_mmr_write32(SPI_TXCTL(p->n), txctl);
		wb_mmr_write32(SPI_RXCTL(p->n), rxctl);
		wb_mmr_write32(SPI_CTL(p->n), ctl | SPI_CTL_EN);
		wb_mmr_sync();
		/* the core feed fills the FIFO only once EN is set */
		while ((out < n || pads != 0u) && !(wb_mmr_read32(SPI_STAT(p->n)) & SPI_STAT_TFF)) {
			wb_mmr_write32(SPI_TFIFO(p->n), out < n ? get_word(tx + out, 4u) : 0u);
			if (out < n)
				out += 4u;
			else
				pads--;
		}
	} else {
		wb_mmr_write32(SPI_RXCTL(p->n), rxctl);
		if (dma) {
			testos_dma_start(p, ch, tx, n, pads);
			out = n;
			pads = 0u;
		}
		wb_mmr_write32(SPI_TXCTL(p->n), txctl);
	}
	wb_spi_testos_send[6] = wb_mmr_read32(SPI_TWC(p->n));
	rc = hand_over(s, flags, start_us);
	t0 = (uint32_t)wb_core_cycles();
	end = wb_time_deadline(p->t, start_us);
	ended = rc == WB_SPI_OK ? OPXY_Q11_END_NONE : OPXY_Q11_END_NO_HANDOVER;
	if (unmasked)
		q[OPXY_Q11_TF_UNMASK] = unmask_at - t0;
	if (rc == WB_SPI_OK && tight)
		wb_spi_testos_send[1] = testos_tight(p, t0, start_us * p->t->cycles_per_us, &or_stat);
	while (rc == WB_SPI_OK) {
		const uint32_t now_stat = wb_mmr_read32(SPI_STAT(p->n));
		const uint32_t now = (uint32_t)wb_core_cycles() - t0;
		const bool master_low = !wb_gpio_get(s->master_pin);
		const uint32_t ilat = wb_mmr_read32(SPI_ILAT(p->n));
		const uint32_t seen = now_stat | (master_low ? 1u << 31 : 0u);

		if (seen != last_seen && wb_spi_testos_trace[0] < OPXY_TESTOS_TRACE_MAX) {
			const uint32_t at = wb_spi_testos_trace[0];

			wb_spi_testos_trace[1u + 2u * at] = now;
			wb_spi_testos_trace[2u + 2u * at] = seen;
			wb_spi_testos_trace[0] = at + 1u;
		}
		last_seen = seen;
		or_stat |= now_stat;
		wb_spi_testos_send[1] = now_stat;
		wb_spi_testos_send[7] |= ilat;
		if (master_low && !low) {
			low = true;
			wb_spi_testos_send[8] = now;
			q[OPXY_Q11_PB15_LOW] = now;
		}
		if (q[OPXY_Q11_TWC0] == OPXY_TESTOS_Q11_NONE && wb_mmr_read32(SPI_TWC(p->n)) == 0u) {
			wb_spi_testos_send[9] = now;
			q[OPXY_Q11_TWC0] = now;
		}
		if ((now_stat & SPI_STAT_TF) && q[OPXY_Q11_TF_STAT] == OPXY_TESTOS_Q11_NONE) {
			wb_spi_testos_send[10] = now;
			q[OPXY_Q11_TF_STAT] = now;
		}
		if ((ilat & SPI_STAT_TF) && q[OPXY_Q11_TF_ILAT] == OPXY_TESTOS_Q11_NONE)
			q[OPXY_Q11_TF_ILAT] = now;
		if (dma && !dma_done && (wb_mmr_read32(DMA_STAT(ch)) & DMA_STAT_IRQDONE)) {
			dma_done = true;
			q[OPXY_Q11_DMA_DONE] = now;
			if (!unmasked) {	/* S-13b.044: TF unmasked once the DMA is done */
				wb_mmr_write32(SPI_IMSK_SET(p->n), SPI_STAT_TF);
				unmasked = true;
				q[OPXY_Q11_TF_UNMASK] = (uint32_t)wb_core_cycles() - t0;
			}
		}
		if (now_stat & (SPI_STAT_TUR | SPI_STAT_TC)) {
			rc = WB_SPI_ERR_HW;
			ended = OPXY_Q11_END_ERROR;
			break;
		}
		if (!(now_stat & SPI_STAT_TFF) && (out < n || pads != 0u)) {
			wb_mmr_write32(SPI_TFIFO(p->n), out < n ? get_word(tx + out, 4u) : 0u);
			if (out < n)
				out += 4u;
			else
				pads--;
		}
		if (ren && !(now_stat & SPI_STAT_RFE)) {
			(void)wb_mmr_read32(SPI_RFIFO(p->n));
			in += 4u;
		}
		if (out == n && (now_stat & SPI_STAT_TF)) {
			ended = OPXY_Q11_END_TF;
			break;
		}
		if (ren && out == n && in == n) {
			ended = OPXY_Q11_END_COUNT;
			break;
		}
		if (low && !master_low) {
			q[OPXY_Q11_PB15_HIGH] = now;
			rc = WB_SPI_TIMEOUT;	/* S-09.013: the master ended it */
			ended = OPXY_Q11_END_MASTER;
			break;
		}
		if (wb_time_passed(end)) {
			rc = WB_SPI_TIMEOUT;
			ended = OPXY_Q11_END_BOUND;
		}
	}
	wb_spi_testos_send[0] = or_stat;
	wb_spi_testos_send[2] = (uint32_t)rc;
	wb_spi_testos_send[3] = (uint32_t)out;
	wb_spi_testos_send[4] = (uint32_t)in;

	(void)wb_gpio_gpio(s->ready_pin);	/* S-13b.043: PA4 back first */
	wb_mmr_sync();
	q[OPXY_Q11_TWC] = wb_mmr_read32(SPI_TWC(p->n));
	stat = wb_mmr_read32(SPI_STAT(p->n));
	q[OPXY_Q11_STAT] = stat;
	wb_spi_testos_send[5] = q[OPXY_Q11_TWC];
	wb_spi_testos_send[11] = wb_mmr_read32(SPI_TXCTL(p->n));
	wb_mmr_write32(SPI_CTL(p->n), ctl);	/* EN clear, MIOM kept: the transmit stops */
	if (dma)
		wb_dma_stop(ch);
	if (low && q[OPXY_Q11_PB15_HIGH] == OPXY_TESTOS_Q11_NONE) {
		const uint64_t until = wb_time_deadline(p->t, TESTOS_WATCH_US);

		while (!wb_gpio_get(s->master_pin) && !wb_time_passed(until))
			;
		if (wb_gpio_get(s->master_pin))
			q[OPXY_Q11_PB15_HIGH] = (uint32_t)wb_core_cycles() - t0;
	}
	wb_mmr_write32(SPI_CTL(p->n), single);	/* single I/O, EN found clear */
	wb_mmr_write32(SPI_RXCTL(p->n), 0u);
	wb_mmr_write32(SPI_TXCTL(p->n), 0u);
	wb_mmr_write32(SPI_STAT(p->n), stat & SPI_STAT_W1C);
	wb_mmr_write32(SPI_IMSK_CLR(p->n), SPI_STAT_TF);
	wb_mmr_write32(SPI_ILAT_CLR(p->n), 0xFFFFFFFFu);
	wb_mmr_write32(SPI_CTL(p->n), single | SPI_CTL_EN);	/* enabled again, FIFOs empty */
	wb_mmr_sync();
	q[OPXY_Q11_RC] = (uint32_t)rc;
	q[OPXY_Q11_ENDED] = ended;
	return rc;
}

/*
 * Which reply this is (waffle/testos.h): the set-up of the first five mode
 * bits alone, an armed reply, or the default one (false). A mode with an
 * armed bit applies to armed replies alone.
 */
static bool testos_taken(const struct wb_spi_slave *s, const uint8_t *tx,
			 size_t n, unsigned flags, uint32_t start_us, int *rc)
{
	const uint32_t mode = wb_spi_testos_mode;
	const uint32_t arm = wb_spi_testos_arm;

	testos_reply_mode = 0u;
	if ((mode & OPXY_TESTOS_REPLY_ARMED) == 0u) {
		if (mode == 0u)
			return false;
		testos_reply_mode = mode;
		*rc = testos_send(s, tx, n, flags, start_us);
		testos_reply_mode = 0u;
		return true;
	}
	if (arm & OPXY_TESTOS_ARM_SKIP) {
		wb_spi_testos_arm = arm & ~OPXY_TESTOS_ARM_SKIP;
		return false;
	}
	if (arm == 0u)
		return false;
	wb_spi_testos_arm = arm - 1u;
	*rc = testos_q11_send(s, tx, n, flags, start_us, mode);
	wb_spi_testos_tally[0]++;
	if (*rc == WB_SPI_OK)
		wb_spi_testos_tally[1]++;
	return true;
}

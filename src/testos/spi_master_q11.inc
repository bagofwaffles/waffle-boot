/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The test OS's master transfers for S-13b Q-11: one whole transfer in
 * S-13b.040's enable-last order, with what SPI_STAT showed on the way
 * (wb_spi_testos_send_last(), wb_spi_testos_recv_last()).
 *
 * Part of src/drivers/bf70x/spi.c in the test OS's build (waffle/testos.h,
 * docs/test-os.md), included there where it stands rather than compiled
 * alone: it uses the driver's static functions and state, and the order of
 * the driver's functions places the test OS image's code.
 */

/* SPI_STAT read into seen[] (bf70x_spi.h): fin is TF for a send, RF for a receive. */
static uint32_t testos_seen(const struct wb_spi *s, uint32_t t0, uint32_t fin,
			    uint32_t *seen)
{
	const uint32_t stat = wb_mmr_read32(SPI_STAT(s->n));

	seen[0] |= stat;
	seen[1] = stat;
	if (seen[3] == OPXY_TESTOS_Q11_NONE && (stat & fin))
		seen[3] = (uint32_t)wb_core_cycles() - t0;
	return stat;
}

/* S-13b Q-11 (1c), (1g): one whole transfer in S-13b.040's enable-last order; a
 * receive when tx is NULL. */
static int testos_last(const struct wb_spi *s, uint32_t ctl, uint32_t clk,
		       const uint8_t *tx, uint8_t *rx, size_t n, uint32_t *seen)
{
	const struct wb_spi_config c = { ctl, clk, 0u, 0u };
	const unsigned w = 1u << ((ctl & SPI_CTL_SIZE_MASK) >> SPI_CTL_SIZE_SHIFT);
	const uint32_t xctl = SPI_XCTL_EN | SPI_XCTL_TI | SPI_XCTL_WCEN;
	const uint32_t fin = tx ? SPI_STAT_TF : SPI_STAT_RF;
	const uint32_t clk0 = wb_mmr_read32(SPI_CLK(s->n));
	uint32_t t0;
	size_t i;
	int rc = WB_SPI_OK;

	seen[0] = 0u;
	seen[1] = 0u;
	seen[2] = OPXY_TESTOS_Q11_NONE;
	seen[3] = OPXY_TESTOS_Q11_NONE;
	if (wb_spi_check(s->n, &c) != WB_SPI_OK || n == 0u || n % w != 0u ||
	    n / w > SPI_TWC_MAX || (!tx && !rx))
		return WB_SPI_ERR_ARG;
	wb_mmr_write32(SPI_CTL(s->n), 0u);
	wb_mmr_write32(SPI_CLK(s->n), clk);
	wb_mmr_write32(SPI_CTL(s->n), ctl);	/* EN clear: the write that may change MIOM */
	wb_mmr_write32(SPI_STAT(s->n), SPI_STAT_W1C);
	if (tx) {
		wb_mmr_write32(SPI_TWC(s->n), (uint32_t)(n / w));
		wb_mmr_write32(SPI_RXCTL(s->n), 0u);	/* one channel alone in quad I/O */
		wb_mmr_write32(SPI_TXCTL(s->n), xctl);
	} else {
		wb_mmr_write32(SPI_RWC(s->n), (uint32_t)(n / w));
		wb_mmr_write32(SPI_TXCTL(s->n), 0u);
		wb_mmr_write32(SPI_RXCTL(s->n), xctl);
	}
	t0 = (uint32_t)wb_core_cycles();
	wb_mmr_write32(SPI_CTL(s->n), ctl | SPI_CTL_EN);	/* EN last */
	wb_mmr_sync();
	for (i = 0; rc == WB_SPI_OK && i < n; i += w) {
		const uint32_t bit = tx ? SPI_STAT_TFF : SPI_STAT_RFE;
		const uint64_t end = wb_time_deadline(s->t, s->word_timeout_us);

		while (testos_seen(s, t0, fin, seen) & bit) {
			if (wb_time_passed(end)) {
				rc = WB_SPI_TIMEOUT;
				break;
			}
		}
		if (rc != WB_SPI_OK)
			break;
		if (tx)
			wb_mmr_write32(SPI_TFIFO(s->n), get_word(tx + i, w));
		else
			put_word(rx + i, w, wb_mmr_read32(SPI_RFIFO(s->n)));
	}
	if (rc == WB_SPI_OK)
		seen[2] = (uint32_t)wb_core_cycles() - t0;
	if (rc == WB_SPI_OK && tx) {
		/* as wait_stat(): the bound restarts when the FIFO's level moves */
		uint64_t end = wb_time_deadline(s->t, s->word_timeout_us);
		uint32_t level = UINT32_MAX;

		for (;;) {
			const bool late = wb_time_passed(end);
			const uint32_t stat = testos_seen(s, t0, fin, seen);

			if (stat & SPI_STAT_TF)
				break;
			if ((stat & SPI_STAT_TFS_MASK) != level) {
				level = stat & SPI_STAT_TFS_MASK;
				end = wb_time_deadline(s->t, s->word_timeout_us);
			} else if (late) {
				rc = WB_SPI_TIMEOUT;
				break;
			}
		}
	} else {
		(void)testos_seen(s, t0, fin, seen);
	}
	/* EN cleared with MIOM kept, then MIOM cleared with EN found clear */
	wb_mmr_write32(SPI_CTL(s->n), ctl);
	wb_mmr_write32(SPI_RXCTL(s->n), 0u);
	wb_mmr_write32(SPI_TXCTL(s->n), 0u);
	wb_mmr_write32(SPI_STAT(s->n), seen[1] & SPI_STAT_W1C);
	wb_mmr_write32(SPI_CTL(s->n), 0u);
	wb_mmr_write32(SPI_CLK(s->n), clk0);
	wb_mmr_sync();
	if (rc == WB_SPI_OK && (seen[0] & (SPI_STAT_ROR | SPI_STAT_MF)))
		rc = WB_SPI_ERR_HW;
	return rc;
}

int wb_spi_testos_send_last(const struct wb_spi *s, uint32_t ctl, uint32_t clk,
			    const uint8_t *tx, size_t n, uint32_t *seen)
{
	return testos_last(s, ctl, clk, tx, NULL, n, seen);
}

int wb_spi_testos_recv_last(const struct wb_spi *s, uint32_t ctl, uint32_t clk,
			    uint8_t *rx, size_t n, uint32_t *seen)
{
	return testos_last(s, ctl, clk, NULL, rx, n, seen);
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RTC0: held mode's set-up and the count register (S-13a.070 - S-13a.072,
 * S-04.070). See waffle/drivers/bf70x_rtc.h.
 */
#include "waffle/drivers/bf70x_rtc.h"

#include <stddef.h>

#include "waffle/drivers/mmr.h"

/* A STAT read; it clears SEC, so every read notes a seconds event. */
static uint32_t status(struct wb_rtc *r)
{
	uint32_t s = wb_mmr_read32(RTC0_STAT);

	if (s & RTC_STAT_SEC)
		r->ticked = true;
	return s;
}

/* S-13a.072: no write pending, seen in a STAT read after the last write. */
static int idle(struct wb_rtc *r, const struct wb_time *t)
{
	uint64_t end = wb_time_deadline(t, WB_RTC_EDGE_US);

	for (;;) {
		if (!(status(r) & RTC_STAT_WRPEND))
			return WB_RTC_OK;
		if (wb_time_passed(end))
			return WB_RTC_ERR_TIMEOUT;
	}
}

/*
 * A seconds event after this call, and no write pending: the HRM's moment
 * for a 1 Hz register write. A pending write completes at an edge, so two
 * edges bound the wait.
 */
static int edge(struct wb_rtc *r)
{
	uint64_t end;
	bool seen = false;

	(void)status(r);		/* an older event does not count */
	end = wb_time_deadline(r->t, 2u * WB_RTC_EDGE_US);
	for (;;) {
		uint32_t s = status(r);

		seen = seen || (s & RTC_STAT_SEC) != 0u;
		if (seen && !(s & RTC_STAT_WRPEND))
			return WB_RTC_OK;
		if (wb_time_passed(end))
			return WB_RTC_ERR_TIMEOUT;
	}
}

int wb_rtc_held_start(struct wb_rtc *r, const struct wb_time *t)
{
	uint32_t init;
	int rc;

	r->t = NULL;
	r->ticked = false;
	r->no_tick = false;
	rc = idle(r, t);
	if (rc != WB_RTC_OK)
		return rc;
	/* S-13a.071: CAL from this read */
	init = wb_mmr_read32(RTC0_INIT);
	wb_mmr_write32(RTC0_INIT, init & RTC_INIT_CAL_MASK);
	wb_mmr_write32(RTC0_IEN, 0);
	(void)idle(r, t);
	r->t = t;
	return WB_RTC_OK;
}

int wb_rtc_read(struct wb_rtc *r, uint32_t *clk)
{
	if (r->t == NULL)
		return WB_RTC_ERR_STATE;
	if (!r->ticked && !r->no_tick) {
		uint64_t end = wb_time_deadline(r->t, WB_RTC_EDGE_US);

		while (!(status(r) & RTC_STAT_SEC) && !wb_time_passed(end))
			;
		r->no_tick = !r->ticked;
	} else if (!r->ticked) {
		(void)status(r);
	}
	if (!r->ticked)
		return WB_RTC_ERR_TIMEOUT;
	*clk = wb_mmr_read32(RTC0_CLK);
	return WB_RTC_OK;
}

int wb_rtc_peek(const struct wb_rtc *r, uint32_t *clk)
{
	if (r->t == NULL)
		return WB_RTC_ERR_STATE;
	*clk = wb_mmr_read32(RTC0_CLK);
	return WB_RTC_OK;
}

int wb_rtc_write(struct wb_rtc *r, uint32_t clk)
{
	int rc;

	if (r->t == NULL)
		return WB_RTC_ERR_STATE;
	if ((clk & RTC_CLK_SEC_MASK) > 59u ||
	    (clk & RTC_CLK_MIN_MASK) >> RTC_CLK_MIN_SHIFT > 59u ||
	    (clk & RTC_CLK_HOUR_MASK) >> RTC_CLK_HOUR_SHIFT > 23u)
		return WB_RTC_ERR_VALUE;
	rc = edge(r);
	if (rc != WB_RTC_OK)
		return rc;
	wb_mmr_write32(RTC0_CLK, clk);
	return idle(r, r->t);
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The master's start-up in the front (see fr_start.h). */
#include "fr_start.h"

#include "fr_startlog.h"

/* S-13c.006's 120 ms, counted from the INIT stage's start at the latest. */
#define PANEL_FLOOR_US 120000u

/* S-13b.001: PA8, the panel's reset strap (S-13c.004). */
#define STRAP_PA8 (1u << 8)

static uint64_t now(const struct fr_start *s)
{
	return s->hw->clock->now_us(s->hw->clock->ctx);
}

static void stamp(const struct fr_start *s, struct fr_handoff *ho, enum fr_stamp k,
		  uint64_t us)
{
	(void)s;
	/* The record keeps cycles, as the INIT stage's stamps. */
	ho->stamps[k] = us * FR_CCLK_MHZ;
}

/* No frame from here, before S-07.051's reports (S-13c.022). */
static void poweroff(void *ctx)
{
	struct fr_start *s = ctx;

	if (s->panel_up)
		fr_panel_off(&s->panel);
}

static void draw(struct fr_start *s)
{
	if (s->panel_up)
		fr_panel_frame(&s->panel, fr_screen_row, &s->screen);
}

/* 0x51 {3C FF 02} after the frame's last byte (S-13c.019, S-05.004). */
static void light(struct fr_start *s, struct fr_handoff *ho)
{
	if (!s->panel_up || s->lit)
		return;
	fr_psoc_report(&s->psoc, FR_REPORT_HELD);
	s->lit = true;
	ho->flags |= FR_HO_PANEL_LIT;
	stamp(s, ho, FR_STAMP_LOGO, now(s));
}

enum fr_next fr_start_run(struct fr_start *s, const struct fr_start_hw *hw,
			  struct fr_handoff *ho)
{
	const uint8_t *raw;
	struct fr_log log;

	s->hw = hw;
	s->panel_up = false;
	s->lit = false;
	s->why = 0;
	raw = hw->mem->map(hw->mem->ctx, FR_DESC_ADDR, FR_DESC_MAX);
	s->desc_ok = raw != NULL && fr_desc_parse(raw, FR_DESC_MAX, &s->desc);

	/* S-13c.004-S-13c.013, S-05.004: the panel and the logo frame first,
	 * dark; every wait in it is bounded and the bus is write only. */
	if (hw->panel != NULL) {
		const uint64_t floor_us = ho->stamps[FR_STAMP_INIT_ENTRY] / FR_CCLK_MHZ + PANEL_FLOOR_US;

		fr_panel_begin(&s->panel, hw->panel, hw->clock, ho->dma9_xmod,
			       ho->dma9_ymod, (ho->strap & STRAP_PA8) != 0u ? 1u : 0u);
		/* S-13c.006 whatever DMA9_YMOD's time base: the first stage released
		 * the reset before the INIT stage began, and the counter's count
		 * from there, at FR_CCLK_MHZ per us, never overstates the time. */
		if (s->panel.ready_us < floor_us)
			s->panel.ready_us = floor_us;
		fr_panel_start(&s->panel);
		s->panel_up = true;
		fr_screen_logo(&s->screen);
		draw(s);
		ho->flags |= FR_HO_PANEL_UP;
		ho->panel_reset_ms = s->panel.reset_ms;
	}

	/* S-07.020, S-07.021: the greeting; its failure wait ends when the PSoC
	 * greets, at most 1.5 s. */
	fr_psoc_init(&s->psoc, hw->psoc, hw->clock);
	s->psoc.on_poweroff = poweroff;
	s->psoc.ctx = s;
	if (fr_psoc_greet(&s->psoc) == FR_PSOC_OFF)
		return FR_NEXT_HALT;
	if (!s->psoc.greeted) {
		ho->flags |= FR_HO_GREET_WAITED;
		if (fr_psoc_greet_wait(&s->psoc, FR_GREET_WAIT_US) == FR_PSOC_OFF)
			return FR_NEXT_HALT;
	}
	if (s->psoc.greeted)
		ho->flags |= FR_HO_GREETED;
	stamp(s, ho, FR_STAMP_GREETED, now(s));

	/* S-07.022, S-04.060, then S-07.054's first requests and any charge
	 * wait: the switch is off, the unit only charges, dark. */
	fr_psoc_power_queries(&s->psoc);
	fr_psoc_report(&s->psoc, FR_REPORT_START);
	if (fr_psoc_power_requests(&s->psoc) == FR_PSOC_OFF)
		return FR_NEXT_HALT;
	if (s->psoc.state_known && s->psoc.state == FR_POWER_CHARGE) {
		ho->flags |= FR_HO_CHARGE_WAITED;
		if (fr_psoc_charge_wait(&s->psoc) == FR_PSOC_OFF)
			return FR_NEXT_HALT;
	}

	/* S-05.004: the logo lit as soon as the PSoC can take the message. */
	light(s, ho);

	/* S-07.025, S-07.032: the subscription, then the one key-status reply. */
	fr_psoc_subscribe(&s->psoc);
	ho->flags |= FR_HO_SUBSCRIBED;
	if (fr_psoc_key_status(&s->psoc) == FR_PSOC_OFF)
		return FR_NEXT_HALT;
	stamp(s, ho, FR_STAMP_KEYS, now(s));
	if (s->psoc.keys_replied) {
		size_t n = s->psoc.keys_len < FR_HO_KEYS_MAX ? s->psoc.keys_len : FR_HO_KEYS_MAX;

		ho->flags |= FR_HO_KEYS_REPLIED;
		fr_copy(ho->keys, s->psoc.keys, n);
		ho->key_len = (uint8_t)n;
		/* S-02.080: Power + M4, key 4 alone; no reply is no chord. */
		if (fr_keys_one(s->psoc.keys, s->psoc.keys_len) == FR_KEY_4) {
			s->why = FR_WHY_KEYS;	/* S-02.084, S-15.150 */
			return FR_NEXT_RECOVERY;
		}
	}

	/* S-01.023: the rest of the image as the image tool recorded it. */
	if (!fr_image_check(hw->mem, &s->desc)) {
		s->why = FR_WHY_CHECK;
		return FR_NEXT_RECOVERY;
	}
	/* S-02.095: this start in the selected slot's log, for the main image
	 * to finish; after three that did not finish, recovery. Anything
	 * about the log that is not as expected writes nothing and goes on.
	 * A trial start of this front logs nothing, and a start of the slot
	 * named spends a trial pending in the other (S-02.093). */
	switch (fr_log_step(hw->flash, hw->mem, &log)) {
	case FR_LOG_RECOVERY:
		s->why = FR_WHY_COUNT;
		return FR_NEXT_RECOVERY;
	case FR_LOG_GO_MARKED:
		ho->flags |= FR_HO_START_LOGGED;
		ho->log_slot = log.slot;
		ho->log_entry = log.next;
		break;
	case FR_LOG_GO:
	default:
		break;
	}
	/* The power messages that came while the image was checked and the
	 * log written, before the power state is handed over: a power-off ends
	 * the start here. A greeting or ping is not answered here (S-07.076). */
	if (fr_psoc_poll_power(&s->psoc) == FR_PSOC_OFF)
		return FR_NEXT_HALT;
	ho->power_state = s->psoc.state;
	if (s->psoc.state_known)
		ho->flags |= FR_HO_POWER_KNOWN;
	ho->battery_len = (uint8_t)(s->psoc.batt_len < FR_HO_BATTERY_MAX ?
					    s->psoc.batt_len : FR_HO_BATTERY_MAX);
	fr_copy(ho->battery, s->psoc.batt, ho->battery_len);
	/* The bytes out before the full bootloader opens the UART again. */
	fr_psoc_drain(&s->psoc, 20000u);
	ho->psoc_seq = s->psoc.seq;
	stamp(s, ho, FR_STAMP_HANDOFF, now(s));
	return FR_NEXT_MAIN;
}

void fr_halt(struct fr_start *s, uint32_t us)
{
	const struct fr_clock *c = s->hw->clock;
	const uint64_t end = now(s) + us;

	if (fr_psoc_sleep(&s->psoc, us) == FR_PSOC_OK)
		return;
	/* S-07.051 step 3: powered off, nothing more is handled or sent. */
	while (now(s) < end)
		c->delay_us(c->ctx, 1000u);
}

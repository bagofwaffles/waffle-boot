/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.116: a protected component's confirmation (see waffle/s15_confirm.h).
 */
#include "waffle/s15_confirm.h"

const char s15_confirm_title[] = "Confirm Install";

static bool bit(const uint8_t *set, unsigned k)
{
	return (((unsigned)set[k / 8u] >> (k % 8u)) & 1u) != 0u;
}

static void set_bit(uint8_t *set, unsigned k, bool v)
{
	if (v)
		set[k / 8u] = (uint8_t)(set[k / 8u] | (1u << (k % 8u)));
	else
		set[k / 8u] = (uint8_t)(set[k / 8u] & ~(1u << (k % 8u)));
}

void s15_confirm_lines(const struct s15_confirm_info *info,
		       struct s15_buf out[S15_CONFIRM_LINES],
		       enum s15_style style[S15_CONFIRM_LINES])
{
	const bool first_stage = info->step == S15_STEP_SPIBOOT;
	unsigned i;

	for (i = 0; i < S15_CONFIRM_LINES; i++) {
		s15_buf_init(&out[i]);
		style[i] = S15_ST_TEXT;
	}
	style[0] = S15_ST_HEAD;
	s15_buf_str(&out[0], first_stage ? "Install the first stage?"
					 : "Install the bootloader?");
	style[1] = S15_ST_KV;
	s15_buf_str(&out[1], "Package\t");
	s15_buf_str(&out[1], info->package != NULL ? info->package : "");
	if (info->installed != NULL) {
		s15_buf_str(&out[1], "\tRunning\t");
		s15_buf_str(&out[1], info->installed);
	}
	style[2] = S15_ST_WARN;
	if (first_stage) {
		s15_buf_str(&out[2],
			    "Nothing on the unit can repair a failed write.");
	} else if (info->slot >= 0) {
		s15_buf_str(&out[2], "Slot ");
		s15_buf_dec(&out[2], s15_slot_number((unsigned)info->slot));
		s15_buf_str(&out[2], " is replaced. Recovery remains.");
	} else {
		s15_buf_str(&out[2],
			    "The other slot is replaced. Recovery remains.");
	}
	style[4] = S15_ST_KEYS;
	s15_buf_str(&out[4], "Hold `1` for 5 s to install");
	style[5] = S15_ST_ITEM;
	s15_buf_str(&out[5], "8 Skip\tSkipped when the time runs out");
}

void s15_confirm_bar(struct s15_buf *out, unsigned fill, unsigned left_s)
{
	s15_buf_init(out);
	s15_buf_str(out, "Hold `1`\t");
	s15_buf_dec(out, fill > 100u ? 100u : fill);
	s15_buf_str(out, "%\t");
	s15_buf_dec(out, left_s);
	s15_buf_str(out, " s");
}

static void end(struct s15_confirm *c, enum s15_confirm_choice choice,
		enum s15_confirm_end how)
{
	c->choice = choice;
	c->end = how;
	c->holding = false;
}

void s15_confirm_open(struct s15_confirm *c, const uint8_t *down_now,
		      bool readable, uint32_t now_ms)
{
	for (size_t i = 0; i < sizeof(c->stale); i++)
		c->stale[i] = c->down[i] = down_now != NULL ? down_now[i] : 0u;
	c->opened_ms = now_ms;
	c->held_ms = now_ms;
	c->held_key = 0;
	c->holding = false;
	c->choice = S15_CONFIRM_WAIT;
	c->end = S15_CONFIRM_END_NONE;
	if (!readable)
		end(c, S15_CONFIRM_SKIP, S15_CONFIRM_END_UNREADABLE);
}

/* A knob click (0x0A - 0x0D) confirms nothing: it is the easiest key to
 * press by accident. */
static bool knob_click(unsigned key)
{
	return key >= 0x0au && key <= 0x0du;
}

/* S-07.033's rule: no other of the 72 keys down, the precomp key (0x17,
 * S-07.032) aside. */
static bool alone(const struct s15_confirm *c, unsigned key)
{
	for (unsigned k = 0; k < S15_NKEYS; k++) {
		if (k != key && k != 0x17u && bit(c->down, k))
			return false;
	}
	return true;
}

void s15_confirm_key(struct s15_confirm *c, unsigned key, bool down,
		     uint32_t now_ms)
{
	/* what the time decided before this event stands */
	if (key >= S15_NKEYS || s15_confirm_tick(c, now_ms) != S15_CONFIRM_WAIT)
		return;
	set_bit(c->down, key, down);
	/* A key down when the screen was shown acts after an up and a down. */
	if (bit(c->stale, key)) {
		if (!down)
			set_bit(c->stale, key, false);
		return;
	}
	if (!down) {
		if (c->holding && key == c->held_key)
			c->holding = false;	/* an early release */
		return;
	}
	if (s15_digit(key) == 8u) {
		end(c, S15_CONFIRM_SKIP, S15_CONFIRM_END_KEY);
	} else if (c->holding) {
		if (key != 0x17u)
			c->holding = false;	/* another key down */
	} else if (s15_digit(key) == 1u && !knob_click(key) && alone(c, key)) {
		c->holding = true;
		c->held_key = key;
		c->held_ms = now_ms;
	}
}

enum s15_confirm_choice s15_confirm_tick(struct s15_confirm *c,
					 uint32_t now_ms)
{
	if (c->choice != S15_CONFIRM_WAIT)
		return c->choice;
	/* a hold confirms when it reaches 5 s no later than the 60 s mark,
	 * however late this is asked */
	if (c->holding && now_ms - c->held_ms >= S15_CONFIRM_HOLD_MS &&
	    c->held_ms - c->opened_ms <=
	    S15_CONFIRM_TIMEOUT_MS - S15_CONFIRM_HOLD_MS)
		end(c, S15_CONFIRM_INSTALL, S15_CONFIRM_END_HELD);
	else if (now_ms - c->opened_ms >= S15_CONFIRM_TIMEOUT_MS)
		end(c, S15_CONFIRM_SKIP, S15_CONFIRM_END_TIMEOUT);
	return c->choice;
}

unsigned s15_confirm_fill(const struct s15_confirm *c, uint32_t now_ms)
{
	const uint32_t held = now_ms - c->held_ms;

	if (c->end == S15_CONFIRM_END_HELD)
		return 100;
	if (!c->holding)
		return 0;
	return held >= S15_CONFIRM_HOLD_MS ? 100u :
	       (unsigned)(held * 100u / S15_CONFIRM_HOLD_MS);
}

unsigned s15_confirm_left(const struct s15_confirm *c, uint32_t now_ms)
{
	const uint32_t gone = now_ms - c->opened_ms;

	if (gone >= S15_CONFIRM_TIMEOUT_MS)
		return 0;
	return (unsigned)((S15_CONFIRM_TIMEOUT_MS - gone + 999u) / 1000u);
}

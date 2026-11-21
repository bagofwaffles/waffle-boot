/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.116: the selection of the steps before an install run (see
 * waffle/s15_select.h).
 */
#include "waffle/s15_select.h"

/* The rows in digit order, 2 to 7. */
static const enum s15_step by_digit[] = {
	S15_STEP_OS, S15_STEP_CONTENT, S15_STEP_BLE, S15_STEP_BLE_BOOTLOADER,
	S15_STEP_KEYBOARD, S15_STEP_BOOTLOADER,
};
#define ROWS (sizeof(by_digit) / sizeof(by_digit[0]))

unsigned s15_select_digit(enum s15_step step)
{
	for (unsigned i = 0; i < ROWS; i++) {
		if (by_digit[i] == step)
			return i + 2u;
	}
	return 0;
}

int s15_select_step(unsigned digit)
{
	if (digit < 2u || digit >= 2u + ROWS)
		return -1;
	return (int)by_digit[digit - 2u];
}

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

static void end(struct s15_select *s, enum s15_select_choice choice,
		enum s15_select_end how)
{
	s->choice = choice;
	s->end = how;
	s->holding = false;
}

void s15_select_open(struct s15_select *s, const struct wb_device *dev,
		     uint8_t pickable, const uint8_t *down_now, uint32_t now_ms)
{
	s->dev = dev;
	for (size_t i = 0; i < sizeof(s->stale); i++)
		s->stale[i] = s->down[i] = down_now != NULL ? down_now[i] : 0u;
	s->pickable = pickable;
	s->selected = 0;
	s->last_ms = now_ms;
	s->held_ms = now_ms;
	s->held_key = 0;
	s->holding = false;
	s->prompt = false;
	s->choice = S15_SELECT_WAIT;
	s->end = S15_SELECT_END_NONE;
}

void s15_select_prompt_open(struct s15_select *s, const struct wb_device *dev,
			    const uint8_t *down_now, uint32_t now_ms)
{
	s15_select_open(s, dev, 0u, down_now, now_ms);
	s->prompt = true;
}

/* S-07.033's rule: no other of the device's keys down, the precomp key
 * (S-07.032; 0x17 on the OP-XY, none on the OP-1F, S-15.016) aside. */
static bool alone(const struct s15_select *s, unsigned key)
{
	for (unsigned k = 0; k < s->dev->keys.count; k++) {
		if (k != key && k != s->dev->keys.precomp && bit(s->down, k))
			return false;
	}
	return true;
}

void s15_select_key(struct s15_select *s, unsigned key, bool down,
		    uint32_t now_ms)
{
	const unsigned d = s15_digit(s->dev, key);
	int step;

	/* what the time decided before this event stands */
	if (key >= s->dev->keys.count ||
	    s15_select_tick(s, now_ms) != S15_SELECT_WAIT)
		return;
	set_bit(s->down, key, down);
	s->last_ms = now_ms;
	/* A key down when the screen was shown acts after an up and a down. */
	if (bit(s->stale, key)) {
		if (!down)
			set_bit(s->stale, key, false);
		return;
	}
	if (!down) {
		if (s->holding && key == s->held_key)
			s->holding = false;	/* an early release */
		return;
	}
	if (s->holding) {
		if (key != s->dev->keys.precomp)
			s->holding = false;	/* another key down */
		return;
	}
	step = s15_select_step(d);
	if (d == 8u) {
		end(s, S15_SELECT_CANCEL, S15_SELECT_END_KEY);
	} else if (step >= 0) {
		if ((s->pickable & S15_SELECT_BIT(step)) != 0u)
			s->selected ^= S15_SELECT_BIT(step);
	} else if (d == 1u && !s15_knob_click(s->dev, key) &&
		   (s->selected != 0u || s->prompt) && alone(s, key)) {
		/* a knob click starts no hold: the easiest key to press by
		 * accident */
		s->holding = true;
		s->held_key = key;
		s->held_ms = now_ms;
	}
}

enum s15_select_choice s15_select_tick(struct s15_select *s, uint32_t now_ms)
{
	if (s->choice != S15_SELECT_WAIT)
		return s->choice;
	/* a hold installs once it reaches 5 s, however late this is asked;
	 * the 60 s do not end a hold */
	if (s->holding) {
		if (now_ms - s->held_ms >= S15_HOLD_MS)
			end(s, S15_SELECT_INSTALL, S15_SELECT_END_HELD);
	} else if (now_ms - s->last_ms >= S15_SELECT_TIMEOUT_MS) {
		end(s, S15_SELECT_CANCEL, S15_SELECT_END_TIMEOUT);
	}
	return s->choice;
}

unsigned s15_select_fill(const struct s15_select *s, uint32_t now_ms)
{
	const uint32_t held = now_ms - s->held_ms;

	if (s->end == S15_SELECT_END_HELD)
		return 100;
	if (!s->holding)
		return 0;
	return held >= S15_HOLD_MS ? 100u :
	       (unsigned)(held * 100u / S15_HOLD_MS);
}

unsigned s15_select_hold_left(const struct s15_select *s, uint32_t now_ms)
{
	const uint32_t held = now_ms - s->held_ms;

	if (!s->holding || held >= S15_HOLD_MS)
		return 0;
	return (unsigned)((S15_HOLD_MS - held + 999u) / 1000u);
}

unsigned s15_select_left(const struct s15_select *s, uint32_t now_ms)
{
	const uint32_t gone = now_ms - s->last_ms;

	if (gone >= S15_SELECT_TIMEOUT_MS)
		return 0;
	return (unsigned)((S15_SELECT_TIMEOUT_MS - gone + 999u) / 1000u);
}

/* ---- the rows ---- */

static const char *const why[] = {
	[S15_ROW_OFF] = "Not in this version",
	[S15_ROW_OPTION_OFF] = "Protected writes off",
	[S15_ROW_UP_TO_DATE] = "Up to date",
};

/* "D Label\tDetail\tNo" or "...\tYes"; "D Label\tWhy" for a row that
 * cannot be picked. */
static void step_row(const struct s15_select_row *r, enum s15_step step,
		     bool yes, struct s15_buf *out, enum s15_style *style)
{
	s15_buf_init(out);
	s15_buf_dec(out, s15_select_digit(step));
	s15_buf_str(out, " ");
	s15_buf_str(out, s15_step_label(step));
	s15_buf_str(out, "\t");
	if (r->kind != S15_ROW_PICK) {
		*style = S15_ST_PICK_OFF;
		s15_buf_str(out, why[r->kind]);
		return;
	}
	*style = yes ? S15_ST_PICK_ON : S15_ST_PICK;
	if (r->installed != NULL && r->package != NULL) {
		s15_buf_str(out, r->installed);
		s15_buf_str(out, " > ");
	}
	if (r->package != NULL)
		s15_buf_str(out, r->package);
	s15_buf_str(out, yes ? "\tYes" : "\tNo");
}

/* S-02.043: the slot the bootloader step replaces, or the return to
 * stock, which leaves no recovery of waffle-boot's. */
static void warn_line(const struct s15_select_info *info, struct s15_buf *out)
{
	s15_buf_init(out);
	if (info->stock) {
		s15_buf_str(out, s15_stock_line);
	} else if (info->slot >= 0) {
		s15_buf_str(out, "Slot ");
		s15_buf_dec(out, s15_slot_number((unsigned)info->slot));
		s15_buf_str(out, " is replaced. Recovery remains.");
	} else {
		s15_buf_str(out, "The other slot is replaced. Recovery remains.");
	}
}

/* "8 Cancel", with the countdown while no hold runs. */
static void cancel_row(const struct s15_select *s, uint32_t now_ms,
		       struct s15_buf *out)
{
	s15_buf_init(out);
	s15_buf_str(out, "8 Cancel");
	if (!s->holding) {		/* the countdown in the label */
		s15_buf_str(out, " (cancels in ");
		s15_buf_dec(out, s15_select_left(s, now_ms));
		s15_buf_str(out, " s)");
	}
}

static void hold_row(const struct s15_select *s, uint32_t now_ms,
		     struct s15_buf *out)
{
	s15_hold_bar(out, s15_select_fill(s, now_ms),
		     s15_select_hold_left(s, now_ms));
}

/* S-15.197 */
static size_t prompt_lines(const struct s15_select *s, uint32_t now_ms,
			   struct s15_buf out[S15_ROWS],
			   enum s15_style style[S15_ROWS])
{
	size_t n = 0;

	s15_buf_init(&out[n]);
	s15_buf_str(&out[n], "The firmware's signature does not match.");
	style[n++] = S15_ST_WARN;
	s15_buf_init(&out[n]);
	s15_buf_str(&out[n], "Installing it anyway is your choice.");
	style[n++] = S15_ST_TEXT;
	s15_buf_init(&out[n]);
	s15_buf_str(&out[n], "1 Install anyway (hold for ");
	s15_buf_dec(&out[n], S15_HOLD_MS / 1000u);
	s15_buf_str(&out[n], " s)");
	style[n++] = s->holding ? S15_ST_ARMED : S15_ST_ITEM;
	cancel_row(s, now_ms, &out[n]);
	style[n++] = S15_ST_BACK;
	if (s->holding) {
		hold_row(s, now_ms, &out[n]);
		style[n++] = S15_ST_STEP_RUN;
	}
	return n;
}

size_t s15_select_lines(const struct s15_select_info *info,
			const struct s15_select *s, uint32_t now_ms,
			struct s15_buf out[S15_ROWS],
			enum s15_style style[S15_ROWS])
{
	size_t n = 0;

	if (s->prompt)
		return prompt_lines(s, now_ms, out, style);

	/* the hold named in the label, not as a hint, armed or not */
	s15_buf_init(&out[n]);
	s15_buf_str(&out[n], "1 Install selected");
	if (s->selected == 0u) {
		s15_buf_str(&out[n], "\tNothing selected");
	} else {
		s15_buf_str(&out[n], " (hold for ");
		s15_buf_dec(&out[n], S15_HOLD_MS / 1000u);
		s15_buf_str(&out[n], " s)");
	}
	style[n++] = s->holding ? S15_ST_ARMED : S15_ST_ITEM;
	for (unsigned i = 0; i < ROWS; i++) {
		const enum s15_step step = by_digit[i];
		const struct s15_select_row *r = &info->row[step];

		if (r->kind == S15_ROW_NONE)
			continue;
		step_row(r, step, (s->selected & S15_SELECT_BIT(step)) != 0u,
			 &out[n], &style[n]);
		n++;
	}
	cancel_row(s, now_ms, &out[n]);
	style[n++] = S15_ST_BACK;
	if ((s->selected & S15_SELECT_BIT(S15_STEP_BOOTLOADER)) != 0u) {
		warn_line(info, &out[n]);
		style[n++] = S15_ST_WARN;
	}
	/* S-15.197: a valid package, while a row is free of the bar */
	if (info->valid && n + (s->holding ? 1u : 0u) < S15_ROWS) {
		s15_buf_init(&out[n]);
		s15_buf_str(&out[n], s15_signed_line);
		style[n++] = S15_ST_OK;
	}
	if (s->holding) {
		if (n == S15_ROWS)
			n--;		/* the bar in the warning's row */
		hold_row(s, now_ms, &out[n]);
		style[n++] = S15_ST_STEP_RUN;
	}
	return n;
}

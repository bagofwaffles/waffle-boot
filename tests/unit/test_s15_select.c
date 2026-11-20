/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.116: the selection of the steps before a run (src/s15_select.c):
 * the rows' digits, what the keys and the time decide, the bar and the
 * seconds left, and its lines, which fit their rows
 * (wiki/Screen-Style-Guide.md).
 * Requirements: S-15.116 (with its vectors, select_keys.json), S-11.096,
 * S-07.031, S-07.032.
 */
#include <stdbool.h>
#include <stdint.h>

#include "waffle/s15_select.h"
#include "waffle/s15_console.h"
#include "s14_check.h"
#define S15V_SELECT_KEYS
#define S15V_TOKENS
#include "vectors/s15_vectors.h"
#include "s15_tokens.h"

#define K1 0x06u	/* digit 1 */
#define K1B 0x0Eu	/* digit 1, the other key */
#define KNOB1 0x0Au	/* digit 1 by a knob click */
#define K2 0x07u	/* digit 2: Main DSP */
#define K3 0x08u	/* digit 3: Content */
#define K7 0x14u	/* digit 7: Bootloader */
#define K8 0x15u	/* digit 8 */
#define KX 0x40u	/* a step key: no digit */
#define T0 1000u	/* when the screen is shown */
#define ALL 0x3Fu	/* every row can be picked */

static struct s15_select s;

static void open_at(uint32_t now, uint8_t pickable)
{
	s15_select_open(&s, pickable, NULL, now);
}

static void press(unsigned key, uint32_t at)
{
	s15_select_key(&s, key, true, at);
	s15_select_key(&s, key, false, at);
}

/* The rows' digits: 2 Main DSP, 3 Content, 4 BLE, 5 BLE bootloader,
 * 6 Backplane, 7 Bootloader; the keys the tests use give theirs. */
static void test_digits(void)
{
	static const enum s15_step want[] = {
		S15_STEP_OS, S15_STEP_CONTENT, S15_STEP_BLE,
		S15_STEP_BLE_BOOTLOADER, S15_STEP_KEYBOARD, S15_STEP_BOOTLOADER,
	};

	for (unsigned d = 2; d <= 7u; d++)
		CHECK(s15_select_step(d) == (int)want[d - 2u] &&
		      s15_select_digit(want[d - 2u]) == d, "digit %u", d);
	CHECK(s15_select_step(0) < 0 && s15_select_step(1) < 0 &&
	      s15_select_step(8) < 0 && s15_select_step(9) < 0,
	      "1, 8 and others are no row");
	for (unsigned d = 0; d <= 8u; d++)
		CHECK((s15v_select_row_step[d] == 99u && s15_select_step(d) < 0) ||
		      s15v_select_row_step[d] == (unsigned)s15_select_step(d),
		      "the vectors' digit %u", d);
	CHECK(s15_digit(K1) == 1u && s15_digit(K1B) == 1u &&
	      s15_digit(KNOB1) == 1u && s15_digit(K2) == 2u &&
	      s15_digit(K3) == 3u && s15_digit(K7) == 7u && s15_digit(K8) == 8u &&
	      s15_digit(KX) == 0u, "the keys' digits");
}

/* Every row starts at No; a digit toggles its row, a knob click too. */
static void test_toggle(void)
{
	open_at(T0, ALL);
	CHECK(s.selected == 0u && s.choice == S15_SELECT_WAIT, "all No");
	press(K2, T0 + 10u);
	CHECK(s.selected == S15_SELECT_BIT(S15_STEP_OS), "2: Main DSP Yes");
	press(K7, T0 + 20u);
	CHECK(s.selected == (S15_SELECT_BIT(S15_STEP_OS) |
			     S15_SELECT_BIT(S15_STEP_BOOTLOADER)), "7: Bootloader Yes");
	press(K2, T0 + 30u);
	CHECK(s.selected == S15_SELECT_BIT(S15_STEP_BOOTLOADER), "2 again: No");
	press(0x0Bu, T0 + 40u);			/* knob click, digit 2 */
	CHECK((s.selected & S15_SELECT_BIT(S15_STEP_OS)) != 0u, "a knob click toggles");
	/* a row that cannot be picked: its digit does nothing */
	open_at(T0, S15_SELECT_BIT(S15_STEP_OS));
	for (unsigned k = 0; k < S15_NKEYS; k++) {
		const unsigned d = s15_digit(k);

		if (d >= 3u && d <= 7u)
			press(k, T0 + 10u);
	}
	CHECK(s.selected == 0u && s.choice == S15_SELECT_WAIT,
	      "digits of rows that cannot be picked: nothing");
}

/* Key 1 holds only with a row at Yes; 5 s install, not 4999 ms. */
static void test_hold(void)
{
	const unsigned keys[] = { K1, K1B };

	open_at(T0, ALL);
	s15_select_key(&s, K1, true, T0 + 100u);
	CHECK(!s.holding && s15_select_tick(&s, T0 + 10000u) == S15_SELECT_WAIT,
	      "nothing selected: key 1 does nothing");
	for (size_t i = 0; i < 2u; i++) {
		open_at(T0, ALL);
		press(K3, T0 + 100u);
		s15_select_key(&s, keys[i], true, T0 + 2000u);
		CHECK(s.holding && s15_select_tick(&s, T0 + 6999u) == S15_SELECT_WAIT &&
		      s15_select_fill(&s, T0 + 6999u) == 99u &&
		      s15_select_hold_left(&s, T0 + 6999u) == 1u,
		      "key %02X: 4999 ms held: waiting, 99%%", keys[i]);
		CHECK(s15_select_tick(&s, T0 + 7000u) == S15_SELECT_INSTALL &&
		      s.end == S15_SELECT_END_HELD &&
		      s.selected == S15_SELECT_BIT(S15_STEP_CONTENT),
		      "key %02X: 5000 ms held: install the selection", keys[i]);
		CHECK(s15_select_fill(&s, T0 + 7000u) == 100u, "the bar full");
		s15_select_key(&s, K8, true, T0 + 8000u);
		press(K2, T0 + 8000u);
		CHECK(s15_select_tick(&s, T0 + 90000u) == S15_SELECT_INSTALL &&
		      s.selected == S15_SELECT_BIT(S15_STEP_CONTENT),
		      "decided: later keys and time change nothing");
	}
	/* the bar's percent and seconds on the way */
	open_at(T0, ALL);
	press(K2, T0);
	s15_select_key(&s, K1, true, T0);
	CHECK(s15_select_fill(&s, T0) == 0u && s15_select_hold_left(&s, T0) == 5u &&
	      s15_select_fill(&s, T0 + 2000u) == 40u &&
	      s15_select_hold_left(&s, T0 + 2000u) == 3u &&
	      s15_select_fill(&s, T0 + 2500u) == 50u, "fill 0, 40, 50");
	/* a knob click giving 1 starts no hold */
	open_at(T0, ALL);
	press(K2, T0);
	s15_select_key(&s, KNOB1, true, T0 + 10u);
	CHECK(!s.holding && s15_select_tick(&s, T0 + 6000u) == S15_SELECT_WAIT,
	      "knob click 1: no hold");
}

/* An early release ends the hold and keeps the selection; a second hold
 * starts from 0. Any other key down ends it too, and does nothing else. */
static void test_release(void)
{
	open_at(T0, ALL);
	press(K2, T0);
	s15_select_key(&s, K1, true, T0 + 1000u);
	CHECK(s15_select_fill(&s, T0 + 4000u) == 60u, "60%% after 3 s");
	s15_select_key(&s, K1, false, T0 + 4000u);
	CHECK(!s.holding && s15_select_fill(&s, T0 + 4000u) == 0u &&
	      s15_select_tick(&s, T0 + 7000u) == S15_SELECT_WAIT &&
	      s.selected == S15_SELECT_BIT(S15_STEP_OS),
	      "released: no install, the selection kept");
	s15_select_key(&s, K1, true, T0 + 10000u);
	s15_select_key(&s, K1B, false, T0 + 11000u);	/* another key's up */
	CHECK(s15_select_fill(&s, T0 + 11000u) == 20u, "from 0 again");
	CHECK(s15_select_tick(&s, T0 + 14999u) == S15_SELECT_WAIT &&
	      s15_select_tick(&s, T0 + 15000u) == S15_SELECT_INSTALL,
	      "the second hold counts from its own start");
	/* 8 and a digit during a hold only end it */
	open_at(T0, ALL);
	press(K2, T0);
	s15_select_key(&s, K1, true, T0 + 100u);
	s15_select_key(&s, K8, true, T0 + 200u);
	CHECK(!s.holding && s15_select_tick(&s, T0 + 10000u) == S15_SELECT_WAIT,
	      "8 during the hold: the hold ends, nothing canceled");
	open_at(T0, ALL);
	press(K2, T0);
	s15_select_key(&s, K1, true, T0 + 100u);
	s15_select_key(&s, K3, true, T0 + 200u);
	CHECK(!s.holding && s.selected == S15_SELECT_BIT(S15_STEP_OS),
	      "a digit during the hold: the hold ends, no toggle");
	/* a release after 5 s installs though the time was not asked between */
	open_at(T0, ALL);
	press(K2, T0);
	s15_select_key(&s, K1, true, T0 + 1000u);
	s15_select_key(&s, K1, false, T0 + 6500u);
	CHECK(s15_select_tick(&s, T0 + 6500u) == S15_SELECT_INSTALL,
	      "released after 5 s, no tick between: install");
}

/* Digit 8 cancels; keys with no digit do nothing. */
static void test_other_keys(void)
{
	open_at(T0, ALL);
	press(K2, T0);
	s15_select_key(&s, K8, true, T0 + 200u);
	CHECK(s15_select_tick(&s, T0 + 200u) == S15_SELECT_CANCEL &&
	      s.end == S15_SELECT_END_KEY, "8: canceled");
	for (unsigned k = 0; k < S15_NKEYS; k++) {
		if (s15_digit(k) != 0u)
			continue;
		open_at(T0, ALL);
		s15_select_key(&s, k, true, T0);
		s15_select_key(&s, k, false, T0 + 5500u);
		s15_select_key(&s, k, true, T0 + 5600u);
		CHECK(s15_select_tick(&s, T0 + 11000u) == S15_SELECT_WAIT &&
		      s.selected == 0u && !s.holding, "key %02X: no effect", k);
	}
	open_at(T0, ALL);
	s15_select_key(&s, S15_NKEYS, true, T0);
	s15_select_key(&s, 0xFFu, true, T0);
	CHECK(s15_select_tick(&s, T0 + 6000u) == S15_SELECT_WAIT && s.selected == 0u,
	      "keys out of range: no effect");
}

/* 60 s with no key event cancel; every key event restarts them; they never
 * end a hold. */
static void test_timeout(void)
{
	open_at(T0, ALL);
	CHECK(s15_select_tick(&s, T0 + 59999u) == S15_SELECT_WAIT,
	      "59999 ms: waiting");
	CHECK(s15_select_tick(&s, T0 + 60000u) == S15_SELECT_CANCEL &&
	      s.end == S15_SELECT_END_TIMEOUT, "60 s: canceled");

	open_at(T0, ALL);
	s15_select_key(&s, KX, true, T0 + 50000u);
	CHECK(s15_select_left(&s, T0 + 50000u) == 60u &&
	      s15_select_tick(&s, T0 + 109999u) == S15_SELECT_WAIT,
	      "a key event restarts the 60 s");
	s15_select_key(&s, KX, false, T0 + 100000u);
	CHECK(s15_select_tick(&s, T0 + 159999u) == S15_SELECT_WAIT &&
	      s15_select_tick(&s, T0 + 160000u) == S15_SELECT_CANCEL,
	      "its up too");

	open_at(T0, ALL);
	press(K2, T0);
	s15_select_key(&s, K1, true, T0 + 58000u);
	CHECK(s15_select_tick(&s, T0 + 62999u) == S15_SELECT_WAIT &&
	      s15_select_tick(&s, T0 + 63000u) == S15_SELECT_INSTALL,
	      "a hold goes on past the 60 s");
	open_at(T0, ALL);
	press(K2, T0);
	s15_select_key(&s, K1, true, T0 + 1000u);
	CHECK(s15_select_tick(&s, T0 + 200000u) == S15_SELECT_INSTALL,
	      "asked late: the hold's install, not the time's cancel");

	/* the clock wraps: the times are differences */
	open_at(0xFFFFF000u, ALL);
	CHECK(s15_select_tick(&s, 0xFFFFF000u + 59999u) == S15_SELECT_WAIT &&
	      s15_select_tick(&s, 0xFFFFF000u + 60000u) == S15_SELECT_CANCEL,
	      "across the clock's wrap");
}

/* A key down when the screen is shown acts only after an up and a down. */
static void test_stale(void)
{
	uint8_t down[(S15_NKEYS + 7u) / 8u] = { 0 };

	down[K1 / 8u] |= (uint8_t)(1u << (K1 % 8u));
	down[K2 / 8u] |= (uint8_t)(1u << (K2 % 8u));
	down[K8 / 8u] |= (uint8_t)(1u << (K8 % 8u));
	s15_select_open(&s, ALL, down, T0);
	s15_select_key(&s, K8, true, T0 + 10u);	/* a repeat of the state */
	s15_select_key(&s, K2, true, T0 + 10u);
	CHECK(s15_select_tick(&s, T0 + 6000u) == S15_SELECT_WAIT && s.selected == 0u,
	      "keys down when shown: neither 8 nor a toggle");
	s15_select_key(&s, K2, false, T0 + 6000u);
	CHECK(s.selected == 0u, "2's up alone: nothing");
	s15_select_key(&s, K2, true, T0 + 6100u);
	CHECK(s.selected == S15_SELECT_BIT(S15_STEP_OS), "2's up and down: toggled");
	s15_select_key(&s, K2, false, T0 + 6200u);
	s15_select_key(&s, K1, false, T0 + 7000u);
	s15_select_key(&s, K1, true, T0 + 7100u);
	CHECK(!s.holding, "8 still down: key 1 is not alone, no hold");
	s15_select_key(&s, K8, false, T0 + 8000u);
	s15_select_key(&s, K1, false, T0 + 8100u);
	s15_select_key(&s, K1, true, T0 + 9000u);
	CHECK(s15_select_tick(&s, T0 + 13999u) == S15_SELECT_WAIT &&
	      s15_select_tick(&s, T0 + 14000u) == S15_SELECT_INSTALL,
	      "all released, then key 1 alone: the hold counts");
}

/* S-07.033's rule: key 1 holds only with no other of the 72 keys down,
 * precomp aside; any other key down ends the hold. */
static void test_alone(void)
{
	for (unsigned k = 0; k < S15_NKEYS; k++) {
		if (k == K1 || k == 0x17u)
			continue;
		open_at(T0, ALL);
		press(K2, T0);
		s15_select_key(&s, K1, true, T0 + 100u);
		s15_select_key(&s, k, true, T0 + 2000u);
		CHECK(!s.holding && s15_select_tick(&s, T0 + 10000u) == S15_SELECT_WAIT,
		      "key %02X during the hold: it ends", k);
		s15_select_key(&s, k, false, T0 + 10000u);
		CHECK(!s.holding, "key %02X up again: no hold without a new 1", k);
		s15_select_key(&s, K1, false, T0 + 20000u);
		s15_select_key(&s, K1, true, T0 + 21000u);
		CHECK(s15_select_tick(&s, T0 + 26000u) == S15_SELECT_INSTALL,
		      "after key %02X: key 1 alone again holds", k);

		open_at(T0, ALL);
		press(K2, T0);
		s15_select_key(&s, k, true, T0 + 100u);
		s15_select_key(&s, K1, true, T0 + 1000u);
		CHECK(!s.holding, "key %02X down first: no hold", k);
	}
	open_at(T0, ALL);
	press(K2, T0);
	s15_select_key(&s, 0x17u, true, T0 + 100u);
	s15_select_key(&s, K1, true, T0 + 1000u);
	s15_select_key(&s, 0x17u, false, T0 + 2000u);
	s15_select_key(&s, 0x17u, true, T0 + 3000u);
	CHECK(s15_select_tick(&s, T0 + 6000u) == S15_SELECT_INSTALL, "precomp: ignored");
}

/* The whole seconds left, rounded up, to 0. */
static void test_left(void)
{
	open_at(T0, ALL);
	CHECK(s15_select_left(&s, T0) == 60u &&
	      s15_select_left(&s, T0 + 1u) == 60u &&
	      s15_select_left(&s, T0 + 1000u) == 59u &&
	      s15_select_left(&s, T0 + 8000u) == 52u &&
	      s15_select_left(&s, T0 + 59001u) == 1u &&
	      s15_select_left(&s, T0 + 60000u) == 0u &&
	      s15_select_left(&s, T0 + 70000u) == 0u, "seconds left");
	CHECK(s15_select_hold_left(&s, T0 + 10u) == 0u &&
	      s15_select_fill(&s, T0 + 10u) == 0u, "no hold: no bar");
}

/*
 * S-15.116's vectors: the choice by end_ms, when it was made and the
 * selection, with the time asked every millisecond (as the board's loop
 * does) and, the choice alone, asked only at each event and at end_ms.
 */
static enum s15_select_choice play(const struct s15v_select *v, bool dense,
				   long *at)
{
	uint8_t down[(S15_NKEYS + 7u) / 8u] = { 0 };
	size_t e = 0;

	for (size_t i = 0; i < v->n_down; i++)
		down[v->down_at_open[i] / 8u] |=
			(uint8_t)(1u << (v->down_at_open[i] % 8u));
	s15_select_open(&s, (uint8_t)v->pickable, down, T0);
	*at = -1;
	for (uint32_t t = 0; t <= v->end_ms && *at < 0; t++) {
		const bool event = e < v->n && v->events[e].ms == t;

		if (!dense && !event && t != v->end_ms)
			continue;
		if (s15_select_tick(&s, T0 + t) == S15_SELECT_WAIT)
			for (; e < v->n && v->events[e].ms == t; e++)
				s15_select_key(&s, v->events[e].key,
					       v->events[e].down, T0 + t);
		if (s.choice != S15_SELECT_WAIT)
			*at = (long)t;
	}
	return s.choice;
}

static const char *choice_name(enum s15_select_choice ch)
{
	return ch == S15_SELECT_INSTALL ? "install" :
	       ch == S15_SELECT_CANCEL ? "cancel" : NULL;
}

static void test_vectors(void)
{
	CHECK(S15V_SELECT_HOLD_MS == S15_HOLD_MS &&
	      S15V_SELECT_TIMEOUT_MS == S15_SELECT_TIMEOUT_MS, "the times");
	for (size_t i = 0; i < sizeof(s15v_select) / sizeof(s15v_select[0]); i++) {
		const struct s15v_select *v = &s15v_select[i];
		long at, at_sparse;
		const char *got = choice_name(play(v, true, &at));
		const unsigned sel = s.selected;
		const char *sparse = choice_name(play(v, false, &at_sparse));

		CHECK((got == NULL && v->expect == NULL) ||
		      (got != NULL && v->expect != NULL && strcmp(got, v->expect) == 0),
		      "%s: %s, want %s", v->name, got ? got : "waiting",
		      v->expect ? v->expect : "waiting");
		CHECK(v->expect == NULL || at == v->at_ms,
		      "%s: made at %ld, want %ld", v->name, at, v->at_ms);
		CHECK(sel == v->selected, "%s: selected 0x%02x, want 0x%02x",
		      v->name, sel, v->selected);
		CHECK((sparse == NULL && got == NULL) ||
		      (sparse != NULL && got != NULL && strcmp(sparse, got) == 0),
		      "%s: asked sparsely: %s", v->name, sparse ? sparse : "waiting");
	}
}

/* ---- the lines -------------------------------------------------------- */

static size_t part(const char *str, unsigned k, const char **at)
{
	size_t n = 0;

	while (k > 0u && *str != '\0')
		if (*str++ == '\t')
			k--;
	*at = str;
	while (str[n] != '\0' && str[n] != '\t')
		n++;
	return n;
}

static unsigned tabs(const char *str)
{
	unsigned n = 0;

	while (*str != '\0')
		n += *str++ == '\t';
	return n;
}

/* Each line within its row in its style (wiki/Screen-Style-Guide.md). */
static bool fits(enum s15_style st, const char *str)
{
	const char *p;
	size_t n = strlen(str), ticks = 0;

	for (size_t i = 0; i < n; i++)
		ticks += str[i] == '`';
	if (st == S15_ST_WARN)
		return tabs(str) == 0u && 3u + n <= S15_COLS;
	if (st == S15_ST_ITEM || st == S15_ST_ARMED || st == S15_ST_BACK)
		return tabs(str) <= 1u && str[1] == ' ' &&
		       part(str, 0, &p) + 1u + (part(str, 1, &p) - ticks) + 1u <= S15_COLS;
	if (st == S15_ST_PICK || st == S15_ST_PICK_ON)	/* label, detail, state */
		return tabs(str) == 2u && str[1] == ' ' &&
		       S15_PICK_LABEL_COL + part(str, 0, &p) - 2u < S15_PICK_DETAIL_COL &&
		       S15_PICK_DETAIL_COL + part(str, 1, &p) + 1u + part(str, 2, &p) <=
		       S15_COLS;
	if (st == S15_ST_PICK_OFF)
		return tabs(str) == 1u && str[1] == ' ' &&
		       S15_PICK_LABEL_COL + part(str, 0, &p) - 2u < S15_PICK_DETAIL_COL &&
		       S15_PICK_DETAIL_COL + part(str, 1, &p) <= S15_COLS;
	if (st == S15_ST_STEP_RUN)
		return tabs(str) == 2u && 3u + part(str, 0, &p) < S15_STEP_COL &&
		       S15_STEP_COL + 26u + part(str, 2, &p) <= S15_COLS;
	return false;
}

static struct s15_select_info info;

static void full_info(void)
{
	static const char *const have[S15_STEP_COUNT] = {
		[S15_STEP_OS] = "1.1.4", [S15_STEP_BOOTLOADER] = "0.9.40",
		[S15_STEP_BLE] = "0.9.2", [S15_STEP_KEYBOARD] = "0.4.1",
	};
	static const char *const pkg[S15_STEP_COUNT] = {
		[S15_STEP_CONTENT] = "Bass Essentials", [S15_STEP_OS] = "1.1.5",
		[S15_STEP_BOOTLOADER] = "0.9.41", [S15_STEP_BLE] = "0.9.3",
		[S15_STEP_BLE_BOOTLOADER] = "2.0.1", [S15_STEP_KEYBOARD] = "0.4.2",
	};

	for (unsigned i = 0; i < S15_STEP_COUNT; i++) {
		info.row[i].kind = S15_ROW_PICK;
		info.row[i].installed = have[i];
		info.row[i].package = pkg[i];
	}
	info.slot = 1;
	info.stock = false;
}

static size_t lines(struct s15_buf b[S15_ROWS], enum s15_style st[S15_ROWS],
		    uint32_t now)
{
	const size_t n = s15_select_lines(&info, &s, now, b, st);

	for (size_t i = 0; i < n; i++)
		CHECK(fits(st[i], b[i].s), "row %zu fits: '%s'", i, b[i].s);
	return n;
}

static void test_lines(void)
{
	struct s15_buf b[S15_ROWS];
	enum s15_style st[S15_ROWS];
	size_t n;

	/* as opened: every row at No, nothing selected */
	full_info();
	open_at(T0, ALL);
	n = lines(b, st, T0);
	CHECK(n == 8u, "1, six rows, 8: %zu", n);
	CHECK(strcmp(b[0].s, "1 Install selected\tNothing selected") == 0 &&
	      st[0] == S15_ST_ITEM, "row 1: '%s'", b[0].s);
	CHECK(strcmp(b[1].s, "2 Main DSP\t1.1.4 > 1.1.5\tNo") == 0 && st[1] == S15_ST_PICK &&
	      strcmp(b[2].s, "3 Content\tBass Essentials\tNo") == 0 &&
	      strcmp(b[3].s, "4 BLE\t0.9.2 > 0.9.3\tNo") == 0 &&
	      strcmp(b[4].s, "5 BLE bootloader\t2.0.1\tNo") == 0 &&
	      strcmp(b[5].s, "6 Backplane\t0.4.1 > 0.4.2\tNo") == 0 &&
	      strcmp(b[6].s, "7 Bootloader\t0.9.40 > 0.9.41\tNo") == 0,
	      "the rows in digit order: '%s' .. '%s'", b[1].s, b[6].s);
	CHECK(strcmp(b[7].s, "8 Cancel (cancels in 60 s)") == 0 && st[7] == S15_ST_BACK,
	      "row 8: '%s'", b[7].s);
	n = lines(b, st, T0 + 8000u);
	CHECK(n == 8u && strcmp(b[7].s, "8 Cancel (cancels in 52 s)") == 0,
	      "the time: '%s'", b[7].s);

	/* picked, the bootloader among them: Yes, the hint, the warning */
	info.row[S15_STEP_BLE].kind = S15_ROW_OFF;
	info.row[S15_STEP_BLE_BOOTLOADER].kind = S15_ROW_OFF;
	info.row[S15_STEP_KEYBOARD].kind = S15_ROW_UP_TO_DATE;
	open_at(T0, (uint8_t)(S15_SELECT_BIT(S15_STEP_OS) | S15_SELECT_BIT(S15_STEP_CONTENT) |
			      S15_SELECT_BIT(S15_STEP_BOOTLOADER)));
	press(K2, T0);
	press(K7, T0);
	n = lines(b, st, T0);
	CHECK(n == 9u, "nine rows: %zu", n);
	CHECK(strcmp(b[0].s, "1 Install selected (hold for 5 s)") == 0 &&
	      st[0] == S15_ST_ITEM, "row 1 with a selection: '%s'", b[0].s);
	CHECK(strcmp(b[1].s, "2 Main DSP\t1.1.4 > 1.1.5\tYes") == 0 &&
	      st[1] == S15_ST_PICK_ON && st[2] == S15_ST_PICK &&
	      strcmp(b[6].s, "7 Bootloader\t0.9.40 > 0.9.41\tYes") == 0 &&
	      st[6] == S15_ST_PICK_ON, "Yes rows");
	CHECK(strcmp(b[3].s, "4 BLE\tNot in this version") == 0 && st[3] == S15_ST_PICK_OFF &&
	      strcmp(b[5].s, "6 Backplane\tUp to date") == 0 && st[5] == S15_ST_PICK_OFF,
	      "rows that cannot be picked: '%s', '%s'", b[3].s, b[5].s);
	CHECK(strcmp(b[8].s, "Slot 2 is replaced. Recovery remains.") == 0 &&
	      st[8] == S15_ST_WARN, "the warning: '%s'", b[8].s);
	{
		/* required information, by its tokens */
		const char *r[S15_ROWS];
		const char *next[] = { "next", "2", NULL };

		for (size_t i = 0; i < S15_ROWS; i++)
			r[i] = b[i].s;
		CHECK(s15t_match_named("select:install", r, n, NULL),
		      "the install item, its hold and the cancel");
		CHECK(s15t_match_named("select:bootloader", &r[8], 1, next),
		      "the slot the bootloader replaces");
	}
	/* the hold: row 1 armed, the bar in the warning's row, 8 without the time */
	s15_select_key(&s, K1, true, T0 + 1000u);
	n = lines(b, st, T0 + 3000u);
	CHECK(n == 9u && st[0] == S15_ST_ARMED &&
	      strcmp(b[0].s, "1 Install selected (hold for 5 s)") == 0 &&
	      strcmp(b[7].s, "8 Cancel") == 0 &&
	      strcmp(b[8].s, "Release to cancel\t40%\t3 s") == 0 && st[8] == S15_ST_STEP_RUN,
	      "held: '%s', '%s'", b[7].s, b[8].s);
	/* fewer rows: the bar after the warning */
	info.row[S15_STEP_BLE].kind = S15_ROW_NONE;
	n = lines(b, st, T0 + 3000u);
	CHECK(n == 9u && st[6] == S15_ST_BACK && st[7] == S15_ST_WARN &&
	      st[8] == S15_ST_STEP_RUN, "five rows held: the warning, then the bar");
	/* the option off; the bootloader at No: no warning */
	info.row[S15_STEP_BOOTLOADER].kind = S15_ROW_OPTION_OFF;
	open_at(T0, S15_SELECT_BIT(S15_STEP_OS));
	press(K7, T0);
	n = lines(b, st, T0);
	CHECK(n == 7u && strcmp(b[5].s, "7 Bootloader\tProtected writes off") == 0 &&
	      st[5] == S15_ST_PICK_OFF && st[n - 1u] == S15_ST_BACK && s.selected == 0u,
	      "option off: '%s'", b[5].s);
	press(K2, T0);
	s15_select_key(&s, K1, true, T0);
	n = lines(b, st, T0);
	CHECK(n == 8u && st[7] == S15_ST_STEP_RUN, "no warning: the bar last");

	/* the slot index 0 is Slot 1; the selector unread; a return to stock */
	full_info();
	open_at(T0, ALL);
	press(K7, T0);
	info.slot = 0;
	n = lines(b, st, T0);
	CHECK(strcmp(b[n - 1u].s, "Slot 1 is replaced. Recovery remains.") == 0, "slot 1");
	info.slot = -1;
	n = lines(b, st, T0);
	CHECK(strcmp(b[n - 1u].s, "The other slot is replaced. Recovery remains.") == 0,
	      "slot not known");
	info.stock = true;
	n = lines(b, st, T0);
	CHECK(strcmp(b[n - 1u].s, s15_stock_line) == 0 && st[n - 1u] == S15_ST_WARN,
	      "a return to stock: '%s'", b[n - 1u].s);

	/* versions not known, a content pack with no name, a content-only pack */
	full_info();
	info.row[S15_STEP_OS].installed = NULL;
	info.row[S15_STEP_CONTENT].package = NULL;
	open_at(T0, ALL);
	n = lines(b, st, T0);
	CHECK(n == 8u && strcmp(b[1].s, "2 Main DSP\t1.1.5\tNo") == 0 &&
	      strcmp(b[2].s, "3 Content\t\tNo") == 0, "'%s', '%s'", b[1].s, b[2].s);
	for (unsigned i = 0; i < S15_STEP_COUNT; i++)
		info.row[i].kind = i == S15_STEP_CONTENT ? S15_ROW_PICK : S15_ROW_NONE;
	n = lines(b, st, T0);
	CHECK(n == 3u && strncmp(b[1].s, "3 Content", 9) == 0 &&
	      strncmp(b[2].s, "8 Cancel", 8) == 0, "content only: one row");
	/* versions with a build number fit beside Yes */
	full_info();
	info.row[S15_STEP_OS].installed = "1.10.100+2";
	info.row[S15_STEP_OS].package = "1.10.101+3";
	open_at(T0, ALL);
	press(K2, T0);
	(void)lines(b, st, T0);
}

int main(void)
{
	test_digits();
	test_toggle();
	test_hold();
	test_release();
	test_other_keys();
	test_timeout();
	test_stale();
	test_alone();
	test_left();
	test_vectors();
	test_lines();
	return check_done("test_s15_select");
}

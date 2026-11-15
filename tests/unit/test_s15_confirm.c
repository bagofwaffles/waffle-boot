/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.116: a protected component's confirmation (src/s15_confirm.c): what
 * its keys and the time decide, the bar and the seconds left, and its
 * lines, which fit their rows (docs/ui-style-guide.md).
 * Requirements: S-15.116 (with its vectors, confirm_keys.json), S-11.096,
 * S-07.031, S-07.032.
 */
#include <stdbool.h>
#include <stdint.h>

#include "waffle/s15_confirm.h"
#include "waffle/s15_console.h"
#include "s14_check.h"
#define S15V_CONFIRM_KEYS
#include "vectors/s15_vectors.h"

#define K1 0x06u	/* digit 1 */
#define K1B 0x0Eu	/* digit 1, the other key */
#define KNOB1 0x0Au	/* digit 1 by a knob click */
#define K8 0x15u	/* digit 8 */
#define T0 1000u	/* when the screen is shown */

static struct s15_confirm c;

static void open_at(uint32_t now)
{
	s15_confirm_open(&c, NULL, true, now);
}

/* The keys the tests use give the digits they stand for. */
static void test_digits(void)
{
	CHECK(s15_digit(K1) == 1u && s15_digit(K1B) == 1u &&
	      s15_digit(KNOB1) == 1u && s15_digit(K8) == 8u, "the keys' digits");
}

/* A full 5 s hold confirms: not at 4999 ms, at 5000 ms. */
static void test_hold(void)
{
	const unsigned keys[] = { K1, K1B };

	for (size_t i = 0; i < 2u; i++) {
		open_at(T0);
		CHECK(s15_confirm_tick(&c, T0 + 2000u) == S15_CONFIRM_WAIT,
		      "nothing yet");
		s15_confirm_key(&c, keys[i], true, T0 + 2000u);
		CHECK(s15_confirm_tick(&c, T0 + 6999u) == S15_CONFIRM_WAIT &&
		      s15_confirm_fill(&c, T0 + 6999u) == 99u,
		      "key %02X: 4999 ms held: waiting, 99%%", keys[i]);
		CHECK(s15_confirm_tick(&c, T0 + 7000u) == S15_CONFIRM_INSTALL &&
		      c.end == S15_CONFIRM_END_HELD,
		      "key %02X: 5000 ms held: install", keys[i]);
		CHECK(s15_confirm_fill(&c, T0 + 7000u) == 100u, "the bar full");
		/* decided: later keys and time change nothing */
		s15_confirm_key(&c, K8, true, T0 + 8000u);
		CHECK(s15_confirm_tick(&c, T0 + 90000u) == S15_CONFIRM_INSTALL,
		      "the choice stands");
	}
	/* the bar's percent on the way */
	open_at(T0);
	s15_confirm_key(&c, K1, true, T0);
	CHECK(s15_confirm_fill(&c, T0) == 0u &&
	      s15_confirm_fill(&c, T0 + 2000u) == 40u &&
	      s15_confirm_fill(&c, T0 + 2500u) == 50u, "fill 0, 40, 50");
	/* a second digit-1 key while one is held ends the hold (S-07.033) */
	s15_confirm_key(&c, K1B, true, T0 + 3000u);
	CHECK(s15_confirm_tick(&c, T0 + 5000u) == S15_CONFIRM_WAIT &&
	      s15_confirm_fill(&c, T0 + 5000u) == 0u,
	      "two digit-1 keys: no hold");
}

/* An early release empties the bar; a second hold starts from 0. */
static void test_release(void)
{
	open_at(T0);
	s15_confirm_key(&c, K1, true, T0 + 1000u);
	CHECK(s15_confirm_fill(&c, T0 + 4000u) == 60u, "60%% after 3 s");
	s15_confirm_key(&c, K1, false, T0 + 4000u);
	CHECK(s15_confirm_fill(&c, T0 + 4000u) == 0u &&
	      s15_confirm_tick(&c, T0 + 7000u) == S15_CONFIRM_WAIT,
	      "released: the bar empty, no install");
	/* another key's up does not end the hold */
	s15_confirm_key(&c, K1, true, T0 + 10000u);
	s15_confirm_key(&c, K1B, false, T0 + 11000u);
	CHECK(s15_confirm_fill(&c, T0 + 11000u) == 20u, "from 0 again");
	CHECK(s15_confirm_tick(&c, T0 + 14999u) == S15_CONFIRM_WAIT,
	      "the second hold counts from its own start");
	CHECK(s15_confirm_tick(&c, T0 + 15000u) == S15_CONFIRM_INSTALL,
	      "5 s of the second hold");
	/* a release after 5 s confirms though the time was not asked
	 * between: the event's own time decides first */
	open_at(T0);
	s15_confirm_key(&c, K1, true, T0 + 1000u);
	s15_confirm_key(&c, K1, false, T0 + 6500u);
	CHECK(s15_confirm_tick(&c, T0 + 6500u) == S15_CONFIRM_INSTALL,
	      "released after 5 s, no tick between: install");
	/* and 8 after the 60 s, the time not asked since: the time's skip */
	open_at(T0);
	s15_confirm_key(&c, K8, true, T0 + 61000u);
	CHECK(c.choice == S15_CONFIRM_SKIP && c.end == S15_CONFIRM_END_TIMEOUT,
	      "8 after the time ran out: the timeout's skip");
}

/* Digit 8 skips at once; a knob click and every other key do nothing. */
static void test_other_keys(void)
{
	open_at(T0);
	s15_confirm_key(&c, K1, true, T0 + 100u);
	s15_confirm_key(&c, K8, true, T0 + 200u);
	CHECK(s15_confirm_tick(&c, T0 + 200u) == S15_CONFIRM_SKIP &&
	      c.end == S15_CONFIRM_END_KEY, "8: skipped, even during a hold");
	CHECK(s15_confirm_tick(&c, T0 + 10000u) == S15_CONFIRM_SKIP,
	      "the hold does not finish after it");

	open_at(T0);
	s15_confirm_key(&c, KNOB1, true, T0);
	CHECK(s15_confirm_tick(&c, T0 + 6000u) == S15_CONFIRM_WAIT &&
	      s15_confirm_fill(&c, T0 + 6000u) == 0u,
	      "a knob click giving 1 starts no hold");

	for (unsigned k = 0; k < S15_NKEYS; k++) {
		const unsigned d = s15_digit(k);

		if (d == 1u || d == 8u)
			continue;
		open_at(T0);
		s15_confirm_key(&c, k, true, T0);
		s15_confirm_key(&c, k, false, T0 + 5500u);
		s15_confirm_key(&c, k, true, T0 + 5600u);
		CHECK(s15_confirm_tick(&c, T0 + 11000u) == S15_CONFIRM_WAIT &&
		      s15_confirm_fill(&c, T0 + 11000u) == 0u,
		      "key %02X: no effect", k);
	}
	/* keys past the table are ignored */
	open_at(T0);
	s15_confirm_key(&c, S15_NKEYS, true, T0);
	s15_confirm_key(&c, 0xFFu, true, T0);
	CHECK(s15_confirm_tick(&c, T0 + 6000u) == S15_CONFIRM_WAIT,
	      "keys out of range: no effect");
}

/* 60 s with no confirmation skips; a hold that reaches 5 s by then
 * confirms. */
static void test_timeout(void)
{
	open_at(T0);
	CHECK(s15_confirm_tick(&c, T0 + 59999u) == S15_CONFIRM_WAIT,
	      "59999 ms: waiting");
	CHECK(s15_confirm_tick(&c, T0 + 60000u) == S15_CONFIRM_SKIP &&
	      c.end == S15_CONFIRM_END_TIMEOUT, "60 s: skipped");

	open_at(T0);
	s15_confirm_key(&c, K1, true, T0 + 55000u);
	CHECK(s15_confirm_tick(&c, T0 + 59999u) == S15_CONFIRM_WAIT,
	      "the hold under way at 59999 ms");
	CHECK(s15_confirm_tick(&c, T0 + 60000u) == S15_CONFIRM_INSTALL &&
	      c.end == S15_CONFIRM_END_HELD,
	      "a hold begun at 55 s reaches 5 s at the mark: install");

	open_at(T0);
	s15_confirm_key(&c, K1, true, T0 + 55001u);
	CHECK(s15_confirm_tick(&c, T0 + 60000u) == S15_CONFIRM_SKIP,
	      "a hold begun after 55 s is too late");

	/* the clock wraps: the times are differences */
	open_at(0xFFFFF000u);
	CHECK(s15_confirm_tick(&c, 0xFFFFF000u + 59999u) == S15_CONFIRM_WAIT &&
	      s15_confirm_tick(&c, 0xFFFFF000u + 60000u) == S15_CONFIRM_SKIP,
	      "across the clock's wrap");
}

/* A key down when the screen is shown acts only after an up and a down. */
static void test_stale(void)
{
	uint8_t down[(S15_NKEYS + 7u) / 8u] = { 0 };

	down[K1 / 8u] |= (uint8_t)(1u << (K1 % 8u));
	down[K8 / 8u] |= (uint8_t)(1u << (K8 % 8u));
	s15_confirm_open(&c, down, true, T0);
	s15_confirm_key(&c, K8, true, T0 + 10u);	/* a repeat of the state */
	s15_confirm_key(&c, K1, true, T0 + 10u);
	CHECK(s15_confirm_tick(&c, T0 + 6000u) == S15_CONFIRM_WAIT &&
	      s15_confirm_fill(&c, T0 + 6000u) == 0u,
	      "keys down when shown: neither 8 nor the hold");
	s15_confirm_key(&c, K1, false, T0 + 6000u);
	s15_confirm_key(&c, K1, true, T0 + 7000u);
	CHECK(s15_confirm_tick(&c, T0 + 13000u) == S15_CONFIRM_WAIT &&
	      s15_confirm_fill(&c, T0 + 13000u) == 0u,
	      "8 still down: key 1 is not alone, no hold");
	s15_confirm_key(&c, K8, false, T0 + 13000u);
	s15_confirm_key(&c, K1, false, T0 + 13100u);
	s15_confirm_key(&c, K1, true, T0 + 14000u);
	CHECK(s15_confirm_tick(&c, T0 + 18999u) == S15_CONFIRM_WAIT &&
	      s15_confirm_tick(&c, T0 + 19000u) == S15_CONFIRM_INSTALL,
	      "both released, then key 1 alone: the hold counts");

	s15_confirm_open(&c, down, true, T0);
	s15_confirm_key(&c, K8, false, T0 + 10u);
	CHECK(s15_confirm_tick(&c, T0 + 20u) == S15_CONFIRM_WAIT,
	      "8's up alone: nothing");
	s15_confirm_key(&c, K8, true, T0 + 30u);
	CHECK(s15_confirm_tick(&c, T0 + 30u) == S15_CONFIRM_SKIP,
	      "8's up and down: skipped");
}

/* S-07.033's rule: key 1 holds only with no other of the 72 keys down,
 * precomp aside; any other key down ends the hold, and a new one needs key
 * 1 down again. */
static void test_alone(void)
{
	for (unsigned k = 0; k < S15_NKEYS; k++) {
		if (k == K1 || k == K8 || k == 0x17u)
			continue;
		open_at(T0);
		s15_confirm_key(&c, K1, true, T0);
		s15_confirm_key(&c, k, true, T0 + 2000u);
		CHECK(s15_confirm_tick(&c, T0 + 10000u) == S15_CONFIRM_WAIT &&
		      s15_confirm_fill(&c, T0 + 10000u) == 0u,
		      "key %02X during the hold: it ends", k);
		s15_confirm_key(&c, k, false, T0 + 10000u);
		CHECK(s15_confirm_tick(&c, T0 + 20000u) == S15_CONFIRM_WAIT,
		      "key %02X up again: no hold without a new down of 1", k);
		s15_confirm_key(&c, K1, false, T0 + 20000u);
		s15_confirm_key(&c, K1, true, T0 + 21000u);
		CHECK(s15_confirm_tick(&c, T0 + 26000u) == S15_CONFIRM_INSTALL,
		      "after key %02X: key 1 alone again holds", k);

		open_at(T0);
		s15_confirm_key(&c, k, true, T0);
		s15_confirm_key(&c, K1, true, T0 + 1000u);
		CHECK(s15_confirm_tick(&c, T0 + 10000u) == S15_CONFIRM_WAIT &&
		      s15_confirm_fill(&c, T0 + 10000u) == 0u,
		      "key %02X down first: no hold", k);
	}
	/* precomp (mic, 0x17) is no other key */
	open_at(T0);
	s15_confirm_key(&c, 0x17u, true, T0);
	s15_confirm_key(&c, K1, true, T0 + 1000u);
	s15_confirm_key(&c, 0x17u, false, T0 + 2000u);
	s15_confirm_key(&c, 0x17u, true, T0 + 3000u);
	CHECK(s15_confirm_tick(&c, T0 + 6000u) == S15_CONFIRM_INSTALL, "precomp: ignored");
}

/* Keys that cannot be read skip at once, and nothing changes that. */
static void test_unreadable(void)
{
	s15_confirm_open(&c, NULL, false, T0);
	CHECK(c.choice == S15_CONFIRM_SKIP && c.end == S15_CONFIRM_END_UNREADABLE &&
	      s15_confirm_tick(&c, T0) == S15_CONFIRM_SKIP,
	      "unreadable: skipped at once");
	s15_confirm_key(&c, K1, true, T0);
	CHECK(s15_confirm_tick(&c, T0 + 6000u) == S15_CONFIRM_SKIP &&
	      s15_confirm_fill(&c, T0 + 6000u) == 0u, "no hold after it");
}

/* The whole seconds left, rounded up, to 0. */
static void test_left(void)
{
	open_at(T0);
	CHECK(s15_confirm_left(&c, T0) == 60u &&
	      s15_confirm_left(&c, T0 + 1u) == 60u &&
	      s15_confirm_left(&c, T0 + 1000u) == 59u &&
	      s15_confirm_left(&c, T0 + 8000u) == 52u &&
	      s15_confirm_left(&c, T0 + 59001u) == 1u &&
	      s15_confirm_left(&c, T0 + 60000u) == 0u &&
	      s15_confirm_left(&c, T0 + 70000u) == 0u, "seconds left");
}

/*
 * S-15.116's vectors: the choice by end_ms and when it was made, with the
 * time asked every millisecond (as the board's loop does) and, the choice
 * alone, asked only at each event and at end_ms: no answer depends on how
 * often the time is asked.
 */
static enum s15_confirm_choice play(const struct s15v_confirm *v, bool dense,
				    long *at)
{
	uint8_t down[(S15_NKEYS + 7u) / 8u] = { 0 };
	size_t e = 0;

	for (size_t i = 0; i < v->n_down; i++)
		down[v->down_at_open[i] / 8u] |=
			(uint8_t)(1u << (v->down_at_open[i] % 8u));
	s15_confirm_open(&c, down, v->readable, T0);
	*at = c.choice != S15_CONFIRM_WAIT ? 0 : -1;
	for (uint32_t t = 0; t <= v->end_ms && *at < 0; t++) {
		const bool event = e < v->n && v->events[e].ms == t;

		if (!dense && !event && t != v->end_ms)
			continue;
		if (s15_confirm_tick(&c, T0 + t) == S15_CONFIRM_WAIT)
			for (; e < v->n && v->events[e].ms == t; e++)
				s15_confirm_key(&c, v->events[e].key,
						v->events[e].down, T0 + t);
		if (c.choice != S15_CONFIRM_WAIT)
			*at = (long)t;	/* asked sparsely: when it was seen */
	}
	return c.choice;
}

static const char *choice_name(enum s15_confirm_choice ch)
{
	return ch == S15_CONFIRM_INSTALL ? "install" :
	       ch == S15_CONFIRM_SKIP ? "skip" : NULL;
}

static void test_vectors(void)
{
	CHECK(S15V_CONFIRM_HOLD_MS == S15_CONFIRM_HOLD_MS &&
	      S15V_CONFIRM_TIMEOUT_MS == S15_CONFIRM_TIMEOUT_MS, "the times");
	for (size_t i = 0; i < sizeof(s15v_confirm) / sizeof(s15v_confirm[0]); i++) {
		const struct s15v_confirm *v = &s15v_confirm[i];
		long at, at_sparse;
		const char *got = choice_name(play(v, true, &at));
		const char *sparse = choice_name(play(v, false, &at_sparse));

		CHECK((got == NULL && v->expect == NULL) ||
		      (got != NULL && v->expect != NULL && strcmp(got, v->expect) == 0),
		      "%s: %s, want %s", v->name, got ? got : "waiting",
		      v->expect ? v->expect : "waiting");
		CHECK(v->expect == NULL || at == v->at_ms,
		      "%s: made at %ld, want %ld", v->name, at, v->at_ms);
		CHECK((sparse == NULL && got == NULL) ||
		      (sparse != NULL && got != NULL && strcmp(sparse, got) == 0),
		      "%s: asked sparsely: %s", v->name, sparse ? sparse : "waiting");
	}
}

/* ---- the lines -------------------------------------------------------- */

static size_t part(const char *s, unsigned k, const char **at)
{
	size_t n = 0;

	while (k > 0u && *s != '\0')
		if (*s++ == '\t')
			k--;
	*at = s;
	while (s[n] != '\0' && s[n] != '\t')
		n++;
	return n;
}

static unsigned tabs(const char *s)
{
	unsigned n = 0;

	while (*s != '\0')
		n += *s++ == '\t';
	return n;
}

/* Each line within its row in its style (docs/ui-style-guide.md). */
static bool fits(enum s15_style st, const char *s)
{
	const char *p;
	size_t n = strlen(s), ticks = 0;

	for (size_t i = 0; i < n; i++)
		ticks += s[i] == '`';
	if (st == S15_ST_HEAD || st == S15_ST_TEXT)
		return tabs(s) == 0u && n <= S15_COLS;
	if (st == S15_ST_WARN)
		return tabs(s) == 0u && 3u + n <= S15_COLS;
	if (st == S15_ST_KEYS)
		return tabs(s) == 0u && ticks % 2u == 0u && n - ticks <= S15_COLS;
	if (st == S15_ST_ITEM)
		return tabs(s) == 1u && s[1] == ' ' &&
		       part(s, 0, &p) + 1u + part(s, 1, &p) + 1u <= S15_COLS;
	if (st == S15_ST_KV)	/* values from columns 14 and 43, the left before 32 */
		return (tabs(s) == 1u || tabs(s) == 3u) &&
		       part(s, 0, &p) < 14u && 14u + part(s, 1, &p) < 32u &&
		       (tabs(s) == 1u ||
			(part(s, 2, &p) < 11u && 43u + part(s, 3, &p) <= S15_COLS));
	if (st == S15_ST_STEP_RUN)	/* the label before the bar, the third part after it */
		return tabs(s) == 2u && 3u + part(s, 0, &p) < S15_STEP_COL &&
		       S15_STEP_COL + 26u + part(s, 2, &p) <= S15_COLS;
	return false;
}

static void lines_fit(const char *what, const struct s15_confirm_info *info)
{
	struct s15_buf b[S15_CONFIRM_LINES];
	enum s15_style st[S15_CONFIRM_LINES];

	s15_confirm_lines(info, b, st);
	for (unsigned i = 0; i < S15_CONFIRM_LINES; i++)
		CHECK(b[i].n == 0u || fits(st[i], b[i].s),
		      "%s row %u fits: '%s'", what, i, b[i].s);
	CHECK(st[0] == S15_ST_HEAD && st[S15_CONFIRM_LINES - 2u] == S15_ST_ITEM &&
	      strncmp(b[S15_CONFIRM_LINES - 2u].s, "8 ", 2) == 0,
	      "%s: a question first, 8 last", what);
	CHECK(strstr(b[1].s, info->package) != NULL,
	      "%s: the package's version", what);
}

static void test_lines(void)
{
	struct s15_confirm_info boot = { S15_STEP_BOOTLOADER, "1.0.12", "1.0.10", 1 };
	const struct s15_confirm_info fs = { S15_STEP_SPIBOOT, "1.0.3", NULL, -1 };
	/* the longest versions S-11 writes (S11_VERSION_TEXT_MAX) */
	const struct s15_confirm_info wide = { S15_STEP_BOOTLOADER,
					       "65535.65535.65535", "255.255.255", 0 };
	struct s15_buf b[S15_CONFIRM_LINES], bar;
	enum s15_style st[S15_CONFIRM_LINES];

	lines_fit("bootloader", &boot);
	lines_fit("first stage", &fs);
	lines_fit("wide versions", &wide);
	s15_confirm_lines(&boot, b, st);
	CHECK(strcmp(b[0].s, "Install the bootloader?") == 0 &&
	      strcmp(b[1].s, "Package\t1.0.12\tRunning\t1.0.10") == 0 &&
	      strcmp(b[2].s, "Slot 2 is replaced. Recovery remains.") == 0 &&
	      st[2] == S15_ST_WARN && b[3].n == 0u && b[6].n == 0u &&
	      strcmp(b[4].s, "Hold `1` for 5 s to install") == 0 &&
	      st[4] == S15_ST_KEYS &&
	      strcmp(b[5].s, "8 Skip\tSkipped when the time runs out") == 0,
	      "the bootloader's lines");
	/* the slot shown as its number: index 0 is slot 1 */
	boot.slot = 0;
	s15_confirm_lines(&boot, b, st);
	CHECK(strcmp(b[2].s, "Slot 1 is replaced. Recovery remains.") == 0,
	      "slot index 0: '%s'", b[2].s);
	/* the selector unread: the other slot; no running version known */
	boot.slot = -1;
	boot.installed = NULL;
	lines_fit("slot unknown", &boot);
	s15_confirm_lines(&boot, b, st);
	CHECK(strcmp(b[2].s, "The other slot is replaced. Recovery remains.") == 0 &&
	      strcmp(b[1].s, "Package\t1.0.12") == 0, "slot not known: '%s'", b[2].s);
	s15_confirm_lines(&fs, b, st);
	CHECK(strcmp(b[0].s, "Install the first stage?") == 0 &&
	      strcmp(b[1].s, "Package\t1.0.3") == 0 &&
	      strcmp(b[2].s, "Nothing on the unit can repair a failed write.") == 0,
	      "the first stage's lines");

	/* the bar's row: the label, the percent for the bar, the seconds */
	s15_confirm_bar(&bar, 40, 52);
	CHECK(strcmp(bar.s, "Hold 1\t40%\t52 s") == 0 &&
	      fits(S15_ST_STEP_RUN, bar.s), "bar: '%s'", bar.s);
	s15_confirm_bar(&bar, 250, 60);
	CHECK(strcmp(bar.s, "Hold 1\t100%\t60 s") == 0 &&
	      fits(S15_ST_STEP_RUN, bar.s), "bar at most 100%%: '%s'", bar.s);
	s15_confirm_bar(&bar, 0, 0);
	CHECK(strcmp(bar.s, "Hold 1\t0%\t0 s") == 0, "bar empty: '%s'", bar.s);
	CHECK(strcmp(s15_confirm_title, "Confirm Install") == 0, "the title");
}

int main(void)
{
	test_digits();
	test_hold();
	test_release();
	test_other_keys();
	test_timeout();
	test_stale();
	test_alone();
	test_unreadable();
	test_left();
	test_vectors();
	test_lines();
	return check_done("test_s15_confirm");
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15 values and lines: digits, progress, the run's lines, pack names, and every required-information wording against the tokens of
 * tests/oracle/S-15-menus/vectors/info_tokens.json. Lines are written as the
 * vectors write them, "style:text", and fit the body's 56
 * columns in their style (wiki/Screen-Style-Guide.md).
 * Requirements: S-15.011, S-15.041, S-15.140, S-15.161, S-16.022, S-16.038, and the required information of S-15's terms.
 */
#include <stdbool.h>
#include <stdint.h>

#include "waffle/s15_console.h"
#include "waffle/s15_lines.h"
#include "s14_check.h"
#define S15V_KEYS
#define S15V_MANIFEST
#define S15V_UPGRADE
#define S15V_TOKENS
#include "vectors/s15_vectors.h"
#include "s15_tokens.h"

static enum s15_step step_of(const char *s)
{
	static const char *const names[S15_STEP_COUNT] = {
		"content", "os", "bootloader", "ble", "ble bootloader",
		"keyboard",
	};

	for (unsigned i = 0; i < S15_STEP_COUNT; i++)
		if (strcmp(names[i], s) == 0)
			return (enum s15_step)i;
	CHECK(0, "unknown step %s", s);
	return S15_STEP_CONTENT;
}

static enum s15_result result_of(const char *s)
{
	static const char *const names[] = {
		"OK", "Not found", "Already installed", "Skipped", "Failure",
		"Bad file", "Internal error", "Skipped (not confirmed)",
		"Skipped (switched off)", "Skipped (already written)",
	};

	for (unsigned i = 0; i < S15V_N(names); i++)
		if (strcmp(names[i], s) == 0)
			return (enum s15_result)i;
	CHECK(0, "unknown result %s", s);
	return S15_RES_FAILURE;
}

/* A vector's line, "style:text" or the text alone (S15_ST_TEXT). */
static const char *parse(const char *line, enum s15_style *style)
{
	for (unsigned i = 1; i < S15_ST_COUNT; i++) {
		size_t n = strlen(s15_style_names[i]);

		if (strncmp(line, s15_style_names[i], n) == 0 && line[n] == ':') {
			*style = (enum s15_style)i;
			return line + n + 1;
		}
	}
	*style = S15_ST_TEXT;
	return line;
}

/* Whether text in style matches a vector's line. */
static bool line_is(enum s15_style style, const char *text, const char *line)
{
	enum s15_style want;
	const char *t = parse(line, &want);

	return style == want && strcmp(text, t) == 0;
}

static size_t part_len(const char *s, unsigned k)
{
	size_t n = 0;

	while (k > 0u && *s != '\0') {
		if (*s++ == '\t')
			k--;
	}
	while (s[n] != '\0' && s[n] != '\t')
		n++;
	return n;
}

/*
 * Whether text drawn in style fits the body's 56 columns
 * (wiki/Screen-Style-Guide.md): printable characters, tabs only
 * between a styled line's parts, and each part ending by column 56.
 */
static bool fits(enum s15_style style, const char *text)
{
	size_t n = strlen(text), tabs = 0, ticks = 0, w;

	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)text[i];

		if (c == '\t')
			tabs++;
		else if (c < 0x20u || c > 0x7eu)
			return false;
		if (c == '`')
			ticks++;
	}
	switch (style) {
	case S15_ST_ITEM:
	case S15_ST_ITEM_ON:
	case S15_ST_ARMED:
	case S15_ST_BACK:
		/* the label from column 3, a gap, the hint right-aligned */
		w = part_len(text, 0) + (tabs ? 1u + part_len(text, 1) : 0u);
		return tabs <= 1u && part_len(text, 0) >= 3u && text[1] == ' ' &&
		       w + 1u <= S15_COLS;
	case S15_ST_OK:
	case S15_ST_WARN:
	case S15_ST_ERROR:
		return tabs == 0u && 3u + n <= S15_COLS;
	case S15_ST_STEP_RUN:
	case S15_ST_STEP_OK:
	case S15_ST_STEP_SKIP:
	case S15_ST_STEP_FAIL:
		return tabs <= 1u && 3u + part_len(text, 0) < 19u &&
		       (tabs == 0u || 19u + part_len(text, 1) <= S15_COLS);
	case S15_ST_KEYS:
		return tabs == 0u && ticks % 2u == 0u && n - ticks <= S15_COLS;
	case S15_ST_KV:
		return tabs <= 3u;
	case S15_ST_TEXT:
	case S15_ST_DIM:
	case S15_ST_HEAD:
	case S15_ST_COUNT:
	default:
		return tabs == 0u && n <= S15_COLS;
	}
}

/* One row against a token. */
static bool row_matches(const char *token, const char *row,
			const char *const *values)
{
	const char *rows[1] = { row };

	return s15t_match_named(token, rows, 1, values);
}

/* ---- the token port ------------------------------------------------- */

static void test_token_examples(void)
{
	for (size_t i = 0; i < S15V_N(s15v_examples); i++) {
		const struct s15v_example *e = &s15v_examples[i];
		size_t n = 0;

		while (e->rows[n] != NULL)
			n++;
		CHECK(s15t_match_named(e->token, e->rows, n, e->values) == e->expect,
		      "info_match port: example %zu (%s)", i, e->token);
	}
}

/* ---- keys ----------------------------------------------------------- */

static void test_keys(void)
{
	unsigned k, n = 0;

	for (size_t i = 0; i < S15V_N(s15v_digits); i++) {
		CHECK(s15_digit(s15v_digits[i].key) == s15v_digits[i].digit,
		      "S-15.011 key 0x%02x", s15v_digits[i].key);
		n++;
	}
	for (k = 0; k < 256; k++)
		if (s15_digit(k) != 0u)
			n--;
	CHECK(n == 0u, "S-15.011 no other key is a digit");
}

/* ---- the run -------------------------------------------------------- */

static void test_progress(void)
{
	struct s15_buf b;

	for (size_t i = 0; i < S15V_N(s15v_progress); i++) {
		const struct s15v_progress *p = &s15v_progress[i];

		CHECK(s15_progress(p->done, p->total) == p->p,
		      "%u/%u", p->done, p->total);
	}
	/* A step-run row, "Label\tStatus" */
	for (unsigned p = 0; p < 8; p++) {
		char want[40];

		snprintf(want, sizeof want, "Backplane\tInstalling %c",
			 s15v_backplane_chars[p]);
		CHECK(s15_step_progress_line(&b, S15_STEP_KEYBOARD, p) ==
		      S15_OP_REPLACE && strcmp(b.s, want) == 0 &&
		      fits(S15_ST_STEP_RUN, b.s),
		      "backplane %u: '%s'", p, b.s);
	}
	CHECK(s15_step_progress_line(&b, S15_STEP_OS, 42) == S15_OP_REPLACE &&
	      strcmp(b.s, "Main DSP\t42%") == 0, "os: '%s'", b.s);
	for (size_t i = 0; i < S15V_N(s15v_progress_rows); i++) {
		const struct s15v_progress_row *r = &s15v_progress_rows[i];
		enum s15_op op = s15_step_progress_line(&b, step_of(r->step), r->p);

		if (r->line == NULL) {
			CHECK(op == S15_OP_NONE, "%s: no row", r->step);
			continue;
		}
		CHECK(op == S15_OP_REPLACE && line_is(S15_ST_STEP_RUN, b.s, r->line) &&
		      fits(S15_ST_STEP_RUN, b.s),
		      "%s %u%%: '%s' want '%s'", r->step, r->p, b.s,
		      r->line);
	}
}

static void test_labels(void)
{
	for (size_t i = 0; i < S15V_N(s15v_labels); i++) {
		const struct s15v_label *l = &s15v_labels[i];
		enum s15_step s = step_of(l->step);

		CHECK(strcmp(s15_step_label(s), l->label) == 0 &&
		      s15_step_number(s) == l->number, "%s", l->step);
		/* The label ends before the status column */
		CHECK(3u + strlen(s15_step_label(s)) < 19u,
		      "label %s fits", l->label);
	}
}

static enum s15_run_kind kind_of(const char *k, const char *name)
{
	if (strcmp(k, "forced") == 0)
		return S15_RUN_FORCED;
	if (strncmp(k, "content", 7) == 0) {
		(void)name;
		return S15_RUN_CONTENT;
	}
	return S15_RUN_FIRMWARE;
}

static void test_headers(void)
{
	struct s15_buf b, rh[4];
	enum s15_style st[4];

	for (size_t i = 0; i < S15V_N(s15v_headers); i++) {
		const struct s15v_header *h = &s15v_headers[i];

		s15_header_line(&b, kind_of(h->kind, h->name), h->version, h->name);
		CHECK(line_is(S15_ST_HEAD, b.s, h->line) && fits(S15_ST_HEAD, b.s),
		      "header %s: '%s' want '%s'", h->kind, b.s, h->line);
	}
	for (size_t i = 0; i < S15V_N(s15v_run_headers); i++) {
		const struct s15v_run_header *h = &s15v_run_headers[i];
		size_t n = s15_run_header(rh, st, kind_of(h->kind, h->name),
					  h->version, h->name, h->stock);
		size_t k;

		CHECK(n == (h->stock ? 4u : 3u), "run header %zu: %zu "
		      "lines", i, n);
		for (k = 0; k < n && h->lines[k] != NULL; k++) {
			enum s15_style want;
			const char *t = parse(h->lines[k], &want);

			CHECK(st[k] == want && fits(st[k], rh[k].s),
			      "run header %zu line %zu: style %s",
			      i, k, s15_style_names[st[k]]);
			if (strcmp(t, "{INFO:stock}") == 0)
				CHECK(row_matches("stock", rh[k].s, NULL) &&
				      strcmp(rh[k].s, s15_stock_line) == 0,
				      "stock line: '%s'", rh[k].s);
			else
				CHECK(strcmp(rh[k].s, t) == 0,
				      "run header %zu line %zu: '%s'",
				      i, k, rh[k].s);
		}
		CHECK(h->lines[k] == NULL && k == n, "run header %zu: %zu lines",
		      i, n);
	}
}

static void test_results(void)
{
	struct s15_buf b;
	enum s15_style st;

	for (size_t i = 0; i < S15V_N(s15v_results); i++) {
		const struct s15v_result *r = &s15v_results[i];
		enum s15_step step = step_of(r->step);
		enum s15_result res = result_of(r->result);
		enum s15_op op;
		const char *want_op;

		st = S15_ST_COUNT;
		op = s15_result_line(&b, &st, step, res, r->version,
				     r->content_only);
		want_op = op == S15_OP_REPLACE ? "replace"
			: op == S15_OP_APPEND ? "append" : NULL;
		CHECK((want_op == NULL && r->op == NULL) ||
		      (want_op != NULL && r->op != NULL && strcmp(want_op, r->op) == 0),
		      "op %s %s", r->step, r->result);
		if (r->line == NULL || op == S15_OP_NONE)
			continue;
		CHECK(fits(st, b.s), "fits: '%s'", b.s);
		CHECK(line_is(st, b.s, r->line), "%s %s: %s:'%s' want '%s'",
		      r->step, r->result, st < S15_ST_COUNT ? s15_style_names[st] : "?",
		      b.s, r->line);
		if (res == S15_RES_SKIPPED) {
			/* Required information, by its token */
			const char *values[] = { "label", s15_step_label(step), NULL };

			CHECK(row_matches("skipped", b.s, values),
			      "skipped: '%s'", b.s);
		}
		if (res == S15_RES_SKIPPED_WRITTEN) {
			/* S-11.068 item 5, by its token */
			const char *values[] = { "label", s15_step_label(step), NULL };

			CHECK(row_matches("skipped:written", b.s, values),
			      "already written: '%s'", b.s);
		}
		if (res == S15_RES_SKIPPED_UNCONFIRMED || res == S15_RES_SKIPPED_OFF ||
		    res == S15_RES_SKIPPED_WRITTEN) {
			/* not the option's reason: S-11.096, S-11.073 */
			const char *values[] = { "label", s15_step_label(step), NULL };

			CHECK(!row_matches("skipped", b.s, values),
			      "not the option's skip: '%s'", b.s);
		}
	}
	/* Every failure result appends the step's number. */
	st = S15_ST_COUNT;
	CHECK(s15_result_line(&b, &st, S15_STEP_BOOTLOADER, S15_RES_BAD_FILE,
			      NULL, false) == S15_OP_APPEND &&
	      st == S15_ST_ERROR && strcmp(b.s, "Operation 10 failed") == 0,
	      "bad file: '%s'", b.s);
	st = S15_ST_COUNT;
	CHECK(s15_result_line(&b, &st, S15_STEP_BLE, S15_RES_INTERNAL, NULL,
			      false) == S15_OP_APPEND &&
	      st == S15_ST_ERROR && strcmp(b.s, "Operation 7 failed") == 0,
	      "internal error: '%s'", b.s);
	/* The failed step's row, with why when the step gave a cause; the
	 * `step failed` token either way. */
	for (size_t i = 0; i < S15V_N(s15v_failed_rows); i++) {
		const struct s15v_failed_row *r = &s15v_failed_rows[i];
		const char *values[] = { "label", s15_step_label(step_of(r->step)), NULL };

		s15_failed_row(&b, step_of(r->step), r->cause);
		CHECK(line_is(S15_ST_STEP_FAIL, b.s, r->line) &&
		      fits(S15_ST_STEP_FAIL, b.s),
		      "failed row %s: '%s' want '%s'", r->step, b.s,
		      r->line);
		CHECK(row_matches("step failed", b.s, values),
		      "step failed: '%s'", b.s);
	}
}

/* S-11.069, S-11.070: the lines under a Nordic step's failure. */
static void test_notes(void)
{
	const char *lines[S15_NOTE_LINES];
	enum s15_style st[S15_NOTE_LINES];

	CHECK(s15_note_lines(lines, st, S15_NOTE_NONE) == 0u, "no note");
	for (size_t i = 0; i < S15V_N(s15v_notes); i++) {
		const struct s15v_note *v = &s15v_notes[i];
		enum s15_note note = strcmp(v->note, "no_app") == 0 ? S15_NOTE_NO_APP
								  : S15_NOTE_MAY_NOT_START;
		size_t n = s15_note_lines(lines, st, note);
		size_t k;

		for (k = 0; k < n && v->lines[k] != NULL; k++)
			CHECK(line_is(st[k], lines[k], v->lines[k]) &&
			      fits(st[k], lines[k]),
			      "note %s line %zu: '%s' want '%s'", v->note, k,
			      lines[k], v->lines[k]);
		CHECK(v->lines[k] == NULL && k == n, "note %s count", v->note);
	}
}

static void test_outcomes(void)
{
	const char *lines[2];
	enum s15_style st[2];

	for (size_t i = 0; i < S15V_N(s15v_outcomes); i++) {
		const struct s15v_outcome *o = &s15v_outcomes[i];
		enum s15_outcome oc = strcmp(o->outcome, "done") == 0 ? S15_OUT_DONE
				    : strcmp(o->outcome, "nothing to do") == 0
					      ? S15_OUT_NOTHING : S15_OUT_FAILED;
		size_t n = s15_outcome_lines(lines, st, oc, o->skipped,
					     o->reason == 40u, o->content_only,
					     false);
		size_t k;

		for (k = 0; k < n && o->lines[k] != NULL; k++)
			CHECK(line_is(st[k], lines[k], o->lines[k]) &&
			      fits(st[k], lines[k]),
			      "outcome %zu line %zu: '%s' want '%s'", i, k,
			      lines[k], o->lines[k]);
		CHECK(o->lines[k] == NULL && k == n, "outcome %zu count", i);
		/* With the Restart item after it, a run with nothing to do
		 * drops its restart line; the others are as without. */
		k = s15_outcome_lines(lines, st, oc, o->skipped,
				      o->reason == 40u, o->content_only, true);
		CHECK(k == (oc == S15_OUT_NOTHING ? 1u : n) &&
		      line_is(st[0], lines[0], o->lines[0]),
		      "outcome %zu with the item", i);
	}
}

/* A completed run names the steps it skipped before its outcome, on one
 * line or two, no name split between them. */
static void test_not_updated(void)
{
	struct s15_buf b[2];
	enum s15_style st[2];
	size_t n;

	for (size_t i = 0; i < S15V_N(s15v_not_updated); i++) {
		const struct s15v_not_updated *u = &s15v_not_updated[i];
		unsigned mask = 0;
		size_t k;

		for (k = 0; u->steps[k] != NULL; k++)
			mask |= 1u << step_of(u->steps[k]);
		n = s15_not_updated(b, st, mask);
		for (k = 0; k < n && u->lines[k] != NULL; k++)
			CHECK(line_is(st[k], b[k].s, u->lines[k]) &&
			      fits(st[k], b[k].s),
			      "not updated %zu line %zu: '%s' want '%s'", i, k,
			      b[k].s, u->lines[k]);
		CHECK(u->lines[k] == NULL && k == n, "not updated %zu: %zu lines",
		      i, n);
	}
	/* every step's name: two lines, each within its row, every name
	 * whole on one of them, no trailing space */
	n = s15_not_updated(b, st, (1u << S15_STEP_COUNT) - 1u);
	CHECK(n == 2u && fits(st[0], b[0].s) && fits(st[1], b[1].s) &&
	      b[0].s[b[0].n - 1u] == ',' && b[1].s[b[1].n - 1u] != ' ',
	      "all steps: '%s' / '%s'", b[0].s, n > 1u ? b[1].s : "");
	for (unsigned k = 0; k < S15_STEP_COUNT && n == 2u; k++) {
		const char *l = s15_step_label((enum s15_step)k);

		CHECK(strstr(b[0].s, l) != NULL || strstr(b[1].s, l) != NULL,
		      "all steps: %s named", l);
	}
	/* no step, or bits past the steps: no line */
	CHECK(s15_not_updated(b, st, 0u) == 0u &&
	      s15_not_updated(b, st, 1u << S15_STEP_COUNT) == 0u,
	      "nothing skipped: no line");
}

/* ---- packs ----------------------------------------------------------- */

static void test_manifest(void)
{
	static uint8_t data[1024];
	char label[64];

	for (size_t i = 0; i < S15V_N(s15v_manifests); i++) {
		const struct s15v_manifest *m = &s15v_manifests[i];
		char name[64];
		size_t n;

		if (m->text != NULL) {
			memset(data, ' ', m->pad);
			memcpy(data + m->pad, m->text, m->len);
			n = s15_manifest_name(data, m->pad + m->len, name);
		} else {
			n = s15_manifest_name(NULL, 0, name);
		}
		CHECK(strcmp(name, m->expect) == 0 && n == strlen(m->expect),
		      "S-15.041 %s: '%s'", m->name, name);
		(void)s15_pack_label(name, m->dir, label);
		CHECK(strcmp(label, m->label) == 0, "label %s", m->name);
	}
	/* A character the screen does not show is '?', so a tab
	 * cannot split the item's row. */
	CHECK(s15_pack_label("a\tb\x7f" "c", "dir", label) == 5u &&
	      strcmp(label, "a?b?c") == 0, "label '%s'", label);
	CHECK(s15_pack_label("", "drums\tv2", label) == 8u &&
	      strcmp(label, "drums?v2") == 0, "dir label '%s'", label);
	CHECK(s15_pack_label(NULL, "dir", label) == 3u &&
	      strcmp(label, "dir") == 0, "no name");
	{
		char long_name[80];

		memset(long_name, 'n', sizeof long_name - 1u);
		long_name[sizeof long_name - 1u] = '\0';
		CHECK(s15_pack_label(long_name, "dir", label) == 63u &&
		      strlen(label) == 63u, "cut to 63");
	}
}

/* ---- required information ------------------------------------------ */

static void test_required_info(void)
{
	struct s15_buf b;
	const char *rows[9];
	char row[80], item[sizeof b.s + 16];
	size_t n;

	/* S-15.140: the item's state as its hint */
	CHECK(row_matches("option-item:on", s15_option_item_on, NULL) &&
	      !row_matches("option-item:off", s15_option_item_on, NULL) &&
	      fits(S15_ST_ITEM_ON, s15_option_item_on),
	      "S-15.140 option item on");
	CHECK(row_matches("option-item:off", s15_option_item_off, NULL) &&
	      !row_matches("option-item:on", s15_option_item_off, NULL) &&
	      fits(S15_ST_ITEM, s15_option_item_off),
	      "S-15.140 option item off");

	/* The option's screen: the rows, its item with the hold's hint, the
	 * way back; held, the same item armed and the hold's row: nine rows. */
	for (n = 0; n < S15_OPTION_PROMPT_LINES; n++) {
		rows[n] = s15_option_prompt[n];
		CHECK(fits(s15_option_prompt_style[n], rows[n]),
		      "S-15.140 prompt row %zu fits in %s", n,
		      s15_style_names[s15_option_prompt_style[n]]);
	}
	s15_hold_hint(&b, 2u);
	snprintf(item, sizeof item, "2 %s\t%s", s15_option_turn_on, b.s);
	CHECK(fits(S15_ST_ITEM, item) && fits(S15_ST_ARMED, item),
	      "S-15.140 the item fits");
	rows[n++] = item;
	rows[n++] = "8 Back to Settings";
	CHECK(s15t_match_named("option prompt", rows, n, NULL),
	      "S-15.140 the screen names every effect and the hold");
	CHECK(s15_option_prompt_style[0] == S15_ST_HEAD &&
	      n + 1u == S15_ROWS, "S-15.140 a question first; held, every row shown");

	/* The title bar's indicator */
	CHECK(row_matches("indicator", s15_indicator_text, NULL) &&
	      fits(S15_ST_TEXT, s15_indicator_text), "indicator");

	/* Device Info's DSP2 value, in its kv row */
	snprintf(row, sizeof row, "DSP2\t%s", s15_slave_no_os);
	CHECK(row_matches("slave:no_os", row, NULL) &&
	      !row_matches("slave:absent", row, NULL), "slave no OS");
	snprintf(row, sizeof row, "DSP2\t%s", s15_slave_absent);
	CHECK(row_matches("slave:absent", row, NULL) &&
	      !row_matches("slave:no_os", row, NULL), "slave absent");
	snprintf(row, sizeof row, "DSP2\t%s", s15_slave_no_start);
	CHECK(row_matches("slave:no_start", row, NULL) &&
	      !row_matches("slave:no_os", row, NULL) &&
	      !row_matches("slave:absent", row, NULL), "slave not started");
	/* One that stopped answering after the start is none of those. */
	snprintf(row, sizeof row, "DSP2\t%s", s15_slave_lost);
	CHECK(!row_matches("slave:no_start", row, NULL) &&
	      !row_matches("slave:no_os", row, NULL) &&
	      !row_matches("slave:absent", row, NULL), "slave lost");
	CHECK(strlen(s15_slave_no_os) <= 17u && strlen(s15_slave_absent) <= 17u &&
	      strlen(s15_slave_no_start) <= 17u && strlen(s15_slave_lost) <= 17u,
	      "the values fit the left value's column");
	CHECK(fits(S15_ST_WARN, s15_drive_lost) && fits(S15_ST_ERROR, s15_reset_lost),
	      "the lost slave's lines fit");

	/* S-15.161: exact text; no token takes it for its own. */
	CHECK(fits(S15_ST_ERROR, s15_os_reinstall), "S-15.161 fits");
	for (size_t t = 0; t < S15V_N(s15v_tokens); t++)
		CHECK(!row_matches(s15v_tokens[t].name, s15_os_reinstall, NULL),
		      "S-15.161 matches token %s", s15v_tokens[t].name);

	/* The refusal's reason. */
	{
		static const char *const reason[] = { "reason", "signature", NULL };
		enum s15_style st;
		const char *want = parse(s15v_reason, &st);

		s15_reason_line(&b, "signature");
		CHECK(row_matches("reason line", b.s, reason) && st == S15_ST_DIM &&
		      strncmp(b.s, want, strlen(want) - strlen("$reason")) == 0 &&
		      strcmp(b.s + strlen(want) - strlen("$reason"), "signature") == 0 &&
		      fits(st, b.s), "reason line: '%s'", b.s);
	}

	/* Reset needed, right after operation 10's failure. */
	rows[0] = "Operation 10 failed";
	rows[1] = s15_reset_needed;
	CHECK(s15t_match_named("reset needed", rows, 2, NULL) &&
	      fits(S15_ST_ERROR, s15_reset_needed), "reset needed");

	CHECK(row_matches("stock", s15_stock_line, NULL) &&
	      fits(S15_ST_WARN, s15_stock_line), "stock");
	/* S-15.162: the interrupted-install notice, over its two rows */
	CHECK(s15t_match_named("interrupted", s15_interrupted,
			       S15_INTERRUPTED_LINES, NULL) &&
	      !row_matches("interrupted", s15_interrupted[0], NULL) &&
	      fits(S15_ST_ERROR, s15_interrupted[0]) &&
	      fits(S15_ST_ERROR, s15_interrupted[1]), "interrupted");

	/* S-15.015: the hold's hint names its key and time; its row fits
	 * with the bar at its widest */
	s15_hold_hint(&b, 7);
	CHECK(strcmp(b.s, "Hold `7` for 5 s") == 0, "S-15.015 hint '%s'", b.s);
	snprintf(row, sizeof row, "1 Format drive\t%.40s", b.s);
	CHECK(fits(S15_ST_ARMED, row), "format: the hold as the item's hint");
	snprintf(row, sizeof row, "7 Factory reset\t%.40s", b.s);
	CHECK(fits(S15_ST_ARMED, row), "factory reset: the hold as the item's hint");
	s15_hold_bar(&b, 100, 5);
	/* the label short of the bar (S15_STEP_LABEL_MAX), the seconds in the
	 * columns after it */
	CHECK(strcmp(b.s, "Release to cancel\t100%\t5 s") == 0 &&
	      part_len(b.s, 0) <= S15_STEP_LABEL_MAX &&
	      S15_STEP_COL + 26u + strlen("5 s") <= S15_COLS, "S-15.015 bar '%s'", b.s);
	s15_hold_bar(&b, 140, 0);
	CHECK(strcmp(b.s, "Release to cancel\t100%\t0 s") == 0, "S-15.015 bar capped");

	/* The other required-information lines fit one row, with a mark. */
	CHECK(fits(S15_ST_WARN, s15_format_unreadable) &&
	      fits(S15_ST_ERROR, s15_reset_unreadable) &&
	      fits(S15_ST_ERROR, s15_reset_incomplete) &&
	      fits(S15_ST_ERROR, s15_os_ldr_unreadable) &&
	      fits(S15_ST_ERROR, s15_os_write_failed) &&
	      fits(S15_ST_WARN, s15_nothing_skipped), "required lines fit");
	CHECK(strstr(s15_os_ldr_unreadable, "/os.ldr") != NULL, "S-15.094 names /os.ldr");
	s15_pack_failed_line(&b, "Bass");
	CHECK(strcmp(b.s, "Could not remove Bass") == 0, "S-15.043 '%s'", b.s);
}

/* S-15 terms: a slot is shown as its index + 1; nothing shows the raw
 * index. */
static void test_slot_number(void)
{
	CHECK(s15_slot_number(0) == 1u && s15_slot_number(1) == 2u,
	      "index 0 shows as 1, index 1 as 2");
}

/* S-15.094: every row, reason and eMMC line fits its row at the largest
 * values it can name (os_write.json holds the exact lines). */
static void test_os_write_lines(void)
{
	struct s15_buf b, why;
	enum s15_style st;

	for (unsigned s = 0; s < S15_OSW_STEP_COUNT; s++) {
		s15_osw_progress_row(&b, (enum s15_osw_step)s, 100);
		CHECK(fits(S15_ST_STEP_RUN, b.s), "S-15.094 progress row '%s'", b.s);
		s15_osw_done_row(&b, (enum s15_osw_step)s, 0x800000u, 65535u);
		CHECK(fits(S15_ST_STEP_OK, b.s), "S-15.094 done row '%s'", b.s);
		s15_osw_failed_row(&b, (enum s15_osw_step)s);
		CHECK(fits(S15_ST_STEP_FAIL, b.s), "S-15.094 failed row '%s'", b.s);
	}
	CHECK(s15_osw_label(S15_OSW_STEP_COUNT)[0] == '\0', "S-15.094 no label past the steps");
	/* A sector or group is below 65536 (B is at most 255 x 128 KiB,
	 * S-03.008); n of a refused length at most 16385 (S-03.051). */
	for (unsigned w = S15_OSW_WHY_REFUSED; w <= S15_OSW_WHY_FLUSH; w++) {
		bool bytes = w == S15_OSW_WHY_READ || w == S15_OSW_WHY_TOO_SMALL ||
			     w == S15_OSW_WHY_TOO_LARGE;

		s15_osw_reason(&why, (enum s15_osw_why)w, 2,
			       bytes ? 4294967295u : w == S15_OSW_WHY_NO_ROOM ? 16385u : 65535u,
			       w == S15_OSW_WHY_NO_ROOM ? 16384u : 0x800000u);
		s15_reason_line(&b, why.s);
		CHECK(why.n > 0u && fits(S15_ST_DIM, b.s), "S-15.094 reason '%s'", b.s);
	}
	s15_osw_reason(&why, S15_OSW_WHY_NO_ROOM, 0, 3601, 0);
	CHECK(strcmp(why.s, "the boot partition size is unknown") == 0, "S-15.094 B unknown");
	s15_osw_reason(&why, S15_OSW_WHY_NONE, 0, 0, 0);
	CHECK(why.n == 0u, "S-15.094 no reason");
	for (unsigned c = S15_OSW_EMMC_UNTOUCHED; c <= S15_OSW_EMMC_UNCONFIRMED; c++)
		for (unsigned t = 1; t <= 2u; t++) {
			s15_osw_emmc_line(&b, &st, (enum s15_osw_emmc)c, t);
			CHECK(fits(st, b.s), "S-15.094 eMMC line '%s'", b.s);
		}
	CHECK(fits(S15_ST_OK, s15_osw_written) && fits(S15_ST_WARN, s15_osw_select0_failed) &&
	      fits(S15_ST_BACK, s15_osw_back), "S-15.094 fixed lines");
}

/*
 * S-15.163: every step's reason fits its row under "   Reason: " and its
 * Device Info value its 42 columns, at the widest values each can have
 * (a sector of an 8 MiB image, any 32-bit word otherwise), lost or not; a
 * lost slave's step says so; no reason is empty.
 */
static void test_start_lines(void)
{
	struct s15_start s;
	struct s15_buf why, line, info;

	CHECK(fits(S15_ST_ERROR, s15_start_failed), "S-15.163 the first line fits");
	CHECK(fits(S15_ST_ERROR, s15_start_failed_file), "S-15.195 the first line fits");
	for (unsigned w = S15_START_NO_ACTIVE; w < S15_START_COUNT; w++)
		for (unsigned k = 0; k < 2u * S15_CHECK_COUNT; k++)
			for (unsigned at = 0; at < S15_AT_COUNT; at++) {
				memset(&s, 0, sizeof(s));
				s.why = (enum s15_start_why)w;
				s.part = 2u;
				s.value = w == S15_START_IMAGE_READ ? 16257u : 0xFFFFFFFFu;
				s.limit = 0xFFFFFFFFu;
				s.check = (enum s15_check)(k / 2u);
				s.lost = (k & 1u) != 0u;
				s.cmd = 0xFFu;
				s.at = (uint8_t)(at | S15_AT_IN_POLL);
				/* S-15.195: a file start's, each reason its read gives */
				s.file = (at & 1u) != 0u || w == S15_START_FILE;
				s.file_why = (enum s15_osw_why)(S15_OSW_WHY_REFUSED +
								at % (S15_OSW_WHY_TOO_LARGE));
				if (w == S15_START_FILE)
					s.limit = 0x800000u;	/* its largest, S03_OS_MAX */
				s15_start_reason(&why, &s);
				s15_reason_line(&line, why.s);
				s15_start_info(&info, &s);
				CHECK(why.n > 0u && fits(S15_ST_DIM, line.s),
				      "S-15.163 reason %u: '%s'", w, line.s);
				CHECK(info.n > 0u && info.n <= S15_START_INFO_MAX,
				      "S-15.163 info %u: '%s'", w, info.s);
				if (s.lost && (w == S15_START_SELECT ||
					       w == S15_START_IMAGE_READ ||
					       w == S15_START_NO_REPLY))
					CHECK(strstr(why.s, "DSP2 stopped answering") != NULL &&
					      strstr(info.s, ": 0xFF ") != NULL,
					      "S-15.163 lost %u: '%s' '%s'", w, why.s, info.s);
			}
	memset(&s, 0, sizeof(s));
	s15_start_reason(&why, &s);
	s15_start_info(&info, &s);
	CHECK(why.n == 0u && info.n == 0u, "S-15.163 no start failed, no words");
	/* S-15.194: every reason of Start /os.ldr once fits under its line */
	CHECK(fits(S15_ST_ERROR, s15_once_not_started) && fits(S15_ST_WARN, s15_once_restarting) &&
	      fits(S15_ST_ITEM, s15_once_item), "S-15.194 its lines fit");
	for (unsigned r = S15_ONCE_FILE; r <= S15_ONCE_DISABLED; r++)
		for (unsigned fw = S15_OSW_WHY_REFUSED; fw <= S15_OSW_WHY_TOO_LARGE; fw++) {
			const struct s15_os_file f = { (enum s15_osw_why)fw, 0xFFFFFFFFu, 0xFFFFFFFFu };

			s15_once_reason(&why, (enum s15_once)r, &f);
			s15_reason_line(&line, why.s);
			CHECK(why.n > 0u && fits(S15_ST_DIM, line.s), "S-15.194 reason %u %u: '%s'",
			      r, fw, line.s);
		}
}

int main(void)
{
	test_token_examples();
	test_start_lines();
	test_keys();
	test_progress();
	test_labels();
	test_headers();
	test_results();
	test_outcomes();
	test_notes();
	test_not_updated();
	test_manifest();
	test_required_info();
	test_slot_number();
	test_os_write_lines();
	return check_done("test_s15_lines");
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15 values and lines that do not depend on the menu's state.
 */
#include "waffle/s15_lines.h"
#include "waffle/s15_menu.h"

#include "waffle/div64.h"

/* ---- keys ------------------------------------------------------------ */

/* S-15.011, S-15.016: the device's digit keys (device.key.digit1 to
 * digit5to8). */
unsigned s15_digit(const struct wb_device *dev, unsigned key)
{
	return wb_key_digit(dev, key);
}

bool s15_knob_click(const struct wb_device *dev, unsigned key)
{
	unsigned d;

	for (d = 0; d < 4u; d++)
		if (key == dev->keys.digit[d][dev->keys.digit_knob])
			return true;
	return false;
}

/* ---- buffers --------------------------------------------------------- */

void s15_buf_init(struct s15_buf *b)
{
	b->n = 0;
	b->s[0] = '\0';
}

void s15_buf_strn(struct s15_buf *b, const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n && s[i] != '\0' && b->n < S15_BUF - 1u; i++)
		b->s[b->n++] = s[i];
	b->s[b->n] = '\0';
}

void s15_buf_str(struct s15_buf *b, const char *s)
{
	s15_buf_strn(b, s, S15_BUF);
}

void s15_buf_dec(struct s15_buf *b, uint32_t v)
{
	char d[10];
	size_t n = 0;

	do {
		d[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0u);
	while (n > 0u) {
		const char c[2] = { d[--n], '\0' };

		s15_buf_str(b, c);
	}
}

/* ---- the upgrade run ------------------------------------------------- */

/* S-16.022 */
unsigned s15_progress(uint32_t done, uint32_t total)
{
	if (total == 0u)
		return 100;
	return wb_percent32(done, total);
}

static const struct {
	const char *label;
	unsigned number;
} steps[S15_STEP_COUNT] = {
	[S15_STEP_CONTENT] = { "Content", 11 },
	[S15_STEP_OS] = { "Main DSP", 6 },
	[S15_STEP_BOOTLOADER] = { "Bootloader", 10 },
	[S15_STEP_BLE] = { "BLE", 7 },
	[S15_STEP_BLE_BOOTLOADER] = { "BLE bootloader", 8 },
	[S15_STEP_KEYBOARD] = { "Backplane", 9 },
};

static enum s15_step valid_step(enum s15_step step)
{
	if ((unsigned)step < S15_STEP_COUNT)
		return step;
	return S15_STEP_CONTENT;
}

const char *s15_step_label(enum s15_step step)
{
	return steps[valid_step(step)].label;
}

unsigned s15_step_number(enum s15_step step)
{
	return steps[valid_step(step)].number;
}

/* S-11.090: the steps the session option guards. */
bool s15_step_protected(enum s15_step step)
{
	return step == S15_STEP_BOOTLOADER;
}

/* A step row: its label, a tab, then its status. */
static void label_open(struct s15_buf *out, enum s15_step step)
{
	s15_buf_init(out);
	s15_buf_str(out, s15_step_label(step));
	s15_buf_str(out, "\t");
}

enum s15_op s15_step_progress_line(struct s15_buf *out, enum s15_step step,
				   unsigned p)
{
	static const char spin[] = "/-\\|";

	label_open(out, step);
	if (step == S15_STEP_KEYBOARD) {
		s15_buf_str(out, "Installing ");
		s15_buf_strn(out, &spin[p % 4u], 1);
	} else {
		s15_buf_dec(out, p);
		s15_buf_str(out, "%");
	}
	return S15_OP_REPLACE;
}

enum s15_op s15_result_line(struct s15_buf *out, enum s15_style *style,
			    enum s15_step step, enum s15_result result,
			    const char *version, bool content_only)
{
	if (content_only && step != S15_STEP_CONTENT &&
	    (result == S15_RES_NOT_FOUND || result == S15_RES_ALREADY))
		return S15_OP_NONE;
	switch (result) {
	case S15_RES_OK:
		*style = S15_ST_STEP_OK;
		label_open(out, step);
		s15_buf_str(out, "Installed");
		if (step != S15_STEP_CONTENT && version != NULL) {
			s15_buf_str(out, " ");
			s15_buf_str(out, version);
		}
		/* S-11.090: a protected step only runs with the option on. */
		if (s15_step_protected(step))
			s15_buf_str(out, " (protected)");
		return S15_OP_REPLACE;
	case S15_RES_NOT_FOUND:
		*style = S15_ST_STEP_SKIP;
		label_open(out, step);
		s15_buf_str(out, "Not included");
		return S15_OP_REPLACE;
	case S15_RES_ALREADY:
		*style = S15_ST_STEP_SKIP;
		label_open(out, step);
		s15_buf_str(out, "Already installed");
		return S15_OP_REPLACE;
	case S15_RES_SKIPPED:
	case S15_RES_SKIPPED_UNSELECTED:
	case S15_RES_SKIPPED_OFF:
	case S15_RES_SKIPPED_WRITTEN:
		*style = S15_ST_STEP_SKIP;
		label_open(out, step);
		s15_buf_str(out, result == S15_RES_SKIPPED ?
				 "Skipped: protected writes off" :
				 result == S15_RES_SKIPPED_OFF ?
				 "Skipped: not in this version" :
				 result == S15_RES_SKIPPED_WRITTEN ?
				 "Skipped: written, runs on restart" :
				 "Skipped: not selected");
		return S15_OP_REPLACE;
	case S15_RES_FAILURE:
	case S15_RES_BAD_FILE:
	case S15_RES_INTERNAL:
	default:
		*style = S15_ST_ERROR;
		s15_buf_init(out);
		s15_buf_str(out, "Operation ");
		s15_buf_dec(out, s15_step_number(step));
		s15_buf_str(out, " failed");
		return S15_OP_APPEND;
	}
}

/* The row of a step whose operation failed. */
void s15_failed_row(struct s15_buf *out, enum s15_step step,
		    const char *cause)
{
	label_open(out, step);
	s15_buf_str(out, "Failed");
	if (cause != NULL && cause[0] != '\0') {
		s15_buf_str(out, ": ");
		s15_buf_strn(out, cause, S15_CAUSE_MAX);
	}
}

void s15_header_line(struct s15_buf *out, enum s15_run_kind kind,
		     const char *version, const char *name)
{
	s15_buf_init(out);
	switch (kind) {
	case S15_RUN_FIRMWARE:
	case S15_RUN_FORCED:
		s15_buf_str(out, kind == S15_RUN_FORCED ? "Forced upgrade"
							: "Upgrading");
		s15_buf_str(out, " to ");
		s15_buf_str(out, version != NULL ? version : "");
		break;
	case S15_RUN_CONTENT:
	default:
		if (name != NULL && name[0] != '\0') {
			s15_buf_str(out, "Installing \"");
			s15_buf_str(out, name);
			s15_buf_str(out, "\"");
		} else {
			s15_buf_str(out, "Installing content");
		}
		break;
	}
}

size_t s15_run_header(struct s15_buf out[S15_RUN_HEADER_MAX],
		      enum s15_style style[S15_RUN_HEADER_MAX],
		      enum s15_run_kind kind, const char *version,
		      const char *name, bool stock, bool te_signed)
{
	size_t n = 0;

	style[n] = S15_ST_HEAD;
	s15_header_line(&out[n++], kind, version, name);
	if (te_signed) {
		style[n] = S15_ST_OK;
		s15_buf_init(&out[n]);
		s15_buf_str(&out[n++], s15_signed_line);
	}
	if (stock) {
		style[n] = S15_ST_WARN;
		s15_buf_init(&out[n]);
		s15_buf_str(&out[n++], s15_stock_line);
	}
	style[n] = S15_ST_WARN;
	s15_buf_init(&out[n]);
	s15_buf_str(&out[n++], "Keep the power on");
	style[n] = S15_ST_DIM;
	s15_buf_init(&out[n]);
	s15_buf_str(&out[n++], "The screen may go dark briefly");
	return n;
}

/* A warn line's text starts in column 3. */
#define WARN_ROOM (S15_COLS - 3u)

size_t s15_not_updated(struct s15_buf out[2], enum s15_style style[2],
		       unsigned skipped)
{
	size_t n = 0;

	skipped &= (1u << S15_STEP_COUNT) - 1u;
	if (skipped == 0u)
		return 0;
	style[0] = S15_ST_WARN;
	s15_buf_init(&out[0]);
	s15_buf_str(&out[0], "Not updated: ");
	for (unsigned step = 0; step < S15_STEP_COUNT; step++) {
		const char *label = s15_step_label((enum s15_step)step);
		size_t len = 0;

		if ((skipped & (1u << step)) == 0u)
			continue;
		skipped &= ~(1u << step);
		while (label[len] != '\0')
			len++;
		/* the name and its comma stay on one line */
		if (n == 0u && out[0].n + len + 1u > WARN_ROOM) {
			/* the first line ends at its comma, with no space */
			if (out[0].s[out[0].n - 1u] == ' ')
				out[0].s[--out[0].n] = '\0';
			n = 1;
			style[1] = S15_ST_HEAD;
			s15_buf_init(&out[1]);
			s15_buf_str(&out[1], "   ");
		}
		s15_buf_str(&out[n], label);
		if (skipped != 0u)
			s15_buf_str(&out[n], out[n].n + 1u < WARN_ROOM ? ", "
								       : ",");
	}
	return n + 1u;
}

void s15_restart_line(const struct wb_device *dev, struct s15_buf *out)
{
	s15_buf_init(out);
	s15_buf_str(out, "Restart the ");
	s15_buf_str(out, dev->ident.product_text);
	s15_buf_str(out, ".");
}

size_t s15_outcome_lines(const struct wb_device *dev, const char *out[2],
			 struct s15_buf *restart, enum s15_style style[2],
			 enum s15_outcome outcome, bool any_skipped,
			 bool reason40, bool content_only, bool cont)
{
	style[1] = S15_ST_TEXT;
	s15_restart_line(dev, restart);
	out[1] = restart->s;
	switch (outcome) {
	case S15_OUT_DONE:
		style[0] = S15_ST_OK;
		if (reason40) {
			out[0] = "Done! Wait for the restart.";
			return 1;
		}
		out[0] = "Done!";
		return 1;
	case S15_OUT_NOTHING:
		style[0] = any_skipped ? S15_ST_WARN : S15_ST_OK;
		out[0] = any_skipped ? s15_nothing_skipped
				     : "Up to date.";
		return cont ? 1 : 2;	/* the item says what comes next */
	case S15_OUT_FAILED:
	default:
		style[0] = S15_ST_ERROR;
		out[0] = content_only ? "Installation failed." : "Upgrade failed.";
		return 2;
	}
}

size_t s15_note_lines(const char *out[S15_NOTE_LINES],
		      enum s15_style style[S15_NOTE_LINES], enum s15_note note)
{
	switch (note) {
	case S15_NOTE_NO_APP:		/* S-08.048 */
		style[0] = S15_ST_ERROR;
		out[0] = "The BLE chip has no application.";
		style[1] = S15_ST_TEXT;
		out[1] = "Install the package again.";
		return 2;
	case S15_NOTE_MAY_NOT_START:	/* S-08 open question 11 */
		style[0] = S15_ST_ERROR;
		out[0] = "The BLE chip may not start.";
		return 1;
	case S15_NOTE_NONE:
	default:
		return 0;
	}
}

/* ---- content packs --------------------------------------------------- */

/* S-15.041 */
size_t s15_manifest_name(const uint8_t *data, size_t len, char out[64])
{
	static const char key[] = "\"name\"";
	size_t i, k, n = 0;

	out[0] = '\0';
	if (data == NULL)
		return 0;
	if (len > 511u)
		len = 511u;
	for (i = 0; i + 6u <= len; i++) {
		for (k = 0; k < 6u && data[i + k] == (uint8_t)key[k]; k++)
			;
		if (k == 6u)
			break;
	}
	if (i + 6u > len)
		return 0;
	i += 6u;
	while (i < len && (data[i] == ' ' || data[i] == '\t' ||
			   data[i] == '\r' || data[i] == '\n' || data[i] == ':'))
		i++;
	if (i >= len || data[i] != '"')
		return 0;
	for (i++; i < len && data[i] != '"' && n < 63u; i++)
		out[n++] = (char)data[i];
	out[n] = '\0';
	return n;
}

size_t s15_pack_label(const char *name, const char *dir, char out[64])
{
	const char *s = (name != NULL && name[0] != '\0') ? name : dir;
	size_t n = 0;

	/* A character the screen does not show is '?' (a tab
	 * would split the item's row). */
	while (s != NULL && s[n] != '\0' && n < 63u) {
		unsigned char c = (unsigned char)s[n];

		out[n] = (char)((c >= 0x20u && c <= 0x7eu) ? c : '?');
		n++;
	}
	out[n] = '\0';
	return n;
}

/* ---- S-15.132: the hand-over frame ----------------------------------- */

uint16_t s15_logo_pixel(unsigned x, unsigned y)
{
	unsigned i, left = 0, dx, dy;

	if (y < 104u || y > 117u || x < 59u)
		return S15_BLACK;
	for (i = 0; i < 16u; i++) {
		left = 59u + 22u * i + 6u * (i / 4u);
		if (x < left)
			return S15_BLACK;
		if (x < left + 14u)
			break;
	}
	if (i == 16u)
		return S15_BLACK;
	if (i % 4u == 0u)
		return S15_WHITE;
	dx = x - left;
	dy = y - 104u;
	if (dx < 2u || dy < 2u || dx > 11u || dy > 11u)
		return S15_FAINT;
	return S15_BLACK;
}

uint16_t s15_handover_pixel(unsigned c, unsigned r)
{
	/* S-13c.023: window column c is screen y, row r screen x 479 - r. */
	if (c >= S15_HANDOVER_VISIBLE || r >= S15_HANDOVER_ROWS)
		return S15_BLACK;
	return s15_logo_pixel(S15_HANDOVER_ROWS - 1u - r, c);
}

void s15_handover_frame(void (*put)(void *ctx, uint16_t pixel), void *ctx)
{
	for (unsigned r = 0; r < S15_HANDOVER_ROWS; r++)
		for (unsigned c = 0; c < S15_HANDOVER_COLS; c++)
			put(ctx, s15_handover_pixel(c, r));
}

/* ---- required information -------------------------------------------- */

/* S-15.140: the option's screen, its question and every effect; its item's
 * hint names the hold. */
const char *const s15_option_prompt[S15_OPTION_PROMPT_LINES] = {
	"Allow writes to the following? (until restart)",
	"- Bootloader",
	"- Factory metadata (serial, etc)",
};
const enum s15_style s15_option_prompt_style[S15_OPTION_PROMPT_LINES] = {
	S15_ST_HEAD, S15_ST_TEXT, S15_ST_TEXT,
};
const char s15_option_turn_on[] = "Turn on";
const char s15_option_item_on[] = "2 Protected writes\tOn";
const char s15_option_item_off[] = "2 Protected writes\tOff";
const char s15_slave_no_os[] = "No OS";
const char s15_slave_absent[] = "Absent";
const char s15_slave_no_start[] = "Not started";
const char s15_slave_lost[] = "Not answering";
const char s15_drive_lost[] = "DSP2 not answering: drive unavailable";
const char s15_reset_lost[] = "Not removed: DSP2 not answering";
const char s15_device_unknown[] = "Device unknown: see 1 More";
const char s15_mtp_item_shift[] = "2 MTP file transfer\tShift: macOS";

static void buf_hex(struct s15_buf *b, uint32_t v)
{
	static const char digits[] = "0123456789ABCDEF";
	char t[11];
	unsigned i;

	t[0] = '0';
	t[1] = 'x';
	for (i = 0; i < 8u; i++)
		t[2u + i] = digits[(v >> (28u - 4u * i)) & 0xFu];
	t[10] = '\0';
	s15_buf_str(b, t);
}

void s15_first_stage_text(struct s15_buf *out, uint32_t n, uint32_t c)
{
	s15_buf_init(out);
	s15_buf_str(out, "n ");
	buf_hex(out, n);
	s15_buf_str(out, " c ");
	buf_hex(out, c);
}
const char s15_checking_files[] = "Checking files...";
const char s15_settings_host[] = "Not saved: set from the computer";
const char s15_settings_this_start[] = "Not saved: this start only";
const char s15_settings_failed[] = "Not saved: flash write failed";
const char s15_settings_unusable[] = "Not saved: slot can't keep settings";
const char s15_reset_settings_failed[] = "Settings not cleared: flash write failed";
const char s15_reset_logo_failed[] = "Boot logo not removed: flash write failed";

void s15_files_text(struct s15_buf *out, unsigned way, uint32_t objects)
{
	static const char *const why[] = {
		[S15_FILES_NO_CKPT] = "Scanned (no checkpoint)",
		[S15_FILES_CKPT_INVALID] = "Scanned (checkpoint not valid)",
		[S15_FILES_CKPT_STALE] = "Scanned (checkpoint stale)",
		[S15_FILES_CKPT_UNREADABLE] = "Scanned (checkpoint unreadable)",
		[S15_FILES_FOR_WRITE] = "Scanned (before a write)",
		[S15_FILES_SCANNED] = "Scanned",
	};

	s15_buf_init(out);
	if (way == S15_FILES_RESTORED) {
		s15_buf_str(out, "Restored (");
		s15_buf_dec(out, objects);
		s15_buf_str(out, " objects)");
	} else if (way < sizeof why / sizeof why[0] && why[way] != NULL) {
		s15_buf_str(out, why[way]);
	} else {
		s15_buf_str(out, "Not mounted");
	}
}
const char s15_os_reinstall[] =
	"OS needs reinstalling: install a TE firmware update";
const char *const s15_interrupted[S15_INTERRUPTED_LINES] = {
	"Last install did not finish.",
	"Install it again.",
};
const char s15_restarting[] = "Restarting...";
const char s15_restart_item[] = "6 Restart";
const char s15_continue_item[] = "7 Continue to OS";
const char s15_format_unreadable[] =
	"Drive unreadable: a format erases it";
const char s15_reset_unreadable[] =
	"Not removed: drive unreadable";
const char s15_reset_incomplete[] = "Some user data not removed";
const char s15_os_ldr_unreadable[] = "Can't read /os.ldr";
const char s15_os_write_failed[] = "OS write failed";
const char s15_reset_needed[] =
	"Reset needed before a bootloader write";
const char s15_nothing_skipped[] = "Nothing installed.";
const char s15_stock_line[] = "Replaces waffle-boot: back to stock";
const char s15_signed_line[] = "Signed by TE";

/* S-15.015: under the items a hold chooses, while none is held, and in
 * the label of an item that names its hold. */
const char s15_hold_line[] = "   Hold to select";
const char s15_hold_named[] = " (hold to select)";

/* S-15.015: the hold's last row. */
void s15_hold_bar(struct s15_buf *out, unsigned fill, unsigned left_s)
{
	s15_buf_init(out);
	s15_buf_str(out, "Release to cancel\t");
	s15_buf_dec(out, fill > 100u ? 100u : fill);
	s15_buf_str(out, "%\t");
	s15_buf_dec(out, left_s);
	s15_buf_str(out, " s");
}

/* The refusal's reason, under the line it explains. */
void s15_reason_line(struct s15_buf *out, const char *reason)
{
	s15_buf_init(out);
	s15_buf_str(out, "   Reason: ");
	s15_buf_str(out, reason != NULL ? reason : "");
}

/* ---- S-15.163: a plain start that failed ----------------------------- */

const char s15_start_failed[] = "The OS could not be started.";
const char s15_start_failed_file[] = "/os.ldr could not be started.";

static void buf_hex8(struct s15_buf *b, uint32_t v)
{
	static const char digits[] = "0123456789abcdef";

	for (int i = 28; i >= 0; i -= 4)
		s15_buf_strn(b, &digits[(v >> (unsigned)i) & 0xFu], 1);
}

static void buf_hex2(struct s15_buf *b, uint8_t v)
{
	static const char digits[] = "0123456789ABCDEF";

	s15_buf_str(b, "0x");
	s15_buf_strn(b, &digits[v >> 4], 1);
	s15_buf_strn(b, &digits[v & 0xFu], 1);
}

/* Where the bridge's exchange broke, by S09_AT_... */
static const char *const start_at[S15_AT_COUNT] = {
	"lost", "send failed", "no ready", "no reply", "reply broke",
	"bad reply", "bad check", "timed out", "slave fell", "bad value",
};

static const char *const check_reason[S15_CHECK_COUNT] = {
	"image too long to stage",
	"bad block headers",
	"block flag not allowed",
	"loads outside memory",
	"loads over the staging area",
	"loads over the hand-over reserve",
	"loads over the reboot record",
	"loads over the ROM's boot reserve",
};

static const char *const check_info[S15_CHECK_COUNT] = {
	"too long", "block headers", "block flags", "outside memory",
	"over staging", "over reserve", "over reboot record",
	"over boot reserve",
};

/* The steps whose failure was the bridge's when the slave was lost. */
static bool bridge_step(enum s15_start_why why)
{
	return why == S15_START_SELECT || why == S15_START_HEADER_READ ||
	       why == S15_START_IMAGE_READ || why == S15_START_USER_SELECT ||
	       why == S15_START_NO_REPLY;
}

void s15_start_reason(struct s15_buf *out, const struct s15_start *s)
{
	const bool lost = s->lost && bridge_step(s->why);

	s15_buf_init(out);
	if (lost)
		s15_buf_str(out, "DSP2 stopped answering ");
	switch (s->why) {
	case S15_START_NO_ACTIVE:
		s15_buf_str(out, "no active boot partition");
		break;
	case S15_START_SELECT:
		if (lost) {
			s15_buf_str(out, "at the select");
			break;
		}
		s15_buf_str(out, "boot partition ");
		s15_buf_dec(out, s->part);
		s15_buf_str(out, " select failed");
		break;
	case S15_START_HEADER_READ:
		if (lost) {
			s15_buf_str(out, "at the header read");
			break;
		}
		s15_buf_str(out, "partition ");
		s15_buf_dec(out, s->part);
		s15_buf_str(out, " header read failed");
		break;
	case S15_START_IMAGE_READ:
		s15_buf_str(out, lost ? "during the image read" :
					"image read failed");
		break;
	case S15_START_USER_SELECT:
		s15_buf_str(out, lost ? "at the last select" :
					"user area select failed");
		break;
	case S15_START_FORMAT:
		s15_buf_str(out, "header format ");
		s15_buf_dec(out, s->value);
		s15_buf_str(out, " not supported");
		break;
	case S15_START_ZERO_LENGTH:
		s15_buf_str(out, "image length 0");
		break;
	case S15_START_TOO_LONG:
		s15_buf_str(out, "image over 8 MiB");
		break;
	case S15_START_PAST_PARTITION:
		s15_buf_str(out, "image runs past its partition");
		break;
	case S15_START_CRC:
		s15_buf_str(out, "image CRC mismatch");
		break;
	case S15_START_ENCRYPTED:
		s15_buf_str(out, "image is encrypted");
		break;
	case S15_START_REFUSED:
		s15_buf_str(out, check_reason[(unsigned)s->check < S15_CHECK_COUNT ?
					      (unsigned)s->check : 0u]);
		break;
	case S15_START_SLAVE_NO_OS:
		/* S-15.195: DSP2's copy unread, or not the master's */
		s15_buf_str(out, s->file ? "DSP2 could not load the same /os.ldr" :
					   "DSP2 could not load the OS");
		break;
	case S15_START_FILE:
		s15_osw_reason(out, s->file_why, 0u, s->value, s->limit);
		break;
	case S15_START_NO_REPLY:
		s15_buf_str(out, lost ? "at the release" :
					"DSP2 did not answer the release");
		break;
	case S15_START_SLAVE_START:
		s15_buf_str(out, "DSP2's OS did not start");
		break;
	case S15_START_OK:
	case S15_START_COUNT:
	default:
		break;
	}
}

/* A lost slave's step: the request, then where it broke. */
static void lost_info(struct s15_buf *out, const struct s15_start *s)
{
	const unsigned at = s->at & ~S15_AT_IN_POLL;

	s15_buf_str(out, ": ");
	buf_hex2(out, s->cmd);
	s15_buf_str(out, " ");
	s15_buf_str(out, start_at[at < S15_AT_COUNT ? at : 0u]);
}

/* S-15.195: a file start's own step, for Device Info. */
static void file_info(struct s15_buf *out, const struct s15_start *s)
{
	struct s15_buf why;

	s15_buf_str(out, "/os.ldr ");
	if (s->file_why == S15_OSW_WHY_MISSING) {
		s15_buf_str(out, "not on the drive");
	} else if (s->file_why == S15_OSW_WHY_DIRECTORY) {
		s15_buf_str(out, "is a directory");
	} else if (s->file_why == S15_OSW_WHY_REFUSED) {
		s15_buf_str(out, "on a drive that can't be read");
	} else {
		s15_osw_reason(&why, s->file_why, 0u, s->value, s->limit);
		s15_buf_str(out, why.s);
	}
}

void s15_start_info(struct s15_buf *out, const struct s15_start *s)
{
	const bool lost = s->lost && bridge_step(s->why);

	s15_buf_init(out);
	switch (s->why) {
	case S15_START_NO_ACTIVE:
		s15_buf_str(out, "No active boot partition");
		break;
	case S15_START_SELECT:
		s15_buf_str(out, lost ? "Select of partition " : "Partition ");
		s15_buf_dec(out, s->part);
		if (!lost)
			s15_buf_str(out, " select failed");
		break;
	case S15_START_HEADER_READ:
		s15_buf_str(out, lost ? "Header of partition " : "Partition ");
		s15_buf_dec(out, s->part);
		if (!lost)
			s15_buf_str(out, " header read failed");
		break;
	case S15_START_IMAGE_READ:
		if (!lost) {
			s15_buf_str(out, "Partition ");
			s15_buf_dec(out, s->part);
			s15_buf_str(out, " sector ");
		} else {
			s15_buf_str(out, "Sector ");
		}
		s15_buf_dec(out, s->value);
		s15_buf_str(out, lost ? " read" : " read failed");
		break;
	case S15_START_USER_SELECT:
		s15_buf_str(out, lost ? "User area select" : "User area select failed");
		break;
	case S15_START_FORMAT:
		s15_buf_str(out, "Header format ");
		s15_buf_dec(out, s->value);
		s15_buf_str(out, " not supported");
		break;
	case S15_START_ZERO_LENGTH:
		s15_buf_str(out, "Header length 0");
		break;
	case S15_START_TOO_LONG:
		s15_buf_str(out, "Length ");
		s15_buf_dec(out, s->value);
		s15_buf_str(out, ", at most ");
		s15_buf_dec(out, s->limit);
		break;
	case S15_START_PAST_PARTITION:
		s15_buf_str(out, "Needs ");
		s15_buf_dec(out, s->value);
		s15_buf_str(out, " sectors, has ");
		s15_buf_dec(out, s->limit);
		break;
	case S15_START_CRC:
		s15_buf_str(out, "CRC ");
		buf_hex8(out, s->value);
		s15_buf_str(out, ", header says ");
		buf_hex8(out, s->limit);
		break;
	case S15_START_ENCRYPTED:
		s15_buf_str(out, "Partition ");
		s15_buf_dec(out, s->part);
		s15_buf_str(out, " image is encrypted");
		break;
	case S15_START_REFUSED:
		s15_buf_str(out, "Hand-over check: ");
		s15_buf_str(out, check_info[(unsigned)s->check < S15_CHECK_COUNT ?
					    (unsigned)s->check : 0u]);
		break;
	case S15_START_SLAVE_NO_OS:
		s15_buf_str(out, s->file ? "DSP2 refused /os.ldr, reply " :
					   "DSP2 answered no OS, reply ");
		s15_buf_dec(out, s->value);
		break;
	case S15_START_FILE:
		file_info(out, s);
		break;
	case S15_START_NO_REPLY:
		s15_buf_str(out, lost ? "Release" : "No reply to the release");
		break;
	case S15_START_SLAVE_START:
		s15_buf_str(out, "DSP2's OS did not start");
		break;
	case S15_START_OK:
	case S15_START_COUNT:
	default:
		break;
	}
	if (lost)
		lost_info(out, s);
}

/* S-15.043: a pack that could not be removed. */
void s15_pack_failed_line(struct s15_buf *out, const char *label)
{
	s15_buf_init(out);
	s15_buf_str(out, "Could not remove ");
	s15_buf_str(out, label);
}

/* ---- S-15.094 -------------------------------------------------------- */

static const char *const osw_labels[S15_OSW_STEP_COUNT] = {
	[S15_OSW_STEP_READ] = "Read /os.ldr",
	[S15_OSW_STEP_CHECK] = "Check image",
	[S15_OSW_STEP_TARGET] = "Target",
	[S15_OSW_STEP_ERASE] = "Erase",
	[S15_OSW_STEP_WRITE] = "Write",
	[S15_OSW_STEP_VERIFY] = "Verify",
	[S15_OSW_STEP_ACTIVATE] = "Activate",
};

const char s15_osw_written[] = "OS written.";
const char s15_osw_select0_failed[] = "The eMMC did not switch back: restart";
const char s15_osw_back[] = "8 Back to Developer";

/* ---- S-15.194: Start /os.ldr once (does not write) ----------- */

const char s15_once_item[] = "3 Start /os.ldr once (does not write)";
const char s15_once_not_started[] = "/os.ldr was not started";
const char s15_once_restarting[] = "Restarting into /os.ldr once";

void s15_once_reason(struct s15_buf *out, enum s15_once why,
		     const struct s15_os_file *file)
{
	s15_buf_init(out);
	switch (why) {
	case S15_ONCE_FILE:
		if (file != NULL)
			s15_osw_reason(out, file->why, 0u, file->value, file->limit);
		break;
	case S15_ONCE_SLAVE:
		s15_buf_str(out, "DSP2 not answering: drive unavailable");
		break;
	case S15_ONCE_DISABLED:
		s15_buf_str(out, "the test OS starts no OS");
		break;
	case S15_ONCE_STARTED:
	default:
		break;
	}
}

/* ---- S-15.190, S-05.033: continue to the OS, at a host's request ---- */

const char s15_cont_restarting[] = "Restarting into the OS...";

const char *s15_osw_label(enum s15_osw_step step)
{
	return (unsigned)step < S15_OSW_STEP_COUNT ? osw_labels[step] : "";
}

static void osw_open(struct s15_buf *out, enum s15_osw_step step)
{
	s15_buf_init(out);
	s15_buf_str(out, s15_osw_label(step));
	s15_buf_str(out, "\t");
}

void s15_osw_progress_row(struct s15_buf *out, enum s15_osw_step step, unsigned p)
{
	osw_open(out, step);
	s15_buf_dec(out, p);
	s15_buf_str(out, "%");
}

void s15_osw_done_row(struct s15_buf *out, enum s15_osw_step step, uint32_t a, uint32_t b)
{
	osw_open(out, step);
	switch (step) {
	case S15_OSW_STEP_READ:
		s15_buf_dec(out, a);
		s15_buf_str(out, " bytes");
		break;
	case S15_OSW_STEP_CHECK:
		s15_buf_str(out, "Fits: ");
		s15_buf_dec(out, a);
		s15_buf_str(out, " of ");
		s15_buf_dec(out, b);
		s15_buf_str(out, " sectors");
		break;
	case S15_OSW_STEP_TARGET:
		s15_buf_str(out, "Boot partition ");
		s15_buf_dec(out, a);
		break;
	case S15_OSW_STEP_ACTIVATE:
		s15_buf_str(out, "Boot partition ");
		s15_buf_dec(out, a);
		s15_buf_str(out, " active");
		break;
	case S15_OSW_STEP_ERASE:
	case S15_OSW_STEP_WRITE:
	case S15_OSW_STEP_VERIFY:
	case S15_OSW_STEP_COUNT:
	default:
		s15_buf_str(out, "Done");
		break;
	}
}

void s15_osw_failed_row(struct s15_buf *out, enum s15_osw_step step)
{
	osw_open(out, step);
	s15_buf_str(out, "Failed");
}

/* "<what> <n> of partition <part> <how>" */
static void osw_where(struct s15_buf *out, const char *what, uint32_t n,
		      unsigned part, const char *how)
{
	s15_buf_str(out, what);
	s15_buf_dec(out, n);
	s15_buf_str(out, " of partition ");
	s15_buf_dec(out, part);
	s15_buf_str(out, how);
}

void s15_osw_reason(struct s15_buf *out, enum s15_osw_why why, unsigned part,
		    uint32_t value, uint32_t limit)
{
	s15_buf_init(out);
	switch (why) {
	case S15_OSW_WHY_REFUSED:
		s15_buf_str(out, "drive unreadable");
		break;
	case S15_OSW_WHY_MISSING:
		s15_buf_str(out, "no /os.ldr on the drive");
		break;
	case S15_OSW_WHY_DIRECTORY:
		s15_buf_str(out, "/os.ldr is a directory");
		break;
	case S15_OSW_WHY_READ:
		s15_buf_str(out, "read failed, error ");
		s15_buf_dec(out, value);
		break;
	case S15_OSW_WHY_MEMORY:
		s15_buf_str(out, "not enough memory");
		break;
	case S15_OSW_WHY_TOO_SMALL:
	case S15_OSW_WHY_TOO_LARGE:
		s15_buf_dec(out, value);
		s15_buf_str(out, why == S15_OSW_WHY_TOO_SMALL ? " bytes, at least " :
								 " bytes, at most ");
		s15_buf_dec(out, limit);
		break;
	case S15_OSW_WHY_NO_ROOM:
		if (limit == 0u) {
			s15_buf_str(out, "boot partition size unknown");
			break;
		}
		s15_buf_str(out, "needs ");
		s15_buf_dec(out, value);
		s15_buf_str(out, " sectors, partition has ");
		s15_buf_dec(out, limit);
		break;
	case S15_OSW_WHY_SELECT:
		s15_buf_str(out, "selecting partition ");
		s15_buf_dec(out, part);
		s15_buf_str(out, " failed");
		break;
	case S15_OSW_WHY_EMMC_READ:
		osw_where(out, "reading sector ", value, part, " failed");
		break;
	case S15_OSW_WHY_EMMC_WRITE:
		osw_where(out, "writing sector ", value, part, " failed");
		break;
	case S15_OSW_WHY_ERASE:
		osw_where(out, "erasing group ", value, part, " failed");
		break;
	case S15_OSW_WHY_MISMATCH:
		osw_where(out, "sector ", value, part, " read back wrong");
		break;
	case S15_OSW_WHY_DISABLED:
		s15_buf_str(out, "OS write disabled in the test OS");
		break;
	case S15_OSW_WHY_FLUSH:
		s15_buf_str(out, "the eMMC did not commit");
		break;
	case S15_OSW_WHY_READS_DIFFER:
		s15_buf_str(out, "two reads differ");
		break;
	case S15_OSW_WHY_NOT_OS:
		s15_buf_str(out, "not a bootable OS");
		break;
	case S15_OSW_WHY_NONE:
	default:
		break;
	}
}

void s15_osw_emmc_line(struct s15_buf *out, enum s15_style *style,
		       enum s15_osw_emmc emmc, unsigned target)
{
	s15_buf_init(out);
	*style = S15_ST_WARN;
	switch (emmc) {
	case S15_OSW_EMMC_UNCHANGED:
		*style = S15_ST_TEXT;
		s15_buf_str(out, "   Active boot partition unchanged.");
		break;
	case S15_OSW_EMMC_BAD_HEADER:
		s15_buf_str(out, "Partition ");
		s15_buf_dec(out, target);
		s15_buf_str(out, " may be active with a bad header");
		break;
	case S15_OSW_EMMC_BOTH_ACTIVE:
		/* S-03.062: BP1 decides (S-03.040) */
		if (target == 1u)
			s15_buf_str(out, "New OS boots; partition 2 may still be active");
		else
			s15_buf_str(out, "Partition 1 still active: old OS boots");
		break;
	case S15_OSW_EMMC_UNCONFIRMED:
		s15_buf_str(out, "Written; the eMMC did not confirm it");
		break;
	case S15_OSW_EMMC_UNTOUCHED:
	default:
		*style = S15_ST_TEXT;
		s15_buf_str(out, "   Nothing written.");
		break;
	}
}

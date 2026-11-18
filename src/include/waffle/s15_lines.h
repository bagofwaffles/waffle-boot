/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15 values and lines that do not depend on the menu's state: the digit
 * keys, progress values, the upgrade run's lines, pack names, the hand-over
 * frame, and the wording of the required-information lines.
 */
#ifndef WAFFLE_S15_LINES_H
#define WAFFLE_S15_LINES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s15_console.h"

/* Keys (S-07.030) the screens name. */
#define S15_KEY_HELP  0x00u
#define S15_KEY_WHITE 0x0du
#define S15_KEY_SHIFT 0x37u

/* S-15.011: the menu digit of a key, 0 when it is none. */
unsigned s15_digit(unsigned key);

/* Round(done * 100 / total), 100 for a total of 0. */
unsigned s15_progress(uint32_t done, uint32_t total);

/* The steps of S-11.057. */
enum s15_step {
	S15_STEP_CONTENT,
	S15_STEP_OS,
	S15_STEP_BOOTLOADER,
	S15_STEP_BLE,
	S15_STEP_BLE_BOOTLOADER,
	S15_STEP_KEYBOARD,
	S15_STEP_SPIBOOT,
	S15_STEP_COUNT
};
const char *s15_step_label(enum s15_step step);
unsigned s15_step_number(enum s15_step step);
bool s15_step_protected(enum s15_step step);

/* S-11.058 results. */
enum s15_result {
	S15_RES_OK,
	S15_RES_NOT_FOUND,
	S15_RES_ALREADY,
	S15_RES_SKIPPED,	/* a protected step, the option off (S-11.090) */
	S15_RES_FAILURE,
	S15_RES_BAD_FILE,
	S15_RES_INTERNAL,
	S15_RES_SKIPPED_UNCONFIRMED,	/* a protected step, not confirmed (S-11.096) */
	S15_RES_SKIPPED_OFF,	/* a switched-off step (S-11.073) */
	S15_RES_SKIPPED_WRITTEN	/* the bootloader, already written (S-11.068 item 5) */
};

/* A line under construction. */
#define S15_BUF 128u
struct s15_buf {
	char s[S15_BUF];
	size_t n;
};
void s15_buf_init(struct s15_buf *b);
void s15_buf_str(struct s15_buf *b, const char *s);
void s15_buf_strn(struct s15_buf *b, const char *s, size_t n);
void s15_buf_dec(struct s15_buf *b, uint32_t v);

/* What a line does to the console. */
enum s15_op {
	S15_OP_NONE,
	S15_OP_APPEND,
	S15_OP_REPLACE
};

/* A step's progress row (S15_ST_STEP_RUN); NONE for the first
 * stage. */
enum s15_op s15_step_progress_line(struct s15_buf *out, enum s15_step step,
				   unsigned p);
/* A step's result: its row (REPLACE), or the line
 * appended after it when its operation failed (APPEND), in *style.
 * version may be NULL. */
enum s15_op s15_result_line(struct s15_buf *out, enum s15_style *style,
			    enum s15_step step, enum s15_result result,
			    const char *version, bool content_only);
/* The longest cause a failed step's row has room for after "Failed: "
 * (S15_COLS - S15_STEP_COL - 8). */
#define S15_CAUSE_MAX 26u
_Static_assert(S15_STEP_COL + 8u + S15_CAUSE_MAX <= S15_COLS,
	       "a failed step's cause ends inside the row");
/* The row of a step whose operation failed (S15_ST_STEP_FAIL), with why
 * when cause is neither NULL nor "". */
void s15_failed_row(struct s15_buf *out, enum s15_step step,
		    const char *cause);

enum s15_run_kind {
	S15_RUN_FIRMWARE,
	S15_RUN_FORCED,
	S15_RUN_CONTENT       /* content-only; with a name if one is given */
};
void s15_header_line(struct s15_buf *out, enum s15_run_kind kind,
		     const char *version, const char *name);

/* The console's lines after a run starts, with their
 * styles: the header, the line for a return to the factory bootloader
 * when stock, the warning and the screen line. Returns the count (3 or 4). */
size_t s15_run_header(struct s15_buf out[4], enum s15_style style[4],
		      enum s15_run_kind kind, const char *version,
		      const char *name, bool stock);

enum s15_outcome {
	S15_OUT_DONE,
	S15_OUT_NOTHING,
	S15_OUT_FAILED
};
/* The parts a completed run skipped, by name: skipped has bit n set for
 * step n. One or two lines with their styles; 0 for no bit. */
size_t s15_not_updated(struct s15_buf out[2], enum s15_style style[2],
		       unsigned skipped);
/* One or two lines into out[], with their styles; returns the count.
 * cont: the Restart item follows, so a run with nothing to do does not
 * also ask for a restart. */
size_t s15_outcome_lines(const char *out[2], enum s15_style style[2],
			 enum s15_outcome outcome, bool any_skipped,
			 bool reason40, bool content_only, bool cont);

/* S-15.041: a pack's name from its manifest.json (data NULL: no file). */
size_t s15_manifest_name(const uint8_t *data, size_t len, char out[64]);
/* The label: the name, else the directory name; 63 at most. */
size_t s15_pack_label(const char *name, const char *dir, char out[64]);

/* S-15.132: the hand-over frame, 224 x 480 in S-13c.015's order, the logo
 * alone; its columns from S15_HANDOVER_VISIBLE on are not visible. */
#define S15_HANDOVER_COLS    224u
#define S15_HANDOVER_ROWS    480u
#define S15_HANDOVER_VISIBLE 222u
/* The step row at screen position (x, y). */
uint16_t s15_logo_pixel(unsigned x, unsigned y);
/* The frame's window pixel (c, r), as handover_frame.json fixes it. */
uint16_t s15_handover_pixel(unsigned c, unsigned r);
void s15_handover_frame(void (*put)(void *ctx, uint16_t pixel), void *ctx);

/* ---- S-15.163: a plain start that failed ----------------------------- */

/* Its step: what failed, in the order the boot tries them (S-05.005 -
 * S-05.007, S-05.021, S-05.024, S-05.032). */
enum s15_start_why {
	S15_START_OK,		/* no failed start: no notice */
	S15_START_NO_ACTIVE,	/* no boot partition active */
	S15_START_SELECT,	/* part's select */
	S15_START_HEADER_READ,	/* part's header read */
	S15_START_IMAGE_READ,	/* part's read at value, limit sectors */
	S15_START_USER_SELECT,	/* the user area's select after the load */
	S15_START_FORMAT,	/* value: the header's format */
	S15_START_ZERO_LENGTH,
	S15_START_TOO_LONG,	/* value: the length, limit: the most */
	S15_START_PAST_PARTITION,	/* value: sectors needed, limit: B's */
	S15_START_CRC,		/* value: the image's CRC, limit: the header's */
	S15_START_ENCRYPTED,	/* part: S-15.161's line */
	S15_START_REFUSED,	/* check: S-05.024's rule */
	S15_START_SLAVE_NO_OS,	/* value: the release's reply */
	S15_START_NO_REPLY,	/* the release got no reply */
	S15_START_SLAVE_START,	/* released; its OS did not start */
	S15_START_COUNT
};

/* S-05.024's rule that refused the image. */
enum s15_check {
	S15_CHECK_LENGTH,	/* longer than the staging area */
	S15_CHECK_PARSE,	/* a block header, or no final block */
	S15_CHECK_FLAGS,	/* a block flag not allowed */
	S15_CHECK_SPACE,	/* a destination outside L1, L2 and DDR */
	S15_CHECK_STAGING,
	S15_CHECK_RESERVE,	/* the hand-over reserve */
	S15_CHECK_RECORD,
	S15_CHECK_BOOT_RESERVE,	/* the ROM's boot reserve */
	S15_CHECK_COUNT
};

/* Where the bridge's exchange broke, as S09_AT_... numbers them
 * (waffle/s09_master.h), with S15_AT_IN_POLL set for a poll's. */
#define S15_AT_COUNT 10u
#define S15_AT_IN_POLL 0x80u

struct s15_start {
	enum s15_start_why why;
	unsigned part;		/* a boot partition (1, 2), 0 the user area */
	uint32_t value, limit;	/* as why names them */
	enum s15_check check;	/* S15_START_REFUSED */
	bool lost;		/* the slave stopped answering on it */
	uint8_t cmd, at;	/* then the request and where it broke */
};

/* The notice's first line (S15_ST_ERROR). */
extern const char s15_start_failed[];
/* Its second: the reason, for s15_reason_line(); not for an encrypted
 * image, whose second line is s15_os_reinstall. */
void s15_start_reason(struct s15_buf *out, const struct s15_start *s);
/* Device Info's OS start value: the step, and for a lost slave the request
 * and where it broke. At most S15_START_INFO_MAX characters. */
#define S15_START_INFO_MAX 42u
void s15_start_info(struct s15_buf *out, const struct s15_start *s);

/* ---- required information (wording free; S-15 terms) -------------- */

#define S15_OPTION_PROMPT_LINES 8u
extern const char *const s15_option_prompt[S15_OPTION_PROMPT_LINES];
extern const enum s15_style s15_option_prompt_style[S15_OPTION_PROMPT_LINES];
extern const char s15_option_item_on[];
extern const char s15_option_item_off[];
extern const char s15_slave_no_os[];
extern const char s15_slave_no_start[];
extern const char s15_slave_absent[];
extern const char s15_slave_lost[];	/* it stopped answering after the start */
extern const char s15_drive_lost[];	/* S15_ST_WARN: the drive with it */
extern const char s15_reset_lost[];	/* S15_ST_ERROR: a reset that could not run */
extern const char s15_checking_files[];	/* S15_ST_DIM: the scan before a write (S-15.191) */
/* S15_ST_WARN under Settings: why a change lasts for this start only
 * (S-15.172, S-15.193) */
extern const char s15_settings_host[];
extern const char s15_settings_this_start[];
extern const char s15_settings_failed[];
extern const char s15_settings_unusable[];
extern const char s15_reset_settings_failed[];	/* S15_ST_WARN: the clear of S-02.105 */
/* S-15.192: Device Info 2's Files value for a way of enum s15_files. */
void s15_files_text(struct s15_buf *out, unsigned way, uint32_t objects);
extern const char s15_os_reinstall[];         /* exact text, S-15.161 */
#define S15_INTERRUPTED_LINES 2u
extern const char *const s15_interrupted[S15_INTERRUPTED_LINES];  /* S-11.080 */
extern const char s15_restarting[];
extern const char s15_restart_item[];	/* S15_ST_ITEM, digit 6 */
extern const char s15_format_unreadable[];
extern const char s15_reset_unreadable[];
extern const char s15_reset_incomplete[];
extern const char s15_os_ldr_unreadable[];
extern const char s15_os_write_failed[];
extern const char s15_reset_needed[];
extern const char s15_nothing_skipped[];
extern const char s15_fs_warning[];
extern const char s15_stock_line[];
/* S-15.015: the line under items chosen by a hold (S15_ST_DIM). */
extern const char s15_hold_line[];
/* S-15.015: a hold's item hint ("Hold 7 for 5 s"), and its last row
 * (S15_ST_STEP_RUN): the bar at fill percent, then the seconds left. */
void s15_hold_hint(struct s15_buf *out, unsigned digit);
void s15_hold_bar(struct s15_buf *out, unsigned fill, unsigned left_s);

void s15_reason_line(struct s15_buf *out, const char *reason);
void s15_pack_failed_line(struct s15_buf *out, const char *label);

/* ---- S-15.094: the OS write's screen -------------------------------- */

/* The steps, a row each. */
enum s15_osw_step {
	S15_OSW_STEP_READ,	/* /yaffs2/os.ldr read whole */
	S15_OSW_STEP_CHECK,	/* S-03.051 */
	S15_OSW_STEP_TARGET,	/* S-03.052 */
	S15_OSW_STEP_ERASE,	/* S-03.055 step 1 */
	S15_OSW_STEP_WRITE,	/* step 2 */
	S15_OSW_STEP_VERIFY,	/* step 3 */
	S15_OSW_STEP_ACTIVATE,	/* steps 4 to 8 */
	S15_OSW_STEP_COUNT
};

/* Why a write stopped: its reason line. */
enum s15_osw_why {
	S15_OSW_WHY_NONE,
	S15_OSW_WHY_REFUSED,	/* the volume is refused (S-12.052) */
	S15_OSW_WHY_MISSING,	/* no /yaffs2/os.ldr */
	S15_OSW_WHY_DIRECTORY,
	S15_OSW_WHY_READ,	/* value: the bridge's errno (S-09.042) */
	S15_OSW_WHY_MEMORY,	/* no buffer for the image */
	S15_OSW_WHY_TOO_SMALL,	/* S-03.051: value the length, limit the least */
	S15_OSW_WHY_TOO_LARGE,	/* value the length, limit the most */
	S15_OSW_WHY_NO_ROOM,	/* value the sectors needed, limit B's (0: unknown) */
	S15_OSW_WHY_SELECT,	/* part */
	S15_OSW_WHY_EMMC_READ,	/* part, value the sector */
	S15_OSW_WHY_EMMC_WRITE,	/* part, value the sector */
	S15_OSW_WHY_ERASE,	/* part, value the erase group */
	S15_OSW_WHY_MISMATCH,	/* part, value the sector read back wrong */
	S15_OSW_WHY_DISABLED,	/* the test OS (waffle/testos.h) writes nothing */
	S15_OSW_WHY_FLUSH,	/* S-03.068 */
	S15_OSW_WHY_READS_DIFFER,	/* the file read differently twice */
	S15_OSW_WHY_NOT_OS	/* S-05.024 would refuse the image */
};

/* The boot partitions after a failed write (S-03.056 - S-03.062, S-03.068). */
enum s15_osw_emmc {
	S15_OSW_EMMC_UNTOUCHED,		/* nothing written (S-03.051, S-03.061) */
	S15_OSW_EMMC_UNCHANGED,		/* the active partition as before (S-03.056, S-03.057) */
	S15_OSW_EMMC_BAD_HEADER,	/* S-03.057's recovery write failed */
	S15_OSW_EMMC_BOTH_ACTIVE,	/* S-03.062 */
	S15_OSW_EMMC_UNCONFIRMED	/* S-03.068's third flush failed */
};

const char *s15_osw_label(enum s15_osw_step step);
/* A step's rows: running with its progress (S15_ST_STEP_RUN), done
 * (S15_ST_STEP_OK; a and b as the step's status names them: the length;
 * the sectors needed and B's; the target) and failed (S15_ST_STEP_FAIL). */
void s15_osw_progress_row(struct s15_buf *out, enum s15_osw_step step, unsigned p);
void s15_osw_done_row(struct s15_buf *out, enum s15_osw_step step, uint32_t a, uint32_t b);
void s15_osw_failed_row(struct s15_buf *out, enum s15_osw_step step);
/* The reason, for s15_reason_line(); value and limit as why names them. */
void s15_osw_reason(struct s15_buf *out, enum s15_osw_why why, unsigned part,
		    uint32_t value, uint32_t limit);
/* The line that says what a failed write left, in *style. */
void s15_osw_emmc_line(struct s15_buf *out, enum s15_style *style,
		       enum s15_osw_emmc emmc, unsigned target);
extern const char s15_osw_written[];
extern const char s15_osw_select0_failed[];	/* S15_ST_WARN, S-03.063 */
extern const char s15_osw_back[];

/* ---- S-15.190, S-05.033: continue to the OS, at a host's request ---- */

extern const char s15_cont_title[];		/* the restarting view's title */
extern const char s15_cont_restarting[];	/* S15_ST_WARN */

#endif /* WAFFLE_S15_LINES_H */

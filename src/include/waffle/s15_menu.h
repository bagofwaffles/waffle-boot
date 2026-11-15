/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15 held mode: the screens and what keys, host events and upgrade
 * reports do to them.
 *
 * Key events come in as data (S-07.031, decoded by the PSoC link). What is
 * drawn goes to the console (s15_console.h) and out through
 * s15_menu_render(); what is sent or written goes out through
 * s15_env_ops. All calls run on one thread; an env operation may take
 * time (a format, an OS write) and returns when it is done.
 */
#ifndef WAFFLE_S15_MENU_H
#define WAFFLE_S15_MENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s14_log.h"
#include "waffle/s15_confirm.h"
#include "waffle/s15_console.h"
#include "waffle/s15_keytest.h"
#include "waffle/s15_lines.h"
#include "waffle/s15_settings.h"

/* The screens of held mode. */
enum s15_screen {
	S15_SCR_MAIN,
	S15_SCR_PACKS,
	S15_SCR_INFO,
	S15_SCR_SYSTEM,
	S15_SCR_SETTINGS,
	S15_SCR_FORMAT,
	S15_SCR_FACTORY,
	S15_SCR_UPGRADE,
	S15_SCR_RESULT,     /* an erasing action's result: digit 8 alone acts */
	S15_SCR_STARTING,   /* S-15.190: restarting for the OS; no key acts */
	S15_SCR_NONE        /* before held mode's first screen */
};

/* S-15.015: what a hold runs. The item's own key down starts the hold;
 * held for S15_CONFIRM_HOLD_MS it runs the action once; its early release,
 * or any other key down, only cancels. */
enum s15_arm {
	S15_ARM_NONE,
	S15_ARM_RESET,      /* the factory reset (S-15.050) */
	S15_ARM_FORMAT,     /* the format (S-15.071) */
	S15_ARM_PACK,       /* removing the pack `pending` (S-15.042) */
	S15_ARM_OPTION      /* turning the option on (S-15.140) */
};

/* S-09: the slave's state, for Device Info. */
enum s15_slave {
	S15_SLAVE_OK,
	S15_SLAVE_NO_OS,
	S15_SLAVE_ABSENT,
	S15_SLAVE_NO_START,	/* S-05.032: released, its OS did not start */
	S15_SLAVE_LOST		/* present at the start, then stopped answering */
};

/* S-10a.010 descriptor sets. */
enum s15_usb_set {
	S15_USB_MAIN,          /* the main screen's set */
	S15_USB_DISK,          /* the upgrade screen's set: the RAM disk */
	S15_USB_MTP_VENDOR,
	S15_USB_MTP,
	S15_USB_SERIAL
};

/* S-15.094: the result of the factory menu's OS write. */
enum s15_os_write {
	S15_OSW_OK,
	S15_OSW_UNREADABLE,    /* /os.ldr missing or not read whole */
	S15_OSW_FAILED         /* S-03's write reported failure */
};

/* S-15.094: how the OS write ended. */
struct s15_osw_end {
	enum s15_os_write result;
	enum s15_osw_why why;      /* S15_OSW_WHY_NONE after a success */
	enum s15_osw_emmc emmc;    /* after a failure */
	unsigned part;             /* a failed eMMC operation's partition */
	unsigned target;           /* S-03.052's target, 0 before it is chosen */
	uint32_t value, limit;     /* as why names them */
	bool select0_failed;       /* S-03.063 */
};

/* What the OS write reports while it runs (S-15.094): a step starts, its
 * progress, its success (a and b: s15_osw_done_row()'s). */
struct s15_osw_report {
	void *ctx;
	void (*step)(void *ctx, enum s15_osw_step step);
	void (*progress)(void *ctx, uint32_t done, uint32_t total);
	void (*done)(void *ctx, uint32_t a, uint32_t b);
};

/* What this boot knows when held mode starts. */
struct s15_boot {
	const char *version;       /* S-17.032 version text */
	unsigned reason;           /* S-04.006 reason taken */
	struct s15_settings settings; /* S-15.172: the settings file's */
	enum s15_slave slave;
	bool running_known;        /* S-01.002: the running slot is established */
	unsigned running_slot;     /* its index then */
	struct s15_start start;    /* S-15.163: a failed plain start; S-15.161
				    * when its image is marked encrypted */
	bool interrupted;          /* S-11.080: the last install did not finish */
	bool start_menu;           /* S-15.175: the boot store's setting as read at this start */
	/* S-17.030 */
	const char *unit_serial, *chip_serial, *psoc_serial;
	const char *os_version, *sw_version, *ble_version, *psoc_version;
	bool provisioned;
};

/* S-15.175: how a write of the boot store ended. */
enum s15_store_save {
	S15_STORE_SAVED,
	S15_STORE_FAILED,	/* an erase, program or the read-back failed */
	S15_STORE_UNUSABLE	/* Slot 2's image reaches the store's sector */
};

#define S15_PACKS_MAX 7u
#define S15_NAME_MAX  255u

struct s15_env_ops {
	/* Start a USB descriptor set (S-10a.010), also when presented. */
	void (*usb_set)(void *ctx, enum s15_usb_set set);
	/* Send 0x48 {01} to the PSoC (S-07.078). */
	void (*volcal_reset)(void *ctx);
	/* The LED messages of S-07.045 for the key test's start or end; the
	 * end's end with 0x40 {01}. */
	void (*keytest_leds)(void *ctx, bool start);
	/* The yaffs2 volume is refused (big-endian, S-12.052). */
	bool (*volume_refused)(void *ctx);
	/* Whether a yaffs2 directory exists. */
	bool (*dir_exists)(void *ctx, const char *path);
	/* Subdirectory `index` of /yaffs2/content/dlc in directory order,
	 * files skipped, "." and ".." skipped (S-16.082); its
	 * name copied to name. False past the last. */
	bool (*pack_dir)(void *ctx, size_t index, char name[S15_NAME_MAX + 1]);
	/* Up to max bytes from the start of
	 * /yaffs2/content/dlc/<dir>/manifest.json; 0 if missing (S-15.041). */
	size_t (*read_manifest)(void *ctx, const char *dir, uint8_t *buf,
				size_t max);
	/* Remove a yaffs2 path with everything in it. True on success or
	 * when it does not exist. */
	bool (*remove_tree)(void *ctx, const char *path);
	/* Erase the first erase group of the OS's FAT volume (S-03.073). */
	void (*erase_os_fat)(void *ctx);
	/* Format the yaffs2 volume (S-12.057) and mount it again; false for
	 * a failed format (S-15.072). volume_refused() then gives the new
	 * mount's result. */
	bool (*format)(void *ctx);
	/* Read /yaffs2/os.ldr and write it with S-03's OS write, reporting
	 * the steps with s15_osw_step() and the others as they go; *end
	 * comes in as a failure that names nothing (S15_OSW_FAILED). */
	void (*os_write)(void *ctx, struct s15_osw_end *end);
	/* The shutdown of S-04.063 (with its black frame) and the
	 * hard reset of S-04.020 W7. Does not return on the device. */
	void (*reset_w7)(void *ctx);
	/* Write the system information to the disk (S-12.017), with the log
	 * export (S-14.051, s14_export_run()). */
	void (*system_info)(void *ctx);
	/* Start the scan of S-11.045 now. */
	void (*scan_now)(void *ctx);
	/* The session upgrade option turned on or off (S-02.055). */
	void (*option)(void *ctx, bool on);
	/* S-15.172: write the settings file for these settings. False when
	 * the drive could not be written. */
	bool (*settings_save)(void *ctx, const struct s15_settings *s);
	void *ctx;
	/* The slave stopped answering (S-09.008) for the rest of the boot: no
	 * request reaches the drive, refused or not. NULL: it never does. */
	bool (*slave_lost)(void *ctx);
	/* S-15.190, S-05.033: restart so that the next start boots the OS,
	 * the restarting view drawn. Does not return on the device. NULL:
	 * there is no such action. */
	void (*continue_os)(void *ctx);
	/* S-15.175: write the boot store's setting; *now is what the store
	 * reads afterwards, also after a failed or refused write. NULL: there
	 * is no store, and no item. */
	enum s15_store_save (*boot_store_save)(void *ctx, bool start_menu, bool *now);
};

/* What the upgrade screen is doing (S-15.103). */
enum s15_activity {
	S15_ACT_IDLE,
	S15_ACT_SCAN,
	S15_ACT_TRANSFER,
	S15_ACT_RUN
};

/* The preparation steps with a progress line. */
enum s15_phase {
	S15_PH_DECRYPT,
	S15_PH_EXTRACT_FIRMWARE,
	S15_PH_EXTRACT_CONTENT,
	S15_PH_TRANSFER
};

enum s15_mtp {
	S15_MTP_OFF,
	S15_MTP_VENDOR,
	S15_MTP_STANDARD
};

/* S-15.094: the OS write's screen on the developer menu. */
enum s15_osw_view {
	S15_OSW_VIEW_NONE,
	S15_OSW_VIEW_RUNNING,
	S15_OSW_VIEW_DONE      /* digit 8 acts, and digit 6 after a success */
};

struct s15_menu {
	struct s15_console con;
	const struct s15_boot *boot;
	const struct s15_env_ops *env;
	enum s15_screen screen;
	const char *title;        /* The title bar's name */
	enum s15_usb_set usb;     /* the set last started */

	/* keys: S-15.010 - S-15.013 */
	uint8_t down[(S15_NKEYS + 7u) / 8u];
	uint8_t stale[(S15_NKEYS + 7u) / 8u];
	bool shift;

	/* the option: S-15.140 */
	bool option_on;

	/* Settings: S-15.170, S-15.173 */
	bool dev_mode;             /* S-15.172: the menu, the serial port */
	bool color;
	bool mtp_macos;            /* S-15.174 */
	bool start_menu;           /* S-15.175, the boot store's */

	/* system menu */
	bool packs_item;

	/* S-15.015: the hold under way */
	enum s15_arm armed;
	unsigned arm_key;          /* the key held */
	uint32_t arm_ms;           /* when it went down */
	unsigned arm_fill;         /* the bar as drawn: percent */
	unsigned arm_left;         /* and seconds left */
	uint32_t now_ms;           /* the time s15_menu_tick() last gave */

	/* pack list */
	unsigned npacks;
	unsigned pending;    /* the held pack, 1 to 7 */
	char dirs[S15_PACKS_MAX][S15_NAME_MAX + 1u];
	char labels[S15_PACKS_MAX][64];

	/* format menu */
	bool refused;

	/* developer menu */
	enum s15_mtp mtp;
	bool busy;
	/* Key downs have no effect from a long call's start until the board
	 * has read what arrived during it (s15_menu_hold()). */
	bool hold;
	enum s15_osw_view osw;
	bool osw_written;          /* S-15.094: the last OS write succeeded */
	int osw_step;        /* the OS write's step whose row is open, or -1 */
	unsigned osw_p;      /* its last progress value */

	/* digit 6 continues to the OS after a completed run (S-11.060) */
	bool restart_ok;

	/* key test */
	bool keytest;
	struct s15_keytest kt;
	bool black_once;
	uint32_t changes;

	/* upgrade screen */
	bool midi;
	enum s15_activity activity;
	bool final;          /* a zip was read in this boot (S-15.103, S-15.115) */
	bool begun;          /* a host transfer has begun in this boot (S-15.103) */
	bool from_disk;      /* the last package was found by a scan */
	bool disk_read;      /* a zip was read from the RAM disk (S-15.115) */
	bool scan_due_set;
	uint32_t scan_due_ms;
	bool content_only;
	int prog_key;        /* last progress line: phase or step, -1 none */
	int row_step;        /* the step whose row is the last line, or -1 */
	unsigned prog_p;
	uint32_t transfer_total;
};

void s15_menu_init(struct s15_menu *m, const struct s15_boot *boot,
		   const struct s15_env_ops *env, struct s14_log *log);
/* Held mode starts: open the first screen (S-15.020). */
void s15_menu_start(struct s15_menu *m);
/* A key event (S-07.031), before or after the start. */
void s15_menu_key(struct s15_menu *m, unsigned key, bool down);
/*
 * A long call of the environment (the OS write, a format, a removal, the
 * FAT erase) holds the keys: key downs during it, and those the board reads
 * after it from what arrived meanwhile, have no effect. The board starts
 * the hold, reads all of that, then releases it.
 */
void s15_menu_hold(struct s15_menu *m);
bool s15_menu_held(const struct s15_menu *m);
void s15_menu_release(struct s15_menu *m);
/* s15_menu_key() with that hold: held mode's key events come here. */
void s15_menu_event(struct s15_menu *m, unsigned key, bool down);
/*
 * The time passes (ms since any fixed point; only differences matter): a
 * hold's bar follows it, and a hold of S15_CONFIRM_HOLD_MS runs its action.
 * The board gives the time before each key event and on every pass, so a
 * key event is timed by the last call.
 */
void s15_menu_tick(struct s15_menu *m, uint32_t now_ms);
/* Draw a frame of held mode (S-15.002, S-15.062); also the
 * fatal-error frame. */
void s15_menu_render(struct s15_menu *m, const struct s15_geometry *g,
		     const struct s15_draw_ops *d);
/* What the title bar and the status bar show now. */
void s15_menu_frame(const struct s15_menu *m, struct s15_frame *f);
/* A counter that changes whenever the frame's content changes. */
uint32_t s15_menu_changes(const struct s15_menu *m);

/* ---- S-15.190, S-05.033: continue to the OS, at a host's request -- */

enum s15_continue {
	S15_CONT_STARTED,	/* the restarting view drawn, the board's restart run */
	S15_CONT_BUSY		/* a scan, host transfer, run or OS write is under way */
};

/* Whether continuing would start now, without acting. */
enum s15_continue s15_menu_continue_check(const struct s15_menu *m);
/* The host command (S-10c.076) comes here, and digit 6 after an install or
 * an OS write (S-15.190): busy changes nothing; otherwise the restarting
 * view, a key test ended and a hold canceled (S-15.015), then
 * env->continue_os(). */
enum s15_continue s15_menu_continue(struct s15_menu *m);

/* ---- the OS write's reports (S-15.094), while env->os_write() runs -- */

/* A step starts: its row. Nothing happens outside the OS write. */
void s15_osw_step(struct s15_menu *m, enum s15_osw_step step);
/* The open step's progress. */
void s15_osw_progress(struct s15_menu *m, uint32_t done, uint32_t total);
/* The open step succeeded; a and b as s15_osw_done_row() names them. */
void s15_osw_done(struct s15_menu *m, uint32_t a, uint32_t b);

/* ---- the upgrade screen's events ----------------------------------- */

/* The host acquired the disk. */
void s15_up_acquired(struct s15_menu *m);
/* S-15.105: the host released the disk at now_ms (since the start). */
void s15_up_released(struct s15_menu *m, uint32_t now_ms);
/* S-15.105: the time passes; starts a due scan. */
void s15_up_tick(struct s15_menu *m, uint32_t now_ms);
/* S-15.104: a host transfer's begin, before its product word is
 * checked; len the package length (0 when it has none). It ends the scans
 * for the rest of the boot (S-15.103). */
void s15_up_transfer_begin(struct s15_menu *m, uint32_t len);
/* The begin started its transfer: the Transfer row, when the
 * begin's length is not 0. */
void s15_up_transfer_started(struct s15_menu *m);
/* The begin's product word passed but its transfer could not
 * start (S-10c.071, S-10c.088): one error line, no reason line. */
void s15_up_no_transfer(struct s15_menu *m);
/* Chunk number n (from 1) has arrived, done bytes of the
 * package; last for the final chunk. */
void s15_up_transfer_chunk(struct s15_menu *m, uint32_t n, uint32_t done,
			   bool last);
/* The scan found no package (S-11.046). */
void s15_up_not_found(struct s15_menu *m);
/* A package refused (S-11.020 checks 1 - 15, or a begin's
 * product word), with its reason's name. */
void s15_up_refused(struct s15_menu *m, const char *reason);
/* A phase starts; its progress. */
void s15_up_phase(struct s15_menu *m, enum s15_phase phase);
void s15_up_progress(struct s15_menu *m, uint32_t done, uint32_t total);
/* The extraction refused (S-11.020 checks 16 - 17). */
void s15_up_extract_refused(struct s15_menu *m, const char *reason,
			    bool content_only);
/* S-15.116: a protected component's confirmation is shown, before the
 * run; then its bar and the seconds left as they change. The keys are
 * s15_confirm's, not the menus'. */
void s15_up_confirm(struct s15_menu *m, const struct s15_confirm_info *info);
void s15_up_confirm_bar(struct s15_menu *m, unsigned fill, unsigned left_s);
/* A run starts. */
void s15_up_run_start(struct s15_menu *m, enum s15_run_kind kind,
		      const char *version, const char *name, bool stock);
void s15_up_step_start(struct s15_menu *m, enum s15_step step,
		       bool first_stage_warning);
void s15_up_step_progress(struct s15_menu *m, enum s15_step step,
			  uint32_t done, uint32_t total);
/* Refused_start: S-02.090 refused the bootloader step's start. Cause:
 * why a step that stopped the run failed, NULL or "" when the result
 * says it all. */
void s15_up_step_result(struct s15_menu *m, enum s15_step step,
			enum s15_result result, const char *version,
			bool refused_start, const char *cause);
/* S-15.115; skipped has bit n set for each step n that reported Skipped:
 * a completed run names them. */
void s15_up_run_end(struct s15_menu *m, enum s15_outcome outcome,
		    unsigned skipped);
/*
 * S-15.115, S-11.049: false once a run (or a refusal at checks 16 - 17)
 * has read a zip from the RAM disk in this boot; the disk is then not
 * offered to the host again. A package that came over a host link leaves
 * it true: the disk is offered again when the transfer ends (S-10c.073,
 * S-10c.090), though no scan follows (S-15.103).
 */
bool s15_up_disk_offerable(const struct s15_menu *m);

#endif /* WAFFLE_S15_MENU_H */

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Renders held-mode screens (S-15) and the hand-over frame (S-15.132)
 * with the bootloader's own drawing code into raw frame buffers, one file
 * per screen, for tools/ui-shots.sh to turn into the pictures of docs/ui/.
 * Host only; not part of the image.
 *
 *     ui-shots <directory>
 *
 * Each <directory>/<name>.raw is the frame window of S-13c.014 in
 * S-13c.015's byte order (224 columns by 480 rows, RGB565 high byte
 * first), as the panel would receive it.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The installer's screens (src/testos/installer_logic.c), its pure part. */
#define OPXY_TESTOS_CHECK 1
#define OPXY_TESTOS_INSTALLER 1
#include "../src/testos/check_logic.c"
#include "../src/testos/installer_logic.c"

#include "front/fr_draw.h"
#include "front/fr_start.h"
#include "waffle/display.h"
#include "waffle/s15_menu.h"

/* This tree's version, from tools/ui-shots.sh: the pictures show it, slot 2
 * one patch older and an offered upgrade one patch newer. */
#if !defined(SHOT_MAJOR) || !defined(SHOT_MINOR) || !defined(SHOT_PATCH)
#error "build with tools/ui-shots.sh, which gives the version"
#endif
#if SHOT_PATCH < 1
#error "the samples need a patch of at least 1"
#endif
static char ver[24], ver_next[24];

static uint8_t fb[S13C_FRAME_BYTES];
static struct wb_display disp;
static struct s15_boot boot;
static struct s15_menu menu;
static unsigned npacks;
static const char *const packs[] = {
	"Bass Essentials", "Field Recordings Vol 2", "drums-808", "Tape Loops",
	"Choir Pads", "Modular Bleeps", "Granular Textures",
};

static void e_usb(void *c, enum s15_usb_set s) { (void)c; (void)s; }
static void e_none(void *c) { (void)c; }
static void e_leds(void *c, bool start) { (void)c; (void)start; }
static bool e_refused(void *c) { (void)c; return false; }

static bool e_dir(void *c, const char *path)
{
	(void)c;
	return npacks != 0u && strcmp(path, "/yaffs2/content/dlc") == 0;
}

static bool e_pack(void *c, size_t i, char name[S15_NAME_MAX + 1])
{
	(void)c;
	if (i >= npacks)
		return false;
	snprintf(name, S15_NAME_MAX + 1, "%s", packs[i]);
	return true;
}

static size_t e_manifest(void *c, const char *dir, uint8_t *buf, size_t max)
{
	(void)c; (void)dir; (void)buf; (void)max;
	return 0;
}

static bool e_remove(void *c, const char *path) { (void)c; (void)path; return true; }
static bool e_format(void *c) { (void)c; return true; }
static void e_osw(void *c, struct s15_osw_end *end);
static void e_option(void *c, bool on) { (void)c; (void)on; }
static bool lost;	/* the slave stopped answering */
static bool e_lost(void *c) { (void)c; return lost; }
/* S-15.190: the board's sequence would leave held mode; the starting
 * view stays. */
static void e_continue(void *c) { (void)c; }

static enum s15_store_save e_settings(void *c, const struct s15_settings *s,
				      struct s15_settings *now)
{
	(void)c;
	*now = *s;
	return S15_STORE_SAVED;
}

static enum s15_store_save e_clear(void *c)
{
	(void)c;
	return S15_STORE_SAVED;
}

static const struct s15_env_ops env = {
	e_usb, e_none, e_leds, e_refused, e_dir, e_pack, e_manifest,
	e_remove, e_none, e_format, e_osw, e_none, e_none, e_none, e_option,
	e_settings, NULL, e_lost, e_continue, NULL, NULL, e_clear,
};

/* S-15.163: the image read lost the slave, as on a unit. */
static void start_lost(void)
{
	boot.start.why = S15_START_IMAGE_READ;
	boot.start.part = 1;
	boot.start.value = 1281;
	boot.start.limit = 128;
	boot.start.lost = true;
	boot.start.cmd = 0x12;
	boot.start.at = 4;	/* S09_AT_REPLY_BROKE */
	lost = true;
}

static void setup(void)
{
	memset(&boot, 0, sizeof boot);
	boot.version = ver;
	boot.reason = 10;
	boot.unit_serial = "XY-0000000";
	boot.chip_serial = "0000-0000-00";
	boot.psoc_serial = "00000000";
	boot.os_version = "1.1.4";
	boot.sw_version = "1.1.4";
	boot.ble_version = "0.9.2";
	boot.psoc_version = "0.4.1";
	boot.provisioned = true;
	boot.start_log = S15_LOG_OK;
	boot.start_log_slot = 1;	/* the running slot, selected */
	boot.start_log_used = 41;
	boot.start_log_erase = 256;
	s15_settings_defaults(&boot.settings);
	boot.settings.color = false;	/* the shots are in gray (S-15.173), but settings-color */
	boot.settings.dev_mode = true;	/* the shots show the developer menu */
	boot.running_known = true;	/* the status bar names slot 2 */
	boot.running_slot = 1;
	npacks = 0;
	lost = false;
}

static uint32_t now_ms;		/* the menu's clock (S-15.015) */

static void start(void)
{
	s15_menu_init(&menu, &boot, &env, NULL);
	now_ms = 0;
	s15_menu_tick(&menu, now_ms);
	s15_menu_start(&menu);
}

static void press(unsigned key)
{
	s15_menu_key(&menu, key, true);
	s15_menu_key(&menu, key, false);
}

/* S-15.015: key down, held for ms and not released. */
static void hold_for(unsigned key, uint32_t ms)
{
	s15_menu_key(&menu, key, true);
	now_ms += ms;
	s15_menu_tick(&menu, now_ms);
}

/* Held for the whole time, then released: the hold's action. */
static void hold(unsigned key)
{
	hold_for(key, S15_CONFIRM_HOLD_MS);
	s15_menu_key(&menu, key, false);
}

static int write_raw(const char *dir, const char *name)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof path, "%s/%s.raw", dir, name);
	f = fopen(path, "wb");
	if (f == NULL || fwrite(fb, 1, sizeof fb, f) != sizeof fb) {
		fprintf(stderr, "ui-shots: cannot write %s\n", path);
		if (f != NULL)
			fclose(f);
		return 1;
	}
	return fclose(f) != 0;
}

static int shot(const char *dir, const char *name)
{
	s15_menu_render(&menu, &wb_display_geometry, wb_display_draw(&disp));
	return write_raw(dir, name);
}

/* S-15.094: the OS write e_osw() plays, and where its shot goes. */
static enum { OSW_RUN, OSW_DONE, OSW_FAILED } osw_how;
static const char *osw_dir;
static int osw_bad;

/* A 1843200-byte image to boot partition 2 of 4 MiB; the run's shot is
 * taken at the Write step's 45%, the failure is a sector read back wrong. */
static void e_osw(void *c, struct s15_osw_end *end)
{
	static const uint32_t a[S15_OSW_STEP_COUNT] = { 1843200u, 3601u, 2u, 0, 0, 0, 2u };
	static const uint32_t b[S15_OSW_STEP_COUNT] = { 0, 8192u, 0, 0, 0, 0, 0 };

	(void)c;
	for (unsigned s = 0; s < S15_OSW_STEP_COUNT; s++) {
		s15_osw_step(&menu, (enum s15_osw_step)s);
		if (osw_how == OSW_RUN && s == S15_OSW_STEP_WRITE) {
			s15_osw_progress(&menu, 45, 100);
			osw_bad |= shot(osw_dir, "oswrite-run");
		}
		if (osw_how == OSW_FAILED && s == S15_OSW_STEP_VERIFY) {
			end->result = S15_OSW_FAILED;
			end->why = S15_OSW_WHY_MISMATCH;
			end->emmc = S15_OSW_EMMC_UNCHANGED;
			end->part = end->target = 2u;
			end->value = 3600u;
			return;
		}
		s15_osw_done(&menu, a[s], b[s]);
	}
	end->result = S15_OSW_OK;
	end->target = 2u;
}

/* Recovery's own composer (src/front/fr_recscreen.c) over a sample unit:
 * slot 1 running and the boot slot with this version, slot 2 checked with
 * the patch before it, both with the running front, USB connected. ui
 * NULL is the main screen; p the protocol's state. */
static struct fr_rec_status rec_st;
static struct fr_proto rec_p;

static void recovery_unit(void)
{
	static const uint16_t v[2][4] = { { SHOT_MAJOR, SHOT_MINOR, SHOT_PATCH, 0 },
					   { SHOT_MAJOR, SHOT_MINOR, SHOT_PATCH - 1, 0 } };

	memset(&rec_st, 0, sizeof rec_st);
	memset(&rec_p, 0, sizeof rec_p);
	rec_st.why = FR_WHY_KEYS;
	rec_st.running.established = true;
	rec_st.running.slot = 0;
	rec_st.selector_ok = true;
	rec_st.sel.index = 0;
	rec_st.sel.verified = 1;
	for (unsigned k = 0; k < 2u; k++) {
		rec_st.slot[k].good = true;
		rec_st.slot[k].has_desc = true;
		rec_st.slot[k].has_front = true;
		rec_st.slot[k].bootable = true;
		rec_st.slot[k].kind = FR_KIND_WB;
		memcpy(rec_st.slot[k].desc.bl_version, v[k], sizeof v[k]);
	}
	rec_st.own_desc = true;
	rec_st.desc = rec_st.slot[0].desc;
	fr_rec_target(&rec_st);
	rec_p.st = &rec_st;
	rec_p.phase = FR_PH_IDLE;
	rec_p.w.target = FR_NO_SLOT;
}

static void recovery_shot(struct fr_screen *s, const struct fr_ui *ui)
{
	fr_rec_screen(s, &rec_st, &rec_p, true, ui);
}

/* A TE slot's version, for the slot line; no TE bytes are used. */
static const uint16_t te[4] = { 1000, 0, 20, 0 };

/* The logo line over fb, as src/front/fr_start.c's loading_line() paints
 * it under the logo frame (S-13c.025). */
static void loading_line(void)
{
	for (unsigned x = 0; x < WB_DISPLAY_W; x++)
		for (unsigned dy = 0; dy < 16u; dy++) {
			uint16_t p = fr_line_pixel("Loading recovery menu...", x, dy);
			uint8_t *q = &fb[2u * ((size_t)(WB_DISPLAY_W - 1u - x) * S13C_FRAME_W +
					       FR_LINE_Y + dy)];

			q[0] = (uint8_t)(p >> 8);
			q[1] = (uint8_t)p;
		}
}

/* A front screen into fb, a pixel at a time (S-13c.023). */
static void front_shot(const struct fr_screen *s)
{
	memset(fb, 0, sizeof fb);
	for (unsigned x = 0; x < WB_DISPLAY_W; x++)
		for (unsigned y = 0; y < WB_DISPLAY_H; y++) {
			uint16_t p = fr_screen_pixel(s, x, y);
			uint8_t *q = &fb[2u * ((size_t)(WB_DISPLAY_W - 1u - x) * S13C_FRAME_W + y)];

			q[0] = (uint8_t)(p >> 8);
			q[1] = (uint8_t)p;
		}
}

/* The installer: the checks under way, the key check with com pressed, the
 * trial offered with its key held, the trial written, the commit offered,
 * done, and a locked install. */
static int installer_shots(const char *dir)
{
	const struct s15_frame fr = { "Installer", ver, 0, S15_SLOT_UNKNOWN,
				      false, false, true };
	static const struct {
		const char *name;
		uint8_t phase, lock, trial;
		unsigned hold_digit, hold_fill, hold_left_s;
	} shots[] = {
		{ "installer-keys", INST_GUIDED, 0, 0, 0, 0, 0 },
		{ "installer-ready", INST_READY, 0, 0, 1u, 600u, 2u },
		{ "installer-trial", INST_TRIAL_WRITTEN, 0, INST_TRIAL_WAITING, 0, 0, 0 },
		{ "installer-commit", INST_COMMIT_READY, 0, INST_TRIAL_REACHED, 0, 0, 0 },
		{ "installer-done", INST_DONE, 0, 0, 0, 0, 0 },
		{ "installer-locked", INST_LOCKED, INST_LOCK_FAILED, 0, 0, 0, 0 },
	};
	struct s15_console con;
	struct tchk_line l[S15_ROWS];
	int bad = 0;

	/* the checks under way: the suite's first screen */
	s15_console_init(&con, NULL);
	s15_console_add(&con, S15_ST_HEAD, tchk_first_lines[0]);
	for (unsigned i = 1; i < TCHK_FIRST_LINES; i++)
		s15_console_add(&con, S15_ST_TEXT, tchk_first_lines[i]);
	s15_console_add(&con, S15_ST_STEP_RUN, "Bridge\ttesting");
	s15_render_console(&con, &wb_display_geometry, wb_display_draw(&disp), &fr);
	bad |= write_raw(dir, "installer-checks");
	for (size_t k = 0; k < sizeof shots / sizeof shots[0]; k++) {
		struct inst_screen sc;
		unsigned n;

		memset(&sc, 0, sizeof sc);
		sc.phase = shots[k].phase;
		sc.version = ver;
		sc.target = 1u;
		sc.lock = shots[k].lock;
		sc.last_rc = 0xFFu;
		sc.os = shots[k].phase == INST_DONE ? INST_OS_ACTIVE : INST_OS_CHECKED;
		sc.trial = shots[k].trial;
		sc.hold_digit = shots[k].hold_digit;
		sc.hold_fill = shots[k].hold_fill;
		sc.hold_left_s = shots[k].hold_left_s;
		sc.keys_seen = shots[k].phase == INST_GUIDED ? 1u : 0u;
		n = inst_screen_lines(&sc, l);
		s15_console_init(&con, NULL);
		for (unsigned i = 0; i < n; i++)
			s15_console_add(&con, (enum s15_style)l[i].style, l[i].s);
		s15_render_console(&con, &wb_display_geometry, wb_display_draw(&disp), &fr);
		bad |= write_raw(dir, shots[k].name);
	}
	return bad;
}

int main(int argc, char **argv)
{
	const char *d;
	int bad = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: ui-shots <directory>\n");
		return 2;
	}
	snprintf(ver, sizeof ver, "%u.%u.%u", SHOT_MAJOR, SHOT_MINOR, SHOT_PATCH);
	snprintf(ver_next, sizeof ver_next, "%u.%u.%u", SHOT_MAJOR, SHOT_MINOR, SHOT_PATCH + 1);
	d = argv[1];
	wb_display_init(&disp, fb);

	/* S-15.132: the frame the OS hand-over shows */
	wb_display_handover_frame(NULL, fb);
	bad |= write_raw(d, "handover");

	/* recovery (S-02.084): the main screen, the slot menu (key 1), the
	 * confirm screen of a try with key 1 held 3 s, the countdown before
	 * a host's write of slot 2, that write received and written, and the
	 * slot line's other kinds */
	{
		static struct fr_screen rs;
		struct fr_ui ui;

		recovery_unit();
		recovery_shot(&rs, NULL);
		front_shot(&rs);
		bad |= write_raw(d, "recovery");

		fr_ui_reset(&ui);
		ui.screen = FR_UI_SLOTS;
		recovery_shot(&rs, &ui);
		front_shot(&rs);
		bad |= write_raw(d, "recovery-slots");

		ui.screen = FR_UI_CONFIRM;
		ui.act = FR_ACT_TRY;
		ui.holding = true;
		ui.hold_pct = 60;
		recovery_shot(&rs, &ui);
		front_shot(&rs);
		bad |= write_raw(d, "recovery-confirm");

		rec_p.phase = FR_PH_COUNTDOWN;
		rec_p.w.way = FR_WAY_PLAIN;
		rec_p.w.target = 1;
		rec_p.has_version = true;
		rec_p.version[0] = SHOT_MAJOR;
		rec_p.version[1] = SHOT_MINOR;
		rec_p.version[2] = SHOT_PATCH + 1;
		rec_p.count_left = 9;
		recovery_shot(&rs, NULL);
		front_shot(&rs);
		bad |= write_raw(d, "recovery-countdown");

		/* a 360 KiB image for slot 2, 40% received, then written
		 * with the slot's bytes 60% programmed */
		recovery_unit();
		rec_p.phase = FR_PH_RECEIVING;
		rec_p.plan_way = FR_WAY_PLAIN;
		rec_p.plan_target = 1;
		rec_p.len = 360u * 1024u;
		rec_p.got = 144u * 1024u;
		recovery_shot(&rs, NULL);
		front_shot(&rs);
		bad |= write_raw(d, "recovery-receiving");

		rec_p.phase = FR_PH_WRITING;
		rec_p.w.way = FR_WAY_PLAIN;
		rec_p.w.act = FR_ACT_NONE;
		rec_p.w.target = 1;
		rec_p.w.state = FR_WS_SLOT;
		rec_p.w.n = rec_p.len;
		rec_p.w.pos = rec_p.len / 5u * 3u;
		recovery_shot(&rs, NULL);
		front_shot(&rs);
		bad |= write_raw(d, "recovery-writing");

		/* S-02.106: slot 2 holds a build of the TE table, then a
		 * bootloader of no known kind; slot information only */
		recovery_unit();
		rec_st.slot[1].kind = FR_KIND_TE_BUILD;
		rec_st.slot[1].has_desc = false;
		rec_st.slot[1].has_front = false;
		memcpy(rec_st.slot[1].te_version, te, sizeof te);
		fr_rec_target(&rec_st);
		recovery_shot(&rs, NULL);
		front_shot(&rs);
		bad |= write_raw(d, "recovery-te");

		rec_st.slot[1].kind = FR_KIND_UNKNOWN;
		memset(rec_st.slot[1].te_version, 0, sizeof te);
		recovery_shot(&rs, NULL);
		front_shot(&rs);
		bad |= write_raw(d, "recovery-unknown");
	}

	/* S-13c.025: the line under the logo on the way into recovery */
	{
		static struct fr_screen ls;

		fr_screen_logo(&ls);
		front_shot(&ls);
		loading_line();
		bad |= write_raw(d, "recovery-loading");
	}

	setup(); start();
	bad |= shot(d, "main");

	/* protected writes on (Settings, 2 held for 5 s), then 8 back */
	setup(); boot.start.why = S15_START_ENCRYPTED; boot.start.part = 1;
	boot.slave = S15_SLAVE_ABSENT; start();
	press(0x07); hold(0x07); press(0x15);
	bad |= shot(d, "main-states");

	/* S-15.163: a plain start that failed, then Device Info (4) and
	 * System (5) with the slave lost */
	setup(); start_lost(); start();
	bad |= shot(d, "main-failed");
	press(0x09);
	bad |= shot(d, "info-failed");
	press(0x15); press(0x12);
	bad |= shot(d, "system-lost");

	setup(); start();
	press(0x07);
	bad |= shot(d, "settings");
	hold_for(0x07, 2000);	/* 2 of the 5 s */
	bad |= shot(d, "option");

	/* S-15.173: Settings with the color on, the one shot in color */
	setup(); boot.settings.color = true; start();
	press(0x07);
	bad |= shot(d, "settings-color");

	setup(); boot.slave = S15_SLAVE_ABSENT;
	start();
	press(0x09);
	bad |= shot(d, "info");
	press(0x06);		/* S-15.082: the second page */
	bad |= shot(d, "info2");

	setup(); npacks = 3; start();
	press(0x12);
	bad |= shot(d, "system");
	hold_for(0x14, 2000);
	bad |= shot(d, "system-armed");

	setup(); start();
	press(0x12);
	press(0x06);
	s15_menu_key(&menu, 0x10, true); s15_menu_key(&menu, 0x10, false);
	s15_menu_key(&menu, 0x20, true);
	bad |= shot(d, "keytest");

	setup(); start();
	press(0x08);
	bad |= shot(d, "developer");

	/* S-15.094: the OS write running, done, failed */
	osw_dir = d;
	osw_how = OSW_RUN;
	setup(); start();
	press(0x08); press(0x12);
	osw_how = OSW_DONE;
	setup(); start();
	press(0x08); press(0x12);
	bad |= shot(d, "oswrite-done");
	osw_how = OSW_FAILED;
	setup(); start();
	press(0x08); press(0x12);
	bad |= shot(d, "oswrite-failed");
	bad |= osw_bad;

	setup(); npacks = 7; start();
	press(0x12); press(0x08); hold_for(0x06, 2000);
	bad |= shot(d, "packs");

	setup(); start();
	press(0x12); press(0x12);
	bad |= shot(d, "format");

	setup(); start();
	press(0x06);
	bad |= shot(d, "upgrade-wait");

	setup(); boot.reason = 30; start();
	s15_up_run_start(&menu, S15_RUN_FIRMWARE, "1.2.0", NULL, false);
	s15_up_step_start(&menu, S15_STEP_CONTENT);
	s15_up_step_result(&menu, S15_STEP_CONTENT, S15_RES_OK, NULL, false, NULL);
	s15_up_step_start(&menu, S15_STEP_OS);
	s15_up_step_progress(&menu, S15_STEP_OS, 64, 100);
	bad |= shot(d, "upgrade-run");
	s15_up_step_result(&menu, S15_STEP_OS, S15_RES_OK, "1.2.0", false, NULL);
	s15_up_step_start(&menu, S15_STEP_BOOTLOADER);
	s15_up_step_result(&menu, S15_STEP_BOOTLOADER, S15_RES_SKIPPED, NULL, false, NULL);
	s15_up_step_start(&menu, S15_STEP_BLE);
	s15_up_step_result(&menu, S15_STEP_BLE, S15_RES_ALREADY, NULL, false, NULL);
	s15_up_step_start(&menu, S15_STEP_BLE_BOOTLOADER);
	s15_up_step_result(&menu, S15_STEP_BLE_BOOTLOADER, S15_RES_FAILURE, NULL,
			   false, "no slot answer");
	s15_up_run_end(&menu, S15_OUT_FAILED, 0);
	bad |= shot(d, "upgrade-failed");

	/* S-15.190: Continue to the OS at the host's request (S-10c.076) */
	setup(); start();
	(void)s15_menu_continue(&menu);
	bad |= shot(d, "continue-restarting");

	/* this version's run: the content and the OS installed, the
	 * Nordic's and the PSoC's parts switched off (S-11.073) */
	setup(); boot.reason = 30; start();
	s15_up_run_start(&menu, S15_RUN_FIRMWARE, "1.2.0", NULL, false);
	s15_up_step_start(&menu, S15_STEP_CONTENT);
	s15_up_step_result(&menu, S15_STEP_CONTENT, S15_RES_OK, NULL, false, NULL);
	s15_up_step_start(&menu, S15_STEP_OS);
	s15_up_step_result(&menu, S15_STEP_OS, S15_RES_OK, "1.2.0", false, NULL);
	s15_up_step_start(&menu, S15_STEP_BLE);
	s15_up_step_result(&menu, S15_STEP_BLE, S15_RES_SKIPPED_OFF, NULL, false, NULL);
	s15_up_step_start(&menu, S15_STEP_BLE_BOOTLOADER);
	s15_up_step_result(&menu, S15_STEP_BLE_BOOTLOADER, S15_RES_SKIPPED_OFF, NULL,
			   false, NULL);
	s15_up_step_start(&menu, S15_STEP_KEYBOARD);
	s15_up_step_result(&menu, S15_STEP_KEYBOARD, S15_RES_SKIPPED_OFF, NULL,
			   false, NULL);
	s15_up_run_end(&menu, S15_OUT_DONE,
		       (1u << S15_STEP_BLE) | (1u << S15_STEP_BLE_BOOTLOADER) |
		       (1u << S15_STEP_KEYBOARD));
	bad |= shot(d, "upgrade-done");

	/* S-15.116: the bootloader's confirmation, protected writes on
	 * (Settings, 2 held for 5 s), digit 1 held for 2 s */
	setup(); start();
	press(0x07); hold(0x07); press(0x15); press(0x06);
	s15_up_released(&menu, 100000);
	s15_up_tick(&menu, 102000);
	s15_up_phase(&menu, S15_PH_EXTRACT_FIRMWARE);
	{
		const struct s15_confirm_info info = { ver_next,
							ver, 0 };

		s15_up_confirm(&menu, &info);
		s15_up_confirm_bar(&menu, 40, 52);
	}
	bad |= shot(d, "upgrade-confirm");

	bad |= installer_shots(d);
	return bad;
}

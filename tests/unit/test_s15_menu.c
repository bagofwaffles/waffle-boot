/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15 held mode: the menu's rules on every key of every screen and state
 * (test_rules), the screens of tests/vectors/s15/ui/screens.json, and the
 * behavior the requirements state.
 *
 * The effect of a key is read back from what the menu does, never from
 * the menu's own account of it: the fake environment records every
 * operation, and a recording log captures every line the console appends
 * (S-15.004).
 *
 * Requirements: S-15.004, S-15.010, S-15.011, S-15.012, S-15.013, S-15.014, S-15.015,
 * S-15.020, S-15.031, S-15.032, S-15.042, S-15.043, S-15.050, S-15.051,
 * S-15.052, S-15.060, S-15.061, S-15.062, S-15.065, S-15.071, S-15.072,
 * S-15.073, S-15.081, S-15.082, S-15.092, S-15.093, S-15.094, S-15.095, S-15.100,
 * S-15.102, S-15.103, S-15.104, S-15.105, S-15.115, S-15.140, S-15.161,
 * S-15.170, S-15.172, S-15.175, S-15.180, S-15.190, S-15.194, S-15.195,
 * S-05.033, S-16.012,
 * S-16.123, S-16.161.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "waffle/s14_log.h"
#include "waffle/s15_menu.h"
#include "waffle/s15_prompt.h"
#include "s14_check.h"
#define S15V_SCREENS
#define S15V_PROMPT_KEYS

#define S15V_UPGRADE
#define S15V_OSW
#include "vectors/s15_vectors.h"

/* ---- the fake environment ------------------------------------------- */

#define MAXREC 64
#define ROWLEN (S15_LINE + 16)    /* a row as rows() writes it */

struct fake {
	/* file system */
	bool refused;
	bool lost;                        /* the slave stopped answering */
	bool format_fails;                /* S-15.072: a failed format */
	bool refused_after;               /* the remount's result */
	bool dlc;
	unsigned npacks;
	const char *pack_names[9];
	const char *manifests[9];
	const char *fail_remove;          /* a path whose removal fails */
	enum s15_os_write osw;            /* the result when osw_case is NULL */
	const struct s15v_osw *osw_case;  /* the write the fake plays (S-15.094) */
	struct s15_osw_end osw_in;        /* *end as the menu passed it */
	char osw_progress[ROWLEN];        /* the last row after the Write step's progress */
	bool osw_same;                    /* an equal value did not change the screen */
	/* records */
	enum s15_usb_set usb[MAXREC];
	unsigned n_usb;
	char removed[MAXREC][300];
	unsigned n_removed;
	unsigned volcal, leds_start, leds_end, erase_fat, format, os_write,
		 reset_w7, sysinfo, scan;
	bool restart_line;                /* shown before the board's reset */
	int option;                       /* -1 none, 0 off, 1 on */
	unsigned saves;                   /* settings_save calls */
	struct s15_settings saved;        /* the last one's settings */
	unsigned dir_checks, vol_checks;
	unsigned order;                   /* sequence counter */
	unsigned sysinfo_at, disk_at;
	bool disk_shows_screen, disk_shows_usb;
	/* S-15.190: continue to the OS */
	unsigned cont;                    /* continue_os calls */
	bool cont_view;                   /* the restarting view shown at the call */
	unsigned leds_end_at_cont;        /* the key test's ends by then */
	/* S-15.172: what a save gives, and the start in boot menu a start
	 * then reads when it was not saved */
	enum s15_store_save store_rc;
	bool store_after;
	/* S-15.194: Start /os.ldr once; what the check gives */
	unsigned once_checks, once;
	bool once_view;                   /* the restarting view shown at the call */
	enum s15_once once_rc;
	struct s15_os_file once_file;
	/* S-02.105: the factory reset's clears and what they give */
	unsigned clears;
	enum s15_store_save clear_rc;
	/* S-15.082: the start log read again, and what it reads then */
	unsigned log_reads;
	bool log_change;
	/* S-15.191: the scans before a write, when each came in the order,
	 * and whether the console's last line was the checking line then */
	unsigned files_scans, files_at;
	bool files_line;
	unsigned usb_at_files;            /* USB sets started before the scan */
	unsigned removed_at_files;        /* removals made before it */
	unsigned saves_at_files;
};

static struct fake F;
static unsigned n_app;    /* lines appended, recorded by the log below */

static struct s15_menu M;

static void f_usb(void *c, enum s15_usb_set s)
{
	(void)c;
	if (F.n_usb < MAXREC)
		F.usb[F.n_usb++] = s;
	if (s == S15_USB_DISK) {
		struct s15_frame fr;

		F.disk_at = ++F.order;
		/* S-15.100: what a frame drawn while the set starts shows */
		s15_menu_frame(&M, &fr);
		F.disk_shows_screen = M.screen == S15_SCR_UPGRADE;
		F.disk_shows_usb = (fr.status & S15_STATUS_USB) != 0u;
	}
}

static void f_volcal(void *c) { (void)c; F.volcal++; }

static void f_leds(void *c, bool start)
{
	(void)c;
	if (start)
		F.leds_start++;
	else
		F.leds_end++;
}

static bool f_refused(void *c) { (void)c; F.vol_checks++; return F.refused; }
static bool f_lost(void *c) { (void)c; return F.lost; }

static bool f_dir_exists(void *c, const char *path)
{
	(void)c;
	F.dir_checks++;
	return strcmp(path, "/yaffs2/content/dlc") == 0 && F.dlc;
}

static bool f_pack_dir(void *c, size_t i, char name[S15_NAME_MAX + 1])
{
	(void)c;
	if (!F.dlc || i >= F.npacks)
		return false;
	snprintf(name, S15_NAME_MAX + 1, "%s", F.pack_names[i]);
	return true;
}

static size_t f_manifest(void *c, const char *dir, uint8_t *buf, size_t max)
{
	(void)c;
	for (unsigned i = 0; i < F.npacks; i++)
		if (strcmp(F.pack_names[i], dir) == 0 && F.manifests[i] != NULL) {
			size_t n = strlen(F.manifests[i]);

			n = n < max ? n : max;
			memcpy(buf, F.manifests[i], n);
			return n;
		}
	return 0;
}

/* The long calls hold the keys, as the board's do (opxy_services.c). */
static bool f_remove(void *c, const char *path)
{
	(void)c;
	s15_menu_hold(&M);
	if (F.n_removed < MAXREC)
		snprintf(F.removed[F.n_removed++], 300, "%s", path);
	if (F.fail_remove != NULL && strcmp(path, F.fail_remove) == 0)
		return false;
	if (strcmp(path, "/yaffs2/content/dlc") == 0) {
		F.dlc = false;
		F.npacks = 0;
	}
	if (strncmp(path, "/yaffs2/content/dlc/", 20) == 0)
		for (unsigned i = 0; i < F.npacks; i++)
			if (strcmp(path + 20, F.pack_names[i]) == 0) {
				for (unsigned j = i; j + 1 < F.npacks; j++) {
					F.pack_names[j] = F.pack_names[j + 1];
					F.manifests[j] = F.manifests[j + 1];
				}
				F.npacks--;
				break;
			}
	return true;
}

static void f_erase(void *c) { (void)c; s15_menu_hold(&M); F.erase_fat++; }
static struct s15_boot BOOT;

static bool f_format(void *c)
{
	(void)c;
	s15_menu_hold(&M);
	F.format++;
	/* the remount scans the erased volume (S-12.057) */
	if (BOOT.files_way == S15_FILES_RESTORED)
		BOOT.files_way = S15_FILES_NO_CKPT;
	F.refused = F.refused_after;	/* the remount after every format */
	return !F.format_fails;
}
static void f_os_write(void *c, struct s15_osw_end *end);
/* The restart: its line is the console's last when the board is called. */
static void f_reset(void *c)
{
	size_t n = 0;
	const char *s = M.con.count != 0u ?
			s15_console_row(&M.con, M.con.count - 1u, &n) : "";

	(void)c;
	F.reset_w7++;
	F.restart_line = n == strlen(s15_restarting) &&
			 memcmp(s, s15_restarting, n) == 0;
}
static void f_sysinfo(void *c) { (void)c; F.sysinfo++; F.sysinfo_at = ++F.order; }
static void f_scan(void *c) { (void)c; F.scan++; }
static void f_option(void *c, bool on) { (void)c; F.option = on; }

static enum s15_store_save f_settings(void *c, const struct s15_settings *s,
				      struct s15_settings *now)
{
	(void)c;
	F.saves++;
	F.saved = *s;
	*now = *s;
	if (F.store_rc != S15_STORE_SAVED)
		now->start_menu = F.store_after;
	return F.store_rc;
}

static void f_continue(void *c)
{
	size_t n = 0;
	const char *s = M.con.count != 0u ?
			s15_console_row(&M.con, M.con.count - 1u, &n) : "";

	(void)c;
	F.cont++;
	F.cont_view = M.screen == S15_SCR_STARTING && !M.keytest &&
		      n == strlen(s15_cont_restarting) &&
		      memcmp(s, s15_cont_restarting, n) == 0;
	F.leds_end_at_cont = F.leds_end;
}


/* S-15.082: a read of the start log that finds one more start each time. */
static void f_logread(void *c)
{
	(void)c;
	F.log_reads++;
	if (F.log_change)
		BOOT.start_log_used++;
}

/* S-12.062: the board's scan; the mount is a scanned one afterwards. */
static void f_files(void *c)
{
	size_t n = 0;
	const char *s = M.con.count != 0u ?
			s15_console_row(&M.con, M.con.count - 1u, &n) : "";

	(void)c;
	s15_menu_hold(&M);
	F.files_scans++;
	F.files_at = ++F.order;
	F.usb_at_files = F.n_usb;
	F.removed_at_files = F.n_removed;
	F.saves_at_files = F.saves;
	F.files_line = n == strlen(s15_checking_files) &&
		       memcmp(s, s15_checking_files, n) == 0;
	BOOT.files_way = S15_FILES_FOR_WRITE;
}

static enum s15_store_save f_clear(void *c)
{
	(void)c;
	F.clears++;
	return F.clear_rc;
}

/* S-15.194 */
static enum s15_once f_once_check(void *c, struct s15_os_file *file)
{
	(void)c;
	F.once_checks++;
	*file = F.once_file;
	return F.once_rc;
}

static void f_once(void *c)
{
	size_t n = 0;
	const char *s = M.con.count != 0u ?
			s15_console_row(&M.con, M.con.count - 1u, &n) : "";

	(void)c;
	F.once++;
	F.once_view = M.screen == S15_SCR_STARTING &&
		      n == strlen(s15_once_restarting) &&
		      memcmp(s, s15_once_restarting, n) == 0;
}

static const struct s15_env_ops env = {
	f_usb, f_volcal, f_leds, f_refused, f_dir_exists, f_pack_dir,
	f_manifest, f_remove, f_erase, f_format, f_os_write, f_reset,
	f_sysinfo, f_scan, f_option, f_settings, NULL, f_lost, f_continue,
	f_logread, f_files, f_clear, f_once_check, f_once,
};

/* A board with no boot store, no continue_os for the host and no file
 * start (the test OS). */
static const struct s15_env_ops env_bare = {
	f_usb, f_volcal, f_leds, f_refused, f_dir_exists, f_pack_dir,
	f_manifest, f_remove, f_erase, f_format, f_os_write, f_reset,
	f_sysinfo, f_scan, f_option, NULL, NULL, f_lost, NULL,
	NULL, NULL, NULL, NULL, NULL,
};

static void clear_records(void)
{
	F.n_usb = F.n_removed = 0;
	F.volcal = F.leds_start = F.leds_end = F.erase_fat = F.format = 0;
	F.os_write = F.reset_w7 = F.sysinfo = F.scan = 0;
	F.once_checks = F.once = 0;
	F.once_view = false;
	F.option = -1;
	F.saves = 0;
	F.dir_checks = F.vol_checks = 0;
	F.order = F.sysinfo_at = F.disk_at = 0;
	F.cont = 0;
	F.clears = 0;
}

static void reset_fake(void)
{
	n_app = 0;
	memset(&F, 0, sizeof F);
	F.osw = S15_OSW_OK;
	F.once_rc = S15_ONCE_FILE;	/* no /os.ldr unless a test puts one */
	F.once_file.why = S15_OSW_WHY_MISSING;
	F.once_file.value = 2u;
	clear_records();
}

static bool usb_called(enum s15_usb_set s)
{
	for (unsigned i = 0; i < F.n_usb; i++)
		if (F.usb[i] == s)
			return true;
	return false;
}

static bool removed(const char *path)
{
	for (unsigned i = 0; i < F.n_removed; i++)
		if (strcmp(F.removed[i], path) == 0)
			return true;
	return false;
}

/* ---- a log that records appended lines ------------------------------ */

#define MAXLINES 128
static char app[MAXLINES][100];
static unsigned n_log_frames;

static bool rec_send(void *c, uint8_t route, uint8_t id, const uint8_t *p,
		     size_t len)
{
	size_t tl = s14_tag_len(p + 4, len - 4u), n = len - 4u - tl;

	(void)c;
	(void)route;
	(void)id;
	n_log_frames++;
	if (n_app < MAXLINES) {
		n = n < 99 ? n : 99;
		memcpy(app[n_app], p + 4 + tl, n);
		app[n_app][n] = '\0';
		n_app++;
	}
	return true;
}

static const struct s14_link_ops rec_link = { rec_send, NULL };
static struct s14_log LOG;

static bool appended(const char *s)
{
	for (unsigned i = 0; i < n_app; i++)
		if (strcmp(app[i], s) == 0)
			return true;
	return false;
}

/* ---- menus ------------------------------------------------------------- */

static void default_boot(void)
{
	memset(&BOOT, 0, sizeof BOOT);
	BOOT.version = "0.3.1";
	BOOT.reason = 10;
	BOOT.unit_serial = "AB123456";
	BOOT.chip_serial = "0011223344556677";
	BOOT.psoc_serial = "PSOC0001";
	BOOT.os_version = "1.2.0";
	BOOT.sw_version = "1.2.0";
	BOOT.ble_version = "1.0.1";
	BOOT.psoc_version = "1.0.7";
	BOOT.provisioned = true;
	BOOT.start_log = S15_LOG_OK;
	BOOT.start_log_used = 41;
	BOOT.start_log_erase = 256;
	s15_settings_defaults(&BOOT.settings);
	BOOT.settings.dev_mode = true;	/* most tests use the developer menu */
}

/* The menu's clock (S-15.015), as the board gives it: ms since start(). */
static uint32_t T;

static void start(void)
{
	s14_log_init(&LOG, S14_MASTER, NULL, &rec_link, NULL, NULL);
	s15_menu_init(&M, &BOOT, &env, &LOG);
	T = 0;
	s15_menu_tick(&M, T);
	s15_menu_start(&M);
}

/* The time passes by ms. */
static void tick(uint32_t ms)
{
	T += ms;
	s15_menu_tick(&M, T);
}

/* A key event, then the board's pass: it reads what a long operation left
 * and releases the keys (opxy_held_settle()). */
static void ev(unsigned key, bool down)
{
	s15_menu_event(&M, key, down);
	s15_menu_release(&M);
}

static void press(unsigned key)
{
	ev(key, true);
	ev(key, false);
}

/* S-15.015: key held for the hold's time, then released. */
static void hold(unsigned key)
{
	ev(key, true);
	tick(S15_CONFIRM_HOLD_MS);
	ev(key, false);
}

/* The console's shown rows, as screens.json writes lines ("style:text",
 * or the text alone in the style text). */
static size_t rows(const char **out, char buf[S15_ROWS][ROWLEN])
{
	unsigned i;

	for (i = 0; i < M.con.count; i++) {
		size_t n;
		const char *s = s15_console_row(&M.con, i, &n);
		enum s15_style st = s15_console_row_style(&M.con, i);

		if (st == S15_ST_TEXT)
			snprintf(buf[i], ROWLEN, "%.*s", (int)n, s);
		else
			snprintf(buf[i], ROWLEN, "%s:%.*s", s15_style_names[st],
				 (int)n, s);
		out[i] = buf[i];
	}
	return i;
}

/* The last shown row, as rows() writes it. */
static const char *last_row(char *buf)
{
	const char *got[S15_ROWS];
	char b[S15_ROWS][ROWLEN];
	size_t n = rows(got, b);

	snprintf(buf, ROWLEN, "%s", n ? got[n - 1u] : "");
	return buf;
}

/* The row k from the end (0: the last), as rows() writes it. */
static const char *row_back(char *buf, size_t k)
{
	const char *got[S15_ROWS];
	char b[S15_ROWS][ROWLEN];
	size_t n = rows(got, b);

	snprintf(buf, ROWLEN, "%s", n > k ? got[n - 1u - k] : "");
	return buf;
}

/* The idle disk form's back line closes the console (S-15.103). */
static bool back_last(void)
{
	char b[ROWLEN];

	return strcmp(last_row(b), "back:8 Back to Main Menu") == 0 &&
	       strcmp(row_back(b, 1), "") == 0;
}

/* ---- the OS write the fake plays (S-15.094) ---------------------------- */

static int named(const char *const *names, size_t n, const char *s)
{
	for (size_t i = 0; i < n; i++)
		if (strcmp(names[i], s) == 0)
			return (int)i;
	return -1;
}

static const char *const osw_steps[] = { "read", "check", "target", "erase", "write", "verify", "activate" };
static const char *const osw_whys[] = {
	"", "refused", "missing", "directory", "read", "memory", "too_small", "too_large", "no_room",
	"select", "emmc_read", "emmc_write", "erase", "mismatch", "", "flush", "reads_differ", "not_os",
};
static const char *const osw_emmc_states[] = { "untouched", "unchanged", "bad_header",
					       "both_active", "unconfirmed" };

/* The steps of a case: each done with the values its row names, the one
 * that failed left open; progress on the steps that count it. */
static void osw_play(const struct s15v_osw *c, struct s15_osw_end *end)
{
	int stop = named(osw_steps, S15V_N(osw_steps), c->stop);
	bool failed = c->why[0] != '\0';

	for (unsigned s = 0; s < S15_OSW_STEP_COUNT && !(failed && stop < 0); s++) {
		bool counted = s == S15_OSW_STEP_READ || s == S15_OSW_STEP_ERASE ||
			       s == S15_OSW_STEP_WRITE || s == S15_OSW_STEP_VERIFY;

		s15_osw_step(&M, (enum s15_osw_step)s);
		if (counted) {
			s15_osw_progress(&M, 1, 2);
			if (s == S15_OSW_STEP_WRITE) {
				uint32_t changes = s15_menu_changes(&M);

				last_row(F.osw_progress);
				s15_osw_progress(&M, 2, 4);	/* still 50% */
				F.osw_same = s15_menu_changes(&M) == changes;
			}
		}
		if (failed && (int)s == stop)
			break;
		if (counted)
			s15_osw_progress(&M, 2, 2);
		s15_osw_done(&M, s == S15_OSW_STEP_READ ? c->length :
				 s == S15_OSW_STEP_CHECK ? (c->length + 511u) / 512u + 1u :
				 s == S15_OSW_STEP_TARGET || s == S15_OSW_STEP_ACTIVATE ? c->target : 0u,
			     s == S15_OSW_STEP_CHECK ? c->boot_size / 512u : 0u);
	}
	end->result = strcmp(c->result, "ok") == 0 ? S15_OSW_OK :
		      strcmp(c->result, "unreadable") == 0 ? S15_OSW_UNREADABLE : S15_OSW_FAILED;
	end->why = (enum s15_osw_why)named(osw_whys, S15V_N(osw_whys), c->why);
	end->emmc = (enum s15_osw_emmc)named(osw_emmc_states, S15V_N(osw_emmc_states), c->emmc);
	end->part = c->part;
	end->target = c->target;
	end->value = c->value;
	end->limit = c->limit;
	end->select0_failed = c->select0_failed;
}

static void f_os_write(void *c, struct s15_osw_end *end)
{
	(void)c;
	s15_menu_hold(&M);
	F.os_write++;
	F.osw_in = *end;
	if (F.osw_case != NULL)
		osw_play(F.osw_case, end);
	else
		end->result = F.osw;
}

static const struct s15v_screen *screen_vec(const char *name)
{
	for (size_t i = 0; i < S15V_N(s15v_screens); i++)
		if (strcmp(s15v_screens[i].name, name) == 0)
			return &s15v_screens[i];
	return NULL;
}

/* The console equals a screen of screens.json. */
static void expect_screen(const char *name)
{
	const char *got[S15_ROWS];
	char buf[S15_ROWS][ROWLEN];
	size_t n = rows(got, buf), i;
	const struct s15v_screen *s = screen_vec(name);

	CHECK(s != NULL, "no screen %s", name);
	if (s == NULL)
		return;
	for (i = 0; i < n && s->lines[i] != NULL; i++)
		CHECK(strcmp(got[i], s->lines[i]) == 0,
		      "%s row %zu: '%s', want '%s'", name, i, got[i],
		      s->lines[i]);
	CHECK(s->lines[i] == NULL && i == n, "%s: %zu rows", name, n);
}

/* The newest rows of the console end with these lines. */
static void expect_tail(const char *name)
{
	const char *got[S15_ROWS];
	char buf[S15_ROWS][ROWLEN];
	size_t n = rows(got, buf), k = 0, i;
	const struct s15v_screen *s = screen_vec(name);

	if (s == NULL)
		return;
	while (s->lines[k] != NULL)
		k++;
	CHECK(n >= k, "%s: tail of %zu rows", name, k);
	for (i = 0; i < k && n >= k; i++)
		CHECK(strcmp(got[n - k + i], s->lines[i]) == 0,
		      "%s tail row %zu: '%s'", name, i, got[n - k + i]);
}

/* The title bar names the screen as screens.json's titles. */
static void expect_title(const char *screen)
{
	struct s15_frame f;
	const char *want = NULL;

	for (size_t i = 0; i < S15V_N(s15v_titles); i++)
		if (strcmp(s15v_titles[i].screen, screen) == 0)
			want = s15v_titles[i].title;
	s15_menu_frame(&M, &f);
	CHECK(want != NULL && f.title != NULL && strcmp(f.title, want) == 0,
	      "title of %s: '%s'", screen, f.title ? f.title : "");
}

/* S-15.082: Device Info's second page, the start log read again each time
 * it opens; 8 goes back to the first page, no other key leaves it. */
static void info2_with(enum s15_log st, unsigned slot, unsigned used,
		       unsigned unfinished, const char *name)
{
	reset_fake(); default_boot();
	BOOT.start_log = (uint8_t)st;
	BOOT.start_log_slot = (uint8_t)slot;
	BOOT.start_log_used = (uint16_t)used;
	BOOT.start_log_unfinished = (uint16_t)unfinished;
	start();
	press(0x09);
	press(0x06);
	CHECK(M.screen == S15_SCR_INFO2 && F.log_reads == 1u, "S-15.082 %s opened", name);
	expect_screen(name);
}

static void test_info2(void)
{
	reset_fake(); default_boot(); BOOT.version = "1.0.0"; start();
	press(0x00);
	CHECK(F.log_reads == 0u, "S-15.082 the first page reads no log");
	press(0x06);
	expect_screen("info2");
	expect_title("info2");
	CHECK(F.log_reads == 1u, "S-15.082 read as it opens");
	press(0x06);                /* only 8 leaves it */
	press(0x0d);
	press(0x12);
	press(0x00);
	CHECK(M.screen == S15_SCR_INFO2 && F.log_reads == 1u, "S-15.082 other keys stay");
	press(0x15);
	CHECK(M.screen == S15_SCR_INFO, "S-15.082 8 back to Device Info");
	expect_screen("info");
	/* opened again: read again, and the new count shown */
	F.log_change = true;
	press(0x0a);                /* knob 1's click is digit 1 too */
	CHECK(F.log_reads == 2u && BOOT.start_log_used == 42u, "S-15.082 read again");
	{
		const char *got[S15_ROWS];
		char buf[S15_ROWS][ROWLEN];

		CHECK(rows(got, buf) > 1u &&
		      strcmp(got[1], "kv:Starts logged\t42 of 16384") == 0,
		      "S-15.082 the new count");
	}
	press(0x15);
	press(0x15);
	expect_screen("main_plain");
	info2_with(S15_LOG_OK, 1u, 3u, 2u, "info2_slot2");
	info2_with(S15_LOG_FULL, 0u, 0u, 0u, "info2_full");
	info2_with(S15_LOG_ODD, 0u, 0u, 0u, "info2_odd");
	info2_with(S15_LOG_NONE, 1u, 0u, 0u, "info2_none");
	info2_with(S15_LOG_UNREAD, 0u, 0u, 0u, "info2_unread");
	/* a board with no read: the page shows what held mode began with */
	reset_fake(); default_boot();
	s14_log_init(&LOG, S14_MASTER, NULL, &rec_link, NULL, NULL);
	s15_menu_init(&M, &BOOT, &env_bare, &LOG);
	s15_menu_start(&M);
	press(0x09);
	press(0x06);
	expect_screen("info2");
}

/* ---- the boot prompt: S-15.180 ---------------------------------------- */

/* The boot prompt, which is shown before held mode and whose keys
 * s15_prompt.h reads: a screen of its own past s15_menu.h's. */
#define SCR_PROMPT 100u

/* The prompt's next screen and effect for one key event on a prompt with
 * no key down when it opened. */
static void prompt_transition(unsigned key, bool down, unsigned *next,
			      const char **effect)
{
	struct s15_prompt p;

	s15_prompt_open(&p, NULL);
	s15_prompt_key(&p, key, down);
	switch (s15_prompt_choice(&p)) {
	case S15_PROMPT_START:
		*next = SCR_PROMPT;
		*effect = "start_os";
		break;
	case S15_PROMPT_HELD:
		*next = S15_SCR_MAIN;
		*effect = "held_mode";
		break;
	case S15_PROMPT_WAIT:
	default:
		*next = SCR_PROMPT;
		*effect = "none";
		break;
	}
}

/* ---- the menu's rules: every screen and state, every key ---------- */

enum { OPT_OFF, OPT_PENDING, OPT_ON };
enum { MTP_OFF, MTP_VENDOR, MTP_STANDARD };

/* A screen with the flags that change what its keys do. */
struct rstate {
	const char *name; unsigned screen;
	bool devmode, packs, armed; unsigned opt;      /* Main, system, settings, upgrade (opt) */
	bool color;                                   /* Settings (S-15.173); true elsewhere */
	unsigned pending;                              /* Packs */
	bool refused, fpend;                           /* Format */
	unsigned mtp; bool osw_done, osw_written;      /* Factory; the OS write's result shown */
	bool macos;                                   /* Settings and factory (S-15.174); true elsewhere */
	bool midi, idle, begun;                        /* Upgrade */
};

static const struct rstate rstates[] = {
	{ "devmode=0,screen=main", S15_SCR_MAIN, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "devmode=1,screen=main", S15_SCR_MAIN, true, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "pending=0,screen=packs", S15_SCR_PACKS, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "pending=1,screen=packs", S15_SCR_PACKS, false, false, false, OPT_OFF, true, 1u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "pending=2,screen=packs", S15_SCR_PACKS, false, false, false, OPT_OFF, true, 2u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "screen=info", S15_SCR_INFO, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "screen=info2", S15_SCR_INFO2, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "armed=0,packs=0,screen=system", S15_SCR_SYSTEM, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "armed=1,packs=0,screen=system", S15_SCR_SYSTEM, false, false, true, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "armed=0,packs=1,screen=system", S15_SCR_SYSTEM, false, true, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "armed=1,packs=1,screen=system", S15_SCR_SYSTEM, false, true, true, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "color=0,devmode=0,macos=0,opt=off,screen=settings", S15_SCR_SETTINGS, false, false, false, OPT_OFF, false, 0u, false, false, MTP_OFF, false, false, false, false, false, false },
	{ "color=0,devmode=0,macos=1,opt=off,screen=settings", S15_SCR_SETTINGS, false, false, false, OPT_OFF, false, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "color=1,devmode=0,macos=0,opt=off,screen=settings", S15_SCR_SETTINGS, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, false, false, false, false },
	{ "color=1,devmode=0,macos=1,opt=off,screen=settings", S15_SCR_SETTINGS, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "color=0,devmode=0,macos=0,opt=pending,screen=settings", S15_SCR_SETTINGS, false, false, false, OPT_PENDING, false, 0u, false, false, MTP_OFF, false, false, false, false, false, false },
	{ "color=0,devmode=0,macos=1,opt=pending,screen=settings", S15_SCR_SETTINGS, false, false, false, OPT_PENDING, false, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "color=1,devmode=0,macos=0,opt=pending,screen=settings", S15_SCR_SETTINGS, false, false, false, OPT_PENDING, true, 0u, false, false, MTP_OFF, false, false, false, false, false, false },
	{ "color=1,devmode=0,macos=1,opt=pending,screen=settings", S15_SCR_SETTINGS, false, false, false, OPT_PENDING, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "color=0,devmode=0,macos=0,opt=on,screen=settings", S15_SCR_SETTINGS, false, false, false, OPT_ON, false, 0u, false, false, MTP_OFF, false, false, false, false, false, false },
	{ "color=0,devmode=0,macos=1,opt=on,screen=settings", S15_SCR_SETTINGS, false, false, false, OPT_ON, false, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "color=1,devmode=0,macos=0,opt=on,screen=settings", S15_SCR_SETTINGS, false, false, false, OPT_ON, true, 0u, false, false, MTP_OFF, false, false, false, false, false, false },
	{ "color=1,devmode=0,macos=1,opt=on,screen=settings", S15_SCR_SETTINGS, false, false, false, OPT_ON, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "color=0,devmode=1,macos=0,opt=off,screen=settings", S15_SCR_SETTINGS, true, false, false, OPT_OFF, false, 0u, false, false, MTP_OFF, false, false, false, false, false, false },
	{ "color=0,devmode=1,macos=1,opt=off,screen=settings", S15_SCR_SETTINGS, true, false, false, OPT_OFF, false, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "color=1,devmode=1,macos=0,opt=off,screen=settings", S15_SCR_SETTINGS, true, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, false, false, false, false },
	{ "color=1,devmode=1,macos=1,opt=off,screen=settings", S15_SCR_SETTINGS, true, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "color=0,devmode=1,macos=0,opt=pending,screen=settings", S15_SCR_SETTINGS, true, false, false, OPT_PENDING, false, 0u, false, false, MTP_OFF, false, false, false, false, false, false },
	{ "color=0,devmode=1,macos=1,opt=pending,screen=settings", S15_SCR_SETTINGS, true, false, false, OPT_PENDING, false, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "color=1,devmode=1,macos=0,opt=pending,screen=settings", S15_SCR_SETTINGS, true, false, false, OPT_PENDING, true, 0u, false, false, MTP_OFF, false, false, false, false, false, false },
	{ "color=1,devmode=1,macos=1,opt=pending,screen=settings", S15_SCR_SETTINGS, true, false, false, OPT_PENDING, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "color=0,devmode=1,macos=0,opt=on,screen=settings", S15_SCR_SETTINGS, true, false, false, OPT_ON, false, 0u, false, false, MTP_OFF, false, false, false, false, false, false },
	{ "color=0,devmode=1,macos=1,opt=on,screen=settings", S15_SCR_SETTINGS, true, false, false, OPT_ON, false, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "color=1,devmode=1,macos=0,opt=on,screen=settings", S15_SCR_SETTINGS, true, false, false, OPT_ON, true, 0u, false, false, MTP_OFF, false, false, false, false, false, false },
	{ "color=1,devmode=1,macos=1,opt=on,screen=settings", S15_SCR_SETTINGS, true, false, false, OPT_ON, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "fpend=0,refused=0,screen=format", S15_SCR_FORMAT, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "fpend=0,refused=1,screen=format", S15_SCR_FORMAT, false, false, false, OPT_OFF, true, 0u, true, false, MTP_OFF, false, false, true, false, false, false },
	{ "fpend=1,refused=0,screen=format", S15_SCR_FORMAT, false, false, false, OPT_OFF, true, 0u, false, true, MTP_OFF, false, false, true, false, false, false },
	{ "fpend=1,refused=1,screen=format", S15_SCR_FORMAT, false, false, false, OPT_OFF, true, 0u, true, true, MTP_OFF, false, false, true, false, false, false },
	{ "screen=result", S15_SCR_RESULT, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "macos=0,mtp=off,screen=factory", S15_SCR_FACTORY, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, false, false, false, false },
	{ "macos=1,mtp=off,screen=factory", S15_SCR_FACTORY, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "mtp=vendor,screen=factory", S15_SCR_FACTORY, false, false, false, OPT_OFF, true, 0u, false, false, MTP_VENDOR, false, false, true, false, false, false },
	{ "mtp=standard,screen=factory", S15_SCR_FACTORY, false, false, false, OPT_OFF, true, 0u, false, false, MTP_STANDARD, false, false, true, false, false, false },
	{ "mtp=off,osw=done,screen=factory", S15_SCR_FACTORY, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, true, false, true, false, false, false },
	{ "mtp=off,osw=written,screen=factory", S15_SCR_FACTORY, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, true, true, false, false, false },
	{ "form=disk,idle=0,opt=off,screen=upgrade", S15_SCR_UPGRADE, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "form=disk,idle=0,opt=on,screen=upgrade", S15_SCR_UPGRADE, false, false, false, OPT_ON, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
	{ "form=disk,idle=1,opt=off,screen=upgrade", S15_SCR_UPGRADE, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, true, false },
	{ "form=disk,idle=1,opt=on,screen=upgrade", S15_SCR_UPGRADE, false, false, false, OPT_ON, true, 0u, false, false, MTP_OFF, false, false, true, false, true, false },
	{ "begun=1,form=disk,idle=0,opt=off,screen=upgrade", S15_SCR_UPGRADE, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, true },
	{ "begun=1,form=disk,idle=0,opt=on,screen=upgrade", S15_SCR_UPGRADE, false, false, false, OPT_ON, true, 0u, false, false, MTP_OFF, false, false, true, false, false, true },
	{ "begun=1,form=disk,idle=1,opt=off,screen=upgrade", S15_SCR_UPGRADE, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, true, true },
	{ "begun=1,form=disk,idle=1,opt=on,screen=upgrade", S15_SCR_UPGRADE, false, false, false, OPT_ON, true, 0u, false, false, MTP_OFF, false, false, true, false, true, true },
	{ "form=midi,idle=0,opt=off,screen=upgrade", S15_SCR_UPGRADE, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, true, false, false },
	{ "form=midi,idle=0,opt=on,screen=upgrade", S15_SCR_UPGRADE, false, false, false, OPT_ON, true, 0u, false, false, MTP_OFF, false, false, true, true, false, false },
	{ "form=midi,idle=1,opt=off,screen=upgrade", S15_SCR_UPGRADE, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, true, true, false },
	{ "form=midi,idle=1,opt=on,screen=upgrade", S15_SCR_UPGRADE, false, false, false, OPT_ON, true, 0u, false, false, MTP_OFF, false, false, true, true, true, false },
	{ "screen=boot_prompt", SCR_PROMPT, false, false, false, OPT_OFF, true, 0u, false, false, MTP_OFF, false, false, true, false, false, false },
};


#define NO_KEY 0xFFu

/* S-15.015: the key a state's hold is held with, or NO_KEY. */
static unsigned held_key(const struct rstate *st)
{
	if (st->screen == S15_SCR_SYSTEM && st->armed)
		return 0x14u;
	if (st->screen == S15_SCR_PACKS && st->pending != 0u)
		return st->pending == 1u ? 0x06u : 0x07u;
	if (st->screen == S15_SCR_FORMAT && st->fpend)
		return 0x06u;
	if (st->screen == S15_SCR_SETTINGS && st->opt == OPT_PENDING)
		return 0x07u;
	return NO_KEY;
}

static void setup_state(const struct rstate *st)
{
	reset_fake();
	default_boot();
	BOOT.settings.dev_mode = !((st->screen == S15_SCR_MAIN ||
				    st->screen == S15_SCR_SETTINGS) && !st->devmode);
	BOOT.settings.color = st->color;
	BOOT.settings.mtp_macos = st->macos && st->mtp != MTP_STANDARD;
	F.dlc = st->packs || st->screen == S15_SCR_PACKS;
	if (F.dlc) {
		F.npacks = 2;
		F.pack_names[0] = "p1";
		F.pack_names[1] = "p2";
	}
	F.refused = st->refused;
	if (st->screen == S15_SCR_UPGRADE)
		BOOT.reason = st->midi ? 40 : 30;
	start();
	switch (st->screen) {
	case S15_SCR_PACKS:
		press(0x12);
		press(0x08);
		if (st->pending != 0u)
			ev(held_key(st), true);		/* S-15.015: held */
		break;
	case S15_SCR_INFO:
		press(0x00);
		break;
	case S15_SCR_INFO2:
		press(0x00);
		press(0x06);
		break;
	case S15_SCR_SYSTEM:
		press(0x12);
		if (st->armed)
			ev(held_key(st), true);
		break;
	case S15_SCR_RESULT:
		press(0x12);
		hold(0x14);			/* a factory reset's result */
		break;
	case S15_SCR_SETTINGS:
		press(0x07);
		break;
	case S15_SCR_FORMAT:
		press(0x12);
		press(0x12);
		if (st->fpend)
			ev(held_key(st), true);
		break;
	case S15_SCR_FACTORY:
		press(0x08);
		if (st->mtp == MTP_VENDOR || st->mtp == MTP_STANDARD)
			press(0x07);
		else if (st->osw_done || st->osw_written) {
			F.osw = st->osw_written ? S15_OSW_OK : S15_OSW_FAILED;
			press(0x12);			/* S-15.094: the write, done */
		}
		CHECK((M.osw == S15_OSW_VIEW_DONE) == (st->osw_done || st->osw_written),
		      "setup %s: osw", st->name);
		break;
	case S15_SCR_UPGRADE:
		if (!st->idle)
			M.activity = S15_ACT_RUN;
		M.begun = st->begun;
		break;
	case S15_SCR_MAIN:
	case S15_SCR_NONE:
	default:
		break;
	}
	CHECK(M.screen == st->screen, "setup %s", st->name);
	if (st->screen == S15_SCR_SETTINGS || st->screen == S15_SCR_UPGRADE) {
		M.option_on = st->opt == OPT_ON;
		if (st->opt == OPT_PENDING)
			ev(held_key(st), true);
	}
	clear_records();
	n_app = 0;
}

#define MAXTOK 8
struct toks {
	char t[MAXTOK][64];
	unsigned n;
};

static void add(struct toks *t, const char *s)
{
	if (t->n < MAXTOK)
		snprintf(t->t[t->n++], sizeof t->t[0], "%s", s);
}

/* What happened, from the records. */
static void observe(struct toks *t, const struct rstate *st,
		    unsigned key, enum s15_screen before, bool armed_before,
		    bool pending_before, bool fpend_before, bool osw_before,
		    bool pack_before)
{
	char want[48];
	bool reset_ran = removed("/yaffs2/user");
	bool armed_now = M.armed == S15_ARM_RESET;
	/* armed by its key (a redraw of an armed screen is not an arming) */
	bool arm = armed_now && key == 0x14u &&
		   appended("7 Factory reset  Hold 7 for 5 s");

	(void)st;
	t->n = 0;
	/* Opening the main screen starts its USB set, opening the system menu
	 * looks at the volume for content/dlc; drawing either again does
	 * neither. */
	if (M.screen == S15_SCR_MAIN && usb_called(S15_USB_MAIN))
		add(t, "open");
	else if (M.screen == S15_SCR_SYSTEM && M.screen != before &&
		 F.vol_checks != 0u)
		add(t, "open");
	else if (M.screen != before && M.screen != S15_SCR_MAIN &&
		 M.screen != S15_SCR_SYSTEM && M.screen != S15_SCR_UPGRADE &&
		 M.screen != S15_SCR_RESULT)
		add(t, "open");
	if (M.screen == S15_SCR_SYSTEM && before == S15_SCR_PACKS &&
	    F.vol_checks == 0u)
		add(t, "redraw");
	/* S-15.094: the developer menu drawn again from the OS write's screen */
	if (M.screen == S15_SCR_FACTORY && osw_before && M.osw == S15_OSW_VIEW_NONE &&
	    appended("5 Write /os.ldr to boot partition"))
		add(t, "redraw");
	if (usb_called(S15_USB_DISK))
		add(t, "open_disk");
	if (armed_before && ((!armed_now && !reset_ran) || arm))
		add(t, "disarm");
	if (arm)
		add(t, "arm");
	if (appended(s15_option_prompt[0]))
		add(t, "option_prompt");
	if (pending_before && F.option != 1 && M.armed != S15_ARM_OPTION)
		add(t, "option_cancel");
	if (F.option == 1)
		add(t, "option_on");
	if (F.option == 0)
		add(t, "option_off");
	if (reset_ran)
		add(t, "factory_reset");
	/* S-15.172: a save, for the setting it changed */
	if (F.saves != 0u && F.saved.dev_mode != BOOT.settings.dev_mode)
		add(t, F.saved.dev_mode ? "dev_mode_on" : "dev_mode_off");
	if (F.saves != 0u && F.saved.color != BOOT.settings.color)
		add(t, F.saved.color ? "color_on" : "color_off");
	if (F.saves != 0u && F.saved.mtp_macos != BOOT.settings.mtp_macos)
		add(t, F.saved.mtp_macos ? "mtp_macos" : "mtp_windows");
	for (unsigned n = 1; n <= 7; n++) {
		snprintf(want, sizeof want, "%u p%u  Hold %u for 5 s", n, n, n);
		if (appended(want))
			snprintf(t->t[t->n < MAXTOK ? t->n++ : 0], 24, "pending:%u", n);
		snprintf(want, sizeof want, "/yaffs2/content/dlc/p%u", n);
		if (removed(want))
			snprintf(t->t[t->n < MAXTOK ? t->n++ : 0], 24, "remove:%u", n);
	}
	if (F.leds_start)
		add(t, "keytest");
	if (F.volcal)
		add(t, "psoc:48 01");
	if (pack_before && M.armed != S15_ARM_PACK && F.n_removed == 0u &&
	    M.screen == S15_SCR_PACKS)
		add(t, "pack_cancel");
	if (appended("1 Format drive  Hold 1 for 5 s"))
		add(t, "format_confirm");
	if (F.format)
		add(t, "format");
	if (fpend_before && M.armed != S15_ARM_FORMAT && !F.format &&
	    M.screen == S15_SCR_FORMAT)
		add(t, "format_cancel");
	if (usb_called(S15_USB_SERIAL))
		add(t, "usb:serial");
	if (usb_called(S15_USB_MTP_VENDOR))
		add(t, "usb:MTP vendor");
	if (usb_called(S15_USB_MTP))
		add(t, "usb:MTP");
	if (F.os_write)
		add(t, "os_write");
	if (F.reset_w7) {
		add(t, "shutdown");
		add(t, "reset:W7");
	}
	if (F.scan)
		add(t, "scan_now");
	if (F.cont)
		add(t, "continue");
	if (F.once_checks)
		add(t, "once_check");
	if (F.once)
		add(t, "once");
	if (F.saves != 0u && F.saved.start_menu != BOOT.settings.start_menu)
		add(t, F.saved.start_menu ? "start_menu_on" : "start_menu_off");
}


/* S-15.011 */
static unsigned digit_of(unsigned key)
{
	if (key >= 0x06u && key <= 0x11u)
		return (key - 0x06u) % 4u + 1u;
	if (key >= 0x12u && key <= 0x15u)
		return key - 0x12u + 5u;
	return 0u;
}

static bool tok(const struct toks *t, const char *s)
{
	for (unsigned i = 0; i < t->n; i++)
		if (strcmp(t->t[i], s) == 0)
			return true;
	return false;
}

static bool tok_has(const struct toks *t, const char *s)
{
	for (unsigned i = 0; i < t->n; i++)
		if (strstr(t->t[i], s) != NULL)
			return true;
	return false;
}

/* Exactly one token, s. */
static bool only(const struct toks *t, const char *s)
{
	return t->n == 1u && tok(t, s);
}

/* The rules of S-15 and of the style guide's "Menus and keys"
 * (wiki/Screen-Style-Guide.md), each checked on every key event of every
 * state. */
enum rule {
	R_UP_NONE,            /* S-15.010: a key up has no effect */
	R_MAIN_INFO,          /* Help and digit 4 open Device Info */
	R_MAIN_SYSTEM,        /* Digit 5 opens System; 8 does nothing */
	R_BACK,               /* 8 leaves every screen below the main one */
	R_INFO_ONLY_8,        /* S-15.081: 8 back, 1 the second page */
	R_INFO2_ONLY_8,       /* S-15.082 */
	R_MAIN_SAFE,          /* S-15.031: nothing destructive, no setting */
	R_DEV_MODE,           /* S-15.170: the developer menu's setting */
	R_MTP_TARGET,         /* S-15.174 */
	R_COLOR,              /* S-15.173 */
	R_WHITE_KNOB,         /* S-15.015: the white knob confirms nothing */
	R_RESET_CONFIRMED,    /* S-15.050 */
	R_FROM_SYSTEM,        /* S-15.060: format and packs only from System */
	R_UPGRADE_EXIT,       /* S-15.103 */
	R_OPTION_KEYS,        /* S-15.140 */
	R_UPGRADE_GATE,       /* S-15.103: the USB key and digit 6 */
	R_FORMAT_KEYS,        /* S-15.071 */
	R_MTP_BUSY,           /* S-15.092, S-15.093 */
	R_OSW_RESULT,         /* S-15.092, S-15.094, S-15.190 */
	R_PACKS_BACK,         /* S-15.042: 8 draws System again */
	R_SCREEN,             /* S-15.014: always a screen that exists */
	R_PROMPT,             /* S-15.180 */
	R_CANCEL_ONLY,        /* S-15.015: during a hold another key only cancels */
	R_PACK_KEYS,          /* S-15.042 */
	R_RESULT_BACK,        /* a result's view: digit 8 alone */
	R_CONTINUE,           /* S-15.190: digit 6 on a written OS's result, the main screen's 7 */
	R_START_MENU,         /* S-15.175: Settings' digit 6 alone */
	R_HOLD,               /* S-15.015: the hold's time runs the action once */
	R_OS_ONCE,            /* S-15.194: the developer menu's digit 3 alone */
	R_N
};

static const char *const rule_names[R_N] = {
	"up_none", "main_info", "main_system", "back", "info_only_8", "info2_only_8",
	"main_safe", "dev_mode", "mtp_target", "color", "white_knob",
	"reset_confirmed", "from_system", "upgrade_exit", "option_keys",
	"upgrade_gate", "format_keys", "mtp_busy", "osw_result",
	"packs_back", "screen", "prompt", "cancel_only", "pack_keys",
	"result_back", "continue", "start_menu", "hold", "os_once",
};

static unsigned rule_bad[R_N], rule_seen[R_N];

/* One rule's check: counted always, printed for its first few failures. */
static void rule(enum rule r, bool ok, const char *state, unsigned key,
		 bool down, const struct toks *got)
{
	rule_seen[r]++;
	if (!ok && rule_bad[r]++ >= 3u) {
		check_count++;
		check_failures++;
		return;
	}
	CHECK(ok, "rule %s: %s key 0x%02x %s: screen %u, %u tokens%s%s",
	      rule_names[r], state, key, down ? "down" : "up", (unsigned)M.screen,
	      got->n, got->n ? " first " : "", got->n ? got->t[0] : "");
}

static void rules_prompt(void)
{
	for (unsigned key = 0; key < 72u; key++) {
		for (int edge = 0; edge < 2; edge++) {
			struct toks none = { .n = 0 };
			unsigned next;
			const char *effect;
			const char *want = "none";

			prompt_transition(key, edge == 0, &next, &effect);
			if (edge == 0 && digit_of(key) == 1u)
				want = "start_os";
			else if (edge == 0 && digit_of(key) == 2u)
				want = "held_mode";
			rule(R_PROMPT, strcmp(effect, want) == 0 &&
			     next == (strcmp(want, "held_mode") == 0 ?
				      (unsigned)S15_SCR_MAIN : SCR_PROMPT),
			     "screen=boot_prompt", key, edge == 0, &none);
		}
	}
}

/* S-15.015: a state's hold runs nothing before its time, its action once
 * at it, and nothing more after: not the time, not the key's release. */
static void rules_hold(const struct rstate *st)
{
	const unsigned hk = held_key(st);
	enum s15_screen before;
	struct toks got;
	char want[24], b[ROWLEN];
	bool ab, pb, fb, kb, ok;

	setup_state(st);
	before = M.screen;
	ab = M.armed == S15_ARM_RESET;
	pb = M.armed == S15_ARM_OPTION;
	fb = M.armed == S15_ARM_FORMAT;
	kb = M.armed == S15_ARM_PACK;
	tick(S15_CONFIRM_HOLD_MS - 1u);
	observe(&got, st, NO_KEY, before, ab, pb, fb, false, kb);
	rule(R_HOLD, got.n == 0u && M.screen == before && M.armed != S15_ARM_NONE &&
	     strcmp(last_row(b), "step-run:Release to cancel\t99%\t1 s") == 0,
	     st->name, hk, true, &got);
	tick(1u);
	observe(&got, st, NO_KEY, before, ab, pb, fb, false, kb);
	snprintf(want, sizeof want, "remove:%u", st->pending);
	ok = st->screen == S15_SCR_SYSTEM ? tok(&got, "factory_reset") :
	     st->screen == S15_SCR_PACKS ? tok(&got, want) :
	     st->screen == S15_SCR_FORMAT ? tok(&got, "format") :
	     only(&got, "option_on");
	rule(R_HOLD, ok && M.armed == S15_ARM_NONE, st->name, hk, true, &got);
	clear_records();
	n_app = 0;
	before = M.screen;
	tick(S15_CONFIRM_HOLD_MS);
	ev(hk, false);
	observe(&got, st, hk, before, false, false, false, false, false);
	rule(R_HOLD, got.n == 0u && M.screen == before, st->name, hk, false, &got);
}

/* The order a line was logged in, or MAXLINES when it was not. */
static unsigned app_at(const char *s)
{
	for (unsigned i = 0; i < n_app; i++)
		if (strcmp(app[i], s) == 0)
			return i;
	return MAXLINES;
}

/*
 * S-15.191, S-12.062: on a mount restored from a checkpoint, the scan
 * before the session's first write, under its line, when a write screen
 * opens: before the disk or the MTP drive is offered, before a factory
 * reset's or a pack's removal, at a host's begin. Once only; never for a
 * format or a settings change; never on a scanned mount.
 */
static void test_files_scan(void)
{
	struct s15_buf b;

	/* Install firmware: the scan, then the screen as always, then the disk */
	reset_fake(); default_boot(); BOOT.files_way = S15_FILES_RESTORED; start();
	press(0x06);
	CHECK(F.files_scans == 1u && F.files_line && F.files_at < F.disk_at &&
	      appended(s15_checking_files), "S-15.191 Install firmware: the scan first");
	CHECK(M.screen == S15_SCR_UPGRADE, "S-15.191 then the upgrade screen");
	expect_screen("upgrade_disk");
	press(0x15);
	press(0x06);
	CHECK(F.files_scans == 1u, "S-12.062 once per session");

	/* on a scanned mount nothing is checked, and the screen is the same */
	reset_fake(); default_boot(); BOOT.files_way = S15_FILES_NO_CKPT; start();
	press(0x06);
	CHECK(F.files_scans == 0u && !appended(s15_checking_files),
	      "S-12.062 a scanned mount: no scan");
	expect_screen("upgrade_disk");
	reset_fake(); default_boot(); start();
	press(0x06);
	CHECK(F.files_scans == 0u, "S-12.062 no mount reported: no scan");

	/* a boot into the upgrade screen, the MIDI form too */
	reset_fake(); default_boot(); BOOT.files_way = S15_FILES_RESTORED; BOOT.reason = 40;
	start();
	CHECK(F.files_scans == 1u && F.files_line && M.screen == S15_SCR_UPGRADE,
	      "S-15.191 the MIDI form opens after the scan");
	expect_screen("upgrade_midi");

	/* MTP: the scan before the drive's USB set */
	reset_fake(); default_boot(); BOOT.files_way = S15_FILES_RESTORED; start();
	press(0x08);
	clear_records();
	press(0x07);
	CHECK(F.files_scans == 1u && F.files_line && F.usb_at_files == 0u &&
	      (usb_called(S15_USB_MTP) || usb_called(S15_USB_MTP_VENDOR)),
	      "S-15.191 MTP: the scan before the drive");
	expect_screen("factory_mtp");

	/* the factory reset: the scan, then the removals */
	reset_fake(); default_boot(); BOOT.files_way = S15_FILES_RESTORED; start();
	press(0x12);
	hold(0x14);
	CHECK(F.files_scans == 1u && F.files_line && F.removed_at_files == 0u &&
	      F.n_removed == 3u && app_at(s15_checking_files) < app_at("Removing user data..."),
	      "S-15.191 factory reset: the scan before the removals");

	/* a pack's removal */
	reset_fake(); default_boot(); BOOT.files_way = S15_FILES_RESTORED;
	F.dlc = true; F.npacks = 1;
	F.pack_names[0] = "bass_pack"; F.manifests[0] = "{\"name\": \"Bass\"}";
	start();
	press(0x12);
	press(0x08);
	hold(0x06);
	CHECK(F.files_scans == 1u && F.files_line && F.removed_at_files == 0u &&
	      removed("/yaffs2/content/dlc/bass_pack"), "S-15.191 a pack's removal");

	/* a host's begin: the scan while the host waits for its reply */
	reset_fake(); default_boot(); BOOT.files_way = S15_FILES_RESTORED; start();
	s15_up_transfer_begin(&M, 1000u);
	CHECK(F.files_scans == 1u && F.files_line && M.screen == S15_SCR_UPGRADE,
	      "S-15.191 a host's begin");
	reset_fake(); default_boot(); BOOT.files_way = S15_FILES_RESTORED; start();
	s15_check_files(&M);
	s15_check_files(&M);
	CHECK(F.files_scans == 1u, "S-15.191 a host's own write: once");

	/* no scan for a format (it erases), a settings change, or the OS write */
	reset_fake(); default_boot(); BOOT.files_way = S15_FILES_RESTORED; start();
	press(0x12);
	press(0x12);
	hold(0x06);
	CHECK(F.format == 1u && F.files_scans == 0u, "S-12.062 a format: no scan");
	reset_fake(); default_boot(); BOOT.files_way = S15_FILES_RESTORED; start();
	press(0x07);
	press(0x08);
	CHECK(F.saves == 1u && F.files_scans == 0u, "S-12.062 a setting saved: no scan");
	reset_fake(); default_boot(); BOOT.files_way = S15_FILES_RESTORED; start();
	press(0x08);
	press(0x12);
	CHECK(F.os_write == 1u && F.files_scans == 0u, "S-12.062 the OS write: no scan");

	/* a slave that stopped answering: no scan is asked of it */
	reset_fake(); default_boot(); BOOT.files_way = S15_FILES_RESTORED; F.lost = true;
	start();
	press(0x06);
	CHECK(F.files_scans == 0u, "S-15.191 the slave lost: no scan");

	/* S-15.192: Device Info 2's Files row */
	reset_fake(); default_boot(); BOOT.files_way = S15_FILES_RESTORED;
	BOOT.files_objects = 1007u;
	start();
	press(0x09);
	press(0x06);
	expect_screen("info2_files");
	s15_files_text(&b, S15_FILES_CKPT_STALE, 9u);
	CHECK(strcmp(b.s, "Scanned (checkpoint stale)") == 0, "S-15.192 stale");
	s15_files_text(&b, S15_FILES_NO_CKPT, 9u);
	CHECK(strcmp(b.s, "Scanned (no checkpoint)") == 0, "S-15.192 none");
	s15_files_text(&b, S15_FILES_CKPT_UNREADABLE, 9u);
	CHECK(strcmp(b.s, "Scanned (checkpoint unreadable)") == 0, "S-15.192 unreadable");
	s15_files_text(&b, S15_FILES_CKPT_INVALID, 9u);
	CHECK(strcmp(b.s, "Scanned (checkpoint not valid)") == 0, "S-15.192 not valid");
	s15_files_text(&b, S15_FILES_FOR_WRITE, 9u);
	CHECK(strcmp(b.s, "Scanned (before a write)") == 0, "S-15.192 for a write");
	s15_files_text(&b, 99u, 9u);
	CHECK(strcmp(b.s, "Not mounted") == 0, "S-15.192 other");
}

static void test_rules(void)
{
	unsigned osw_rows = 0;

	memset(rule_bad, 0, sizeof rule_bad);
	memset(rule_seen, 0, sizeof rule_seen);
	for (size_t s = 0; s < sizeof rstates / sizeof rstates[0]; s++) {
		const struct rstate *st = &rstates[s];

		if (st->screen == SCR_PROMPT) {
			rules_prompt();
			continue;
		}
		if (held_key(st) != NO_KEY)
			rules_hold(st);
		for (unsigned key = 0; key < 72u; key++) {
			for (int edge = 0; edge < 2; edge++) {
				bool down = edge == 0;
				unsigned d = digit_of(key);
				enum s15_screen before, next;
				bool ab, pb, fb, ob, kb, armed_st;
				struct toks got;
				const char *n = st->name;
				const unsigned hk = held_key(st);

				setup_state(st);
				before = M.screen;
				ab = M.armed == S15_ARM_RESET;
				pb = M.armed == S15_ARM_OPTION;
				fb = M.armed == S15_ARM_FORMAT;
				kb = M.armed == S15_ARM_PACK;
				armed_st = M.armed != S15_ARM_NONE;
				ob = M.osw != S15_OSW_VIEW_NONE;
				ev(key, down);
				observe(&got, st, key, before, ab, pb, fb, ob, kb);
				next = M.screen;

				rule(R_SCREEN, next < S15_SCR_NONE, n, key, down, &got);
				/* any other key down, or the held key's release,
				 * cancels; its repeated down and precomp do nothing */
				if (armed_st && ((down && key != hk && key != 0x17u) ||
						 (!down && key == hk)))
					rule(R_CANCEL_ONLY, next == before &&
					     M.armed == S15_ARM_NONE && got.n == 1u &&
					     (tok(&got, "disarm") || tok(&got, "pack_cancel") ||
					      tok(&got, "format_cancel") ||
					      tok(&got, "option_cancel")), n, key, down, &got);
				else if (armed_st && down)
					rule(R_CANCEL_ONLY, next == before && got.n == 0u &&
					     M.armed != S15_ARM_NONE, n, key, down, &got);
				if (before == S15_SCR_RESULT && down)
					rule(R_RESULT_BACK, d == 8u ? next == S15_SCR_SYSTEM &&
					     only(&got, "open") : next == S15_SCR_RESULT &&
					     got.n == 0u, n, key, down, &got);
				if (before == S15_SCR_PACKS && down && !armed_st) {
					bool arms = d >= 1u && d <= 2u &&
						    !(key >= 0x0au && key <= 0x0du);
					char want[24];

					snprintf(want, sizeof want, "pending:%u", d);
					rule(R_PACK_KEYS, arms ? only(&got, want) :
					     !tok_has(&got, "pending") && !tok_has(&got, "remove"),
					     n, key, down, &got);
				}
				if (!down && !(armed_st && key == hk))
					rule(R_UP_NONE, got.n == 0u && next == before,
					     n, key, down, &got);

				if (before == S15_SCR_MAIN) {
					if (down && (key == 0x00u || d == 4u))
						rule(R_MAIN_INFO, next == S15_SCR_INFO &&
						     only(&got, "open"), n, key, down, &got);
					if (down && key == 0x12u)
						rule(R_MAIN_SYSTEM, next == S15_SCR_SYSTEM &&
						     only(&got, "open"), n, key, down, &got);
					if (down && key == 0x15u)
						rule(R_MAIN_SYSTEM, next == S15_SCR_MAIN &&
						     got.n == 0u, n, key, down, &got);
					rule(R_MAIN_SAFE, !tok_has(&got, "factory_reset") &&
					     !tok(&got, "arm") && !tok_has(&got, "format") &&
					     !tok_has(&got, "remove") && !tok_has(&got, "option") &&
					     !tok_has(&got, "dev_mode") && !tok_has(&got, "color") &&
					     !tok_has(&got, "mtp_") && !tok_has(&got, "start_menu"),
					     n, key, down, &got);
					if (down && d == 6u)
						rule(R_CONTINUE, next == S15_SCR_MAIN &&
						     got.n == 0u, n, key, down, &got);
					/* S-15.031: digit 7 continues to the OS */
					if (down && d == 7u)
						rule(R_CONTINUE, next == S15_SCR_STARTING &&
						     tok(&got, "continue") && !tok_has(&got, "usb"),
						     n, key, down, &got);
					if (down && d == 3u)
						rule(R_DEV_MODE, next == (st->devmode ?
						     S15_SCR_FACTORY : S15_SCR_MAIN),
						     n, key, down, &got);
				}

				if (down && key == 0x15u && !armed_st) {
					bool ok = true;

					switch (before) {
					case S15_SCR_PACKS:
					case S15_SCR_FORMAT:
					case S15_SCR_RESULT:
						ok = next == S15_SCR_SYSTEM;
						break;
					case S15_SCR_INFO:
					case S15_SCR_SYSTEM:
					case S15_SCR_SETTINGS:
						ok = next == S15_SCR_MAIN;
						break;
					case S15_SCR_INFO2:
						ok = next == S15_SCR_INFO;
						break;
					case S15_SCR_FACTORY:
						if (st->mtp != MTP_OFF)
							ok = next == S15_SCR_FACTORY &&
							     only(&got, "usb:serial");
						else if (st->osw_done || st->osw_written)
							ok = next == S15_SCR_FACTORY &&
							     only(&got, "redraw");
						else
							ok = next == S15_SCR_MAIN;
						break;
					case S15_SCR_MAIN:      /* R_MAIN_SYSTEM */
					case S15_SCR_UPGRADE:   /* R_UPGRADE_EXIT */
					case S15_SCR_STARTING:
					case S15_SCR_NONE:
					default:
						break;
					}
					rule(R_BACK, ok, n, key, down, &got);
				}

				if (before == S15_SCR_INFO && down)
					rule(R_INFO_ONLY_8, next == (key == 0x15u ?
					     S15_SCR_MAIN : d == 1u ? S15_SCR_INFO2 :
					     S15_SCR_INFO) && (next == before ? got.n == 0u :
					     only(&got, "open")), n, key, down, &got);
				if (before == S15_SCR_INFO2 && down)
					rule(R_INFO2_ONLY_8, next == (key == 0x15u ?
					     S15_SCR_INFO : S15_SCR_INFO2) &&
					     (next == before ? got.n == 0u : only(&got, "open")),
					     n, key, down, &got);

				/* The kept settings: each only by its own digit on
				 * Settings, turned the other way */
				if (before == S15_SCR_SETTINGS && down && d == 1u && !armed_st)
					rule(R_DEV_MODE, tok(&got, st->devmode ?
					     "dev_mode_off" : "dev_mode_on"), n, key, down, &got);
				if (tok_has(&got, "dev_mode_"))
					rule(R_DEV_MODE, before == S15_SCR_SETTINGS && down &&
					     d == 1u, n, key, down, &got);
				if (before == S15_SCR_SETTINGS && down && d == 5u && !armed_st)
					rule(R_MTP_TARGET, tok(&got, st->macos ?
					     "mtp_windows" : "mtp_macos"), n, key, down, &got);
				if (tok_has(&got, "mtp_"))
					rule(R_MTP_TARGET, before == S15_SCR_SETTINGS && down &&
					     d == 5u, n, key, down, &got);
				if (before == S15_SCR_FACTORY && st->mtp == MTP_OFF &&
				    !st->osw_done && !st->osw_written && down) {
					if (d == 2u)
						rule(R_MTP_TARGET, only(&got, st->macos ?
						     "usb:MTP vendor" : "usb:MTP"), n, key, down, &got);
					if (d == 3u)
						rule(R_OS_ONCE, next == S15_SCR_FACTORY &&
						     only(&got, "once_check"), n, key, down, &got);
				}
				if (before == S15_SCR_SETTINGS && down && d == 3u && !armed_st)
					rule(R_COLOR, next == S15_SCR_SETTINGS &&
					     tok(&got, st->color ? "color_off" : "color_on"),
					     n, key, down, &got);
				if (tok_has(&got, "color_"))
					rule(R_COLOR, before == S15_SCR_SETTINGS && down &&
					     d == 3u, n, key, down, &got);
				if (before == S15_SCR_SETTINGS && down && d == 6u && !armed_st)
					rule(R_START_MENU, next == S15_SCR_SETTINGS &&
					     only(&got, "start_menu_on"), n, key, down, &got);
				if (tok_has(&got, "start_menu"))
					rule(R_START_MENU, before == S15_SCR_SETTINGS && down &&
					     d == 6u, n, key, down, &got);
				if (tok(&got, "once_check") || tok(&got, "once"))
					rule(R_OS_ONCE, down && d == 3u &&
					     before == S15_SCR_FACTORY && st->mtp == MTP_OFF &&
					     !st->osw_done && !st->osw_written,
					     n, key, down, &got);
				if (tok(&got, "continue"))
					rule(R_CONTINUE, down && ((d == 6u &&
					     before == S15_SCR_FACTORY && st->osw_written) ||
					     (d == 7u && before == S15_SCR_MAIN)),
					     n, key, down, &got);

				if (down && key == 0x0du) {
					rule(R_WHITE_KNOB, !tok(&got, "factory_reset") &&
					     !tok(&got, "option_on") && !tok_has(&got, "remove") &&
					     !tok(&got, "format"), n, key, down, &got);
					if (!armed_st && (before == S15_SCR_SYSTEM ||
							  before == S15_SCR_SETTINGS ||
							  before == S15_SCR_PACKS ||
							  before == S15_SCR_FORMAT))
						rule(R_WHITE_KNOB, got.n == 0u,
						     n, key, down, &got);
				}

				/* only a hold's time runs the reset, never a key */
				rule(R_RESET_CONFIRMED, !tok(&got, "factory_reset"),
				     n, key, down, &got);
				if (tok(&got, "arm"))
					rule(R_RESET_CONFIRMED, before == S15_SCR_SYSTEM &&
					     key == 0x14u, n, key, down, &got);

				if ((next == S15_SCR_FORMAT || next == S15_SCR_PACKS) &&
				    tok(&got, "open"))
					rule(R_FROM_SYSTEM, before == S15_SCR_SYSTEM,
					     n, key, down, &got);

				if (before == S15_SCR_UPGRADE) {
					bool can = !st->midi && st->idle &&
						   down && key == 0x15u;

					rule(R_UPGRADE_EXIT, next == (can ? S15_SCR_MAIN :
					     S15_SCR_UPGRADE), n, key, down, &got);
					if (key == 0x18u || key == 0x13u)
						rule(R_UPGRADE_GATE, next == S15_SCR_UPGRADE &&
						     got.n == 0u, n, key, down, &got);
				}

				rule(R_OPTION_KEYS, !tok(&got, "option_on"),
				     n, key, down, &got);
				if (tok(&got, "option_prompt"))
					rule(R_OPTION_KEYS, st->opt == OPT_OFF && down &&
					     before == S15_SCR_SETTINGS && d == 2u &&
					     key != 0x0bu, n, key, down, &got);
				if (before == S15_SCR_SETTINGS && st->opt == OPT_OFF &&
				    down && (key == 0x07u || key == 0x0fu))
					rule(R_OPTION_KEYS, only(&got, "option_prompt") &&
					     M.armed == S15_ARM_OPTION, n, key, down, &got);

				if (before == S15_SCR_FORMAT && down) {
					if (!st->fpend)
						rule(R_FORMAT_KEYS, !tok(&got, "format") &&
						     tok(&got, "format_confirm") ==
						     (key == 0x06u || key == 0x0eu),
						     n, key, down, &got);
					else
						rule(R_FORMAT_KEYS, !tok(&got, "format"),
						     n, key, down, &got);
					if (key == 0x0au)
						rule(R_FORMAT_KEYS, next != S15_SCR_UPGRADE,
						     n, key, down, &got);
				}

				if (before == S15_SCR_FACTORY && st->mtp != MTP_OFF && down)
					rule(R_MTP_BUSY, next == S15_SCR_FACTORY &&
					     (d == 8u ? only(&got, "usb:serial") : got.n == 0u),
					     n, key, down, &got);

				if ((st->osw_done || st->osw_written) && down) {
					const bool cont = d == 6u && st->osw_written;
					bool ok;

					if (d == 8u)
						ok = only(&got, "redraw");
					else if (cont)	/* S-15.190 */
						ok = tok(&got, "continue") && F.cont_view &&
						     !tok(&got, "shutdown");
					else
						ok = got.n == 0u;
					rule(R_OSW_RESULT, ok && next == (cont ? S15_SCR_STARTING :
									  S15_SCR_FACTORY),
					     n, key, down, &got);
					osw_rows++;
				}

				if (before == S15_SCR_PACKS && next == S15_SCR_SYSTEM)
					rule(R_PACKS_BACK, only(&got, "redraw") && d == 8u,
					     n, key, down, &got);
			}
		}
	}
	CHECK(osw_rows == 2u * 72u, "rule osw_result: %u key downs", osw_rows);
	for (unsigned r = 0; r < R_N; r++)
		CHECK(rule_seen[r] != 0u, "rule %s: checked at least once",
		      rule_names[r]);
	for (unsigned r = 0; r < R_N; r++)
		if (rule_bad[r] != 0u)
			printf("rule %s: %u failures\n", rule_names[r], rule_bad[r]);
}

/* ---- screens ------------------------------------------------------- */

/*
 * S-15.163: each failed start's notice on the main screen, its row on Device
 * Info, and the main screen again after it; the cases of screens.json's
 * `start`. Its lines take the reinstall line's place and fit the screen with
 * developer mode on.
 */
static void test_start_screens(void)
{
	const char *got[S15_ROWS];
	char buf[S15_ROWS][ROWLEN], want[ROWLEN];
	const size_t items = 6u;	/* the blank row, five items */

	for (size_t i = 0; i < S15V_N(s15v_starts); i++) {
		const struct s15v_start *c = &s15v_starts[i];
		size_t n;

		reset_fake(); default_boot();
		BOOT.start = c->start;
		BOOT.slave = c->slave;
		F.lost = c->start.lost;
		start();
		n = rows(got, buf);
		/* the notice in place of the blank row above digit 7 */
		CHECK(n == items + 3u && strcmp(got[0], "") == 0 &&
		      strcmp(got[items + 2u], "item:7 Continue to OS") == 0,
		      "S-15.163 %s: two lines under the items", c->name);
		if (n != items + 3u)
			continue;
		CHECK(strcmp(got[items], c->first) == 0,
		      "S-15.163 %s: %s", c->name, got[items]);
		CHECK(strcmp(got[items + 1u], c->main) == 0, "S-15.163 %s main: %s",
		      c->name, got[items + 1u]);
		press(0x09);
		n = rows(got, buf);
		CHECK(n == 9u, "S-15.163 %s: Device Info's nine rows", c->name);
		if (n != 9u)
			continue;
		snprintf(want, sizeof want, "kv:OS start\t%s", c->info);
		CHECK(strcmp(got[7], want) == 0, "S-15.163 %s info: %s", c->name, got[7]);
		CHECK(strlen(c->info) <= S15_START_INFO_MAX, "S-15.163 %s fits", c->name);
		snprintf(want, sizeof want, "kv:DSP2\t%s\tBackplane\t1.0.7", c->dsp2);
		CHECK(strcmp(got[4], want) == 0, "S-15.163 %s DSP2: %s", c->name, got[4]);
		CHECK(strcmp(got[8], "back:8 Back to Main Menu") == 0,
		      "S-15.163 %s: the back line last", c->name);
		press(0x15);
		n = rows(got, buf);
		CHECK(n == items + 3u && strcmp(got[items + 1u], c->main) == 0,
		      "S-15.163 %s: again on main", c->name);
	}
	/* The sample screens of screens.json, whole. */
	reset_fake(); default_boot();
	BOOT.start = s15v_starts[0].start;
	F.lost = true;
	start();
	expect_screen("main_start_lost");
	press(0x09);
	expect_screen("info_start_lost");
	/* A boot that attempted no start shows none; the reinstall line only
	 * with an encrypted image's start. */
	reset_fake(); default_boot(); start();
	CHECK(!appended("The OS could not be started."), "S-15.163 no notice");
	CHECK(!appended(s15_os_reinstall), "S-15.161 no line");
	/* A slave that stops answering after the start: DSP2 says so,
	 * System and the format menu warn, and a reset removes nothing. */
	reset_fake(); default_boot(); start();
	F.lost = true;
	press(0x09);
	expect_screen("info_lost");
	press(0x15);
	press(0x12);
	expect_screen("system_lost");
	CHECK(F.dir_checks == 0u, "no pack check without the slave");
	press(0x12);
	expect_screen("format_lost");
	press(0x15);
	hold(0x14);
	expect_screen("factory_reset_lost");
	CHECK(F.n_removed == 0u, "S-15.052 nothing removed");
}

static void test_screens(void)
{
	/* Variants */
	reset_fake(); default_boot(); start();
	expect_screen("main_plain");
	expect_title("main");
	CHECK(usb_called(S15_USB_MAIN), "S-15.032 the main screen's USB set");
	reset_fake(); default_boot(); F.dlc = true; start();
	expect_screen("main_plain");    /* packs change nothing */
	reset_fake(); default_boot(); BOOT.settings.dev_mode = false; start();
	expect_screen("main_devmode_off");
	reset_fake(); default_boot(); s15_settings_defaults(&BOOT.settings);
	start();
	expect_screen("main_devmode_off");	/* S-15.172: off by default */
	reset_fake(); default_boot(); BOOT.start.why = S15_START_ENCRYPTED;
	start();
	expect_screen("main_os_reinstall");
	CHECK(appended(s15v_os_reinstall_text), "S-15.161 the line is logged");
	reset_fake(); default_boot(); BOOT.start.why = S15_START_ENCRYPTED;
	BOOT.settings.dev_mode = false; start();
	expect_screen("main_os_reinstall_devmode_off");
	test_start_screens();
	/* S-11.080: an install a reset interrupted, on the main screen each
	 * time it opens and on the upgrade screen. */
	reset_fake(); default_boot(); BOOT.interrupted = true; start();
	expect_screen("main_interrupted");
	press(0x06);
	expect_screen("upgrade_disk_interrupted");
	press(0x15);
	expect_screen("main_interrupted");
	reset_fake(); default_boot(); BOOT.interrupted = true; BOOT.reason = 40;
	start();
	expect_screen("upgrade_midi_interrupted");
	/* Once a run has started the main screen is not opened again in
	 * that boot (S-15.103, S-15.115), so the notice cannot outlive an
	 * install that followed it. */
	reset_fake(); default_boot(); BOOT.interrupted = true; start();
	press(0x06);
	s15_up_run_start(&M, S15_RUN_FIRMWARE, "1.3.0", NULL, false);
	s15_up_run_end(&M, S15_OUT_DONE, 0);
	press(0x15);
	CHECK(M.screen == S15_SCR_UPGRADE, "S-15.115 no key leaves the outcome");

	/* S-15.170, S-15.140 */
	reset_fake(); default_boot(); start();
	clear_records();
	press(0x07);
	expect_screen("settings");
	expect_title("settings");
	CHECK(F.n_usb == 0u, "S-15.170 no USB set");
	press(0x0b);           /* digit 2 by a knob click: no hold (S-15.015) */
	expect_screen("settings");
	CHECK(M.armed == S15_ARM_NONE, "S-15.015 a knob click starts no hold");
	ev(0x0f, true);        /* digit 2 by a sound key, held */
	expect_screen("option_prompt");
	expect_title("option");
	tick(S15_CONFIRM_HOLD_MS - 1u);
	CHECK(F.option == -1, "S-15.015 not before 5 s");
	tick(1u);
	CHECK(F.option == 1 && F.n_usb == 0u, "S-15.140 on at 5 s, drawn again, no USB set");
	expect_screen("settings_option_on");
	ev(0x0f, false);
	CHECK(F.option == 1 && M.option_on, "S-15.015 the release after it does nothing");
	press(0x06);
	expect_screen("settings_devmode_off_option_on");
	CHECK(F.saves == 1u && !F.saved.dev_mode && !F.saved.color,
	      "S-15.172 off, saved with the color as it is");
	/* A key that cancels the prompt only cancels: digit 1 does not
	 * also turn developer mode off. */
	reset_fake(); default_boot(); start();
	press(0x07);
	ev(0x07, true);
	clear_records();
	press(0x06);
	CHECK(F.saves == 0u && M.dev_mode && F.option == -1 &&
	      M.armed == S15_ARM_NONE, "S-15.140 a cancelling key only cancels");
	expect_screen("settings");
	tick(S15_CONFIRM_HOLD_MS);
	ev(0x07, false);
	CHECK(F.option == -1 && M.screen == S15_SCR_SETTINGS,
	      "S-15.015 a canceled hold's key runs nothing");
	/* an early release turns nothing on, and so does a later time */
	ev(0x07, true);
	tick(S15_CONFIRM_HOLD_MS - 1u);
	ev(0x07, false);
	tick(S15_CONFIRM_HOLD_MS);
	CHECK(F.option == -1 && !M.option_on && M.armed == S15_ARM_NONE,
	      "S-15.015 released at 4999 ms: off");
	expect_screen("settings");
	/* the white knob turns nothing on */
	ev(0x07, true);
	press(0x0d);
	tick(S15_CONFIRM_HOLD_MS);
	CHECK(F.option == -1 && M.armed == S15_ARM_NONE, "S-15.015 the white knob only cancels");
	ev(0x07, false);
	reset_fake(); default_boot(); start();
	press(0x07);
	F.store_rc = S15_STORE_FAILED;
	press(0x06);
	expect_screen("settings_dev_not_saved");
	CHECK(!M.dev_mode, "S-15.172 changed for this start all the same");
	/* the settings are not on the drive: a refused volume saves them */
	reset_fake(); default_boot(); F.refused = true; start();
	press(0x07);
	press(0x06);
	expect_screen("settings_devmode_off");
	CHECK(F.saves == 1u && !F.saved.dev_mode, "S-15.172 saved with the volume refused");
	/* S-15.193: a host's key changes it for this start, saves nothing */
	reset_fake(); default_boot(); start();
	press(0x07);
	s15_menu_host_event(&M, 0x06, true);
	s15_menu_host_event(&M, 0x06, false);
	expect_screen("settings_host");
	CHECK(F.saves == 0u && !M.dev_mode && !M.host_event, "S-15.193 host key: not saved");
	press(0x06);
	CHECK(F.saves == 1u && F.saved.dev_mode && M.dev_mode, "S-15.193 the unit's key saves");

	/* S-15.173: digit 3 turns the color on and off, each change saved */
	reset_fake(); default_boot(); start();
	press(0x07);
	CHECK(!M.color, "S-15.173 in gray by default");
	press(0x08);
	expect_screen("settings_color_on");
	CHECK(M.color && F.saves == 1u && F.saved.color &&
	      F.saved.dev_mode, "S-15.173 on, saved");
	press(0x08);
	expect_screen("settings");
	CHECK(!M.color && F.saves == 2u && !F.saved.color, "S-15.173 off again");
	F.store_rc = S15_STORE_FAILED;
	press(0x08);
	CHECK(M.color && appended(s15_settings_failed),
	      "S-15.172 changed for this start, the line");
	F.store_rc = S15_STORE_UNUSABLE;
	press(0x08);
	CHECK(!M.color && F.saves == 4u && appended(s15_settings_unusable),
	      "S-15.172 refused: this start only, the line");
	/* host keys for color and the MTP target: this start only */
	F.store_rc = S15_STORE_SAVED;
	s15_menu_host_event(&M, 0x08, true);
	s15_menu_host_event(&M, 0x08, false);
	CHECK(M.color && F.saves == 4u && appended(s15_settings_host), "S-15.193 color");
	s15_menu_host_event(&M, 0x12, true);
	s15_menu_host_event(&M, 0x12, false);
	CHECK(!M.mtp_macos && F.saves == 4u && appended(s15_settings_host), "S-15.193 MTP");
	reset_fake(); default_boot(); BOOT.settings.color = true; start();
	press(0x07);
	expect_screen("settings_color_on");

	/* S-15.060, S-15.050 */
	reset_fake(); default_boot(); start();
	press(0x12);
	expect_screen("system");
	expect_title("system");
	ev(0x14, true);
	expect_screen("system_armed");
	tick(2000);            /* 2 of 5 s: the bar and the seconds left */
	expect_tail("hold_2s_tail");
	ev(0x14, false);
	expect_screen("system");
	reset_fake(); default_boot(); F.dlc = true; start();
	press(0x12);
	expect_screen("system_packs");
	ev(0x14, true);
	expect_screen("system_packs_armed");
	ev(0x14, false);

	/* S-15.042 */
	reset_fake(); default_boot(); F.dlc = true; F.npacks = 2;
	F.pack_names[0] = "bass_pack"; F.manifests[0] = "{\"name\": \"Bass\"}";
	F.pack_names[1] = "drums_v2";
	start();
	press(0x12);
	press(0x08);
	expect_screen("packs_two");
	expect_title("packs");
	press(0x0b);   /* a knob click starts no hold */
	expect_screen("packs_two");
	ev(0x0f, true);   /* digit 2 by a sound key */
	expect_screen("packs_two_pending_2");
	ev(0x0f, false);
	expect_screen("packs_two");
	reset_fake(); default_boot(); F.dlc = true; F.npacks = 7;
	for (unsigned i = 0; i < 7u; i++) {
		static const char *const names[] = { "P1", "P2", "P3", "P4",
						     "P5", "P6", "P7" };

		F.pack_names[i] = names[i];
	}
	start();
	press(0x12);
	press(0x10);   /* digit 3 by a sound key */
	expect_screen("packs_seven");
	ev(0x06, true);
	expect_screen("packs_seven_armed_1");
	ev(0x06, false);

	/* S-15.071 */
	reset_fake(); default_boot(); start();
	press(0x12);
	press(0x12);
	expect_screen("format");
	expect_title("format");
	ev(0x0e, true);
	expect_screen("format_armed");
	ev(0x0e, false);
	reset_fake(); default_boot(); F.refused = true; start();
	press(0x12);
	press(0x12);
	expect_screen("format_refused");
	ev(0x06, true);
	expect_screen("format_refused_pending");
	ev(0x06, false);
	reset_fake(); default_boot(); BOOT.version = "1.0.0"; start();
	press(0x00);
	expect_screen("info");
	expect_title("info");
	press(0x07);                /* S-15.081: 8 goes back, 1 opens page 2 */
	press(0x0d);
	press(0x12);
	CHECK(M.screen == S15_SCR_INFO, "S-15.081 other keys stay");
	press(0x15);
	expect_screen("main_plain");
	press(0x15);                /* S-15.031: no item, nothing to go back to */
	CHECK(M.screen == S15_SCR_MAIN, "S-15.031 8 does nothing on main");
	reset_fake(); default_boot(); BOOT.version = "1.0.0"; start();
	press(0x09);   /* S-15.031: digit 4 as the help key */
	expect_screen("info");
	reset_fake(); default_boot(); BOOT.version = "1.0.0"; start();
	press(0x0d);   /* the white knob is digit 4 too */
	expect_screen("info");
	reset_fake(); default_boot(); BOOT.provisioned = false;
	BOOT.version = "1.0.0"; BOOT.sw_version = "0.0.0"; start();
	press(0x00);
	expect_screen("info_unprovisioned");
	reset_fake(); default_boot(); BOOT.version = "1.0.0";
	BOOT.slave = S15_SLAVE_ABSENT; start();
	press(0x00);
	expect_screen("info_slave_absent");
	test_info2();
	test_files_scan();
	reset_fake(); default_boot(); start();     /* No dev mode */
	press(0x08);
	expect_screen("factory");
	expect_title("factory");
	press(0x07);
	expect_screen("factory_mtp");
	expect_title("factory_mtp");

	/* S-15.100 - S-15.102 */
	reset_fake(); default_boot(); start();
	press(0x06);
	expect_screen("upgrade_disk");
	expect_title("upgrade");
	CHECK(F.disk_shows_screen && !F.disk_shows_usb,
	      "S-15.100 the screen before the set, USB only once it is up");
	{
		struct s15_frame fr;

		s15_menu_frame(&M, &fr);
		CHECK((fr.status & S15_STATUS_USB) != 0u,
		      "S-15.100 USB once the set is up");
	}
	s15_up_acquired(&M);
	expect_screen("upgrade_disk");
	reset_fake(); default_boot(); BOOT.reason = 40; start();
	expect_screen("upgrade_midi");
	CHECK(usb_called(S15_USB_MAIN) && !usb_called(S15_USB_DISK),
	      "S-15.102 the main screen's set, no disk");

	/* S-15.052 */
	reset_fake(); default_boot(); start();
	press(0x12);
	hold(0x14);
	expect_screen("factory_reset_run");
	expect_title("freset");
	CHECK(removed("/yaffs2/user") && removed("/yaffs2/logs") &&
	      removed("/yaffs2/content/dlc") && F.erase_fat == 1u &&
	      M.screen == S15_SCR_RESULT, "S-15.052 removed and erased");
}

/* The status bar's words and their colors, as screens.json lists them. */
static void test_status(void)
{
	for (size_t i = 0; i < S15V_N(s15v_status); i++) {
		const struct s15v_status *v = &s15v_status[i];
		struct s15_frame f;
		char lit[40] = "";

		reset_fake(); default_boot();
		BOOT.running_known = v->running >= 0;
		BOOT.running_slot = v->running >= 0 ? (unsigned)v->running : 0u;
		if (v->usb)
			BOOT.reason = 30;     /* the RAM disk is offered */
		start();
		s15_menu_frame(&M, &f);
		strcat(lit, f.slot != S15_SLOT_UNKNOWN ? "dim:" : "unlit:");
		strcat(lit, f.slot == S15_SLOT_UNKNOWN ? "SLOT" :
			    f.slot ? "SLOT2" : "SLOT1");
		strcat(lit, (f.status & S15_STATUS_USB) ? " amber:USB" :
							    " unlit:USB");
		CHECK(strcmp(lit, v->words) == 0, "status %zu: '%s', want '%s'",
		      i, lit, v->words);
		CHECK(f.version != NULL && strcmp(f.version, BOOT.version) == 0,
		      "the version");
	}
}

/* S-15.180: the prompt's lines under the title Boot, logged (S-15.004);
 * what its keys choose, against boot_prompt_keys.json; and the control,
 * which ignores what was down when the screen opened, fails some case. */
static void test_boot_prompt(void)
{
	static const char *const names[] = { NULL, "start", "held" };
	struct s15_prompt_values v;
	unsigned control_wrong = 0;

	reset_fake(); default_boot();
	s14_log_init(&LOG, S14_MASTER, NULL, &rec_link, NULL, NULL);
	s15_menu_init(&M, &BOOT, &env, &LOG);
	v.running = 0;
	v.selector = 0;
	v.verified = 0x01;
	v.sp_kept = true;
	v.entry_sp = 0x080000E8u;
	v.debug = false;
	s15_prompt_show(&M, &v);
	expect_screen("boot_prompt");
	expect_title("boot_prompt");
	CHECK(M.screen == S15_SCR_NONE, "S-15.013 no menu screen before held mode");
	CHECK(appended("Running  Slot 1  Selector  Slot 1") &&
	      appended("1 Start the OS") && appended("Debug key  Not held"),
	      "S-15.004 the prompt's lines are logged, tabs as two spaces");
	CHECK(F.n_usb == 0u, "S-15.180 no USB set");
	press(0x06);
	CHECK(M.screen == S15_SCR_NONE && F.n_usb == 0u,
	      "S-15.013 the menus act on no key before the first screen");
	v.running = -1;
	v.selector = 1;
	v.verified = 0xFF;
	v.sp_kept = false;
	v.debug = true;
	s15_prompt_show(&M, &v);
	expect_screen("boot_prompt_unknown");
	expect_title("boot_prompt");

	for (size_t i = 0; i < S15V_N(s15v_prompt_keys); i++) {
		const struct s15v_prompt_keys *c = &s15v_prompt_keys[i];
		struct s15_prompt p, control;
		uint8_t known[9] = { 0 };
		const char *got, *ctl;

		for (size_t k = 0; k < c->n_down; k++)
			known[c->down_at_open[k] >> 3] |= (uint8_t)(1u << (c->down_at_open[k] & 7u));
		s15_prompt_open(&p, known);
		s15_prompt_open(&control, NULL);
		for (size_t k = 0; k < c->n; k++) {
			s15_prompt_key(&p, c->events[k][0], c->events[k][1] != 0u);
			s15_prompt_key(&control, c->events[k][0],
				       c->events[k][1] != 0u);
		}
		got = names[s15_prompt_choice(&p)];
		ctl = names[s15_prompt_choice(&control)];
		CHECK((got == NULL && c->expect == NULL) ||
		      (got != NULL && c->expect != NULL && strcmp(got, c->expect) == 0),
		      "S-15.180 keys %s: chose %s, want %s", c->name,
		      got ? got : "nothing", c->expect ? c->expect : "nothing");
		if (c->n_down != 0u && (ctl == NULL) != (c->expect == NULL))
			control_wrong++;
		else if (c->n_down != 0u && ctl != NULL && strcmp(ctl, c->expect) != 0)
			control_wrong++;
	}
	CHECK(control_wrong != 0u, "S-15.180 the control fails a case");

	/* The known state's bits, every one of them, are down when shown. */
	{
		struct s15_prompt p;
		static const uint8_t known[9] = { 0xC0, 0, 0, 0, 0, 0, 0, 0, 0 };

		s15_prompt_open(&p, known);		/* 0x06 and 0x07 down */
		s15_prompt_key(&p, 0x06, true);
		s15_prompt_key(&p, 0x07, true);
		CHECK(s15_prompt_choice(&p) == S15_PROMPT_WAIT, "S-15.180 both stale");
		s15_prompt_key(&p, 0x07, false);
		s15_prompt_key(&p, 0x07, true);
		CHECK(s15_prompt_choice(&p) == S15_PROMPT_HELD, "S-15.180 up and down");
		s15_prompt_key(&p, 0x06, false);
		s15_prompt_key(&p, 0x06, true);
		CHECK(s15_prompt_choice(&p) == S15_PROMPT_HELD, "S-15.180 the first digit wins");
		s15_prompt_open(&p, NULL);
		s15_prompt_key(&p, 72, true);		/* S-15.014: no such key */
		CHECK(s15_prompt_choice(&p) == S15_PROMPT_WAIT, "S-15.014 no fault");
	}
}

/* ---- behavior ------------------------------------------------------- */

static void test_keys(void)
{
	/* S-15.013: keys down across the start act only after up and down. */
	reset_fake(); default_boot();
	s14_log_init(&LOG, S14_MASTER, NULL, &rec_link, NULL, NULL);
	s15_menu_init(&M, &BOOT, &env, &LOG);
	ev(0x12, true);
	ev(0x0d, true);
	ev(0x06, true);
	s15_menu_start(&M);
	ev(0x0d, true);     /* a repeated down: nothing */
	CHECK(M.screen == S15_SCR_MAIN && F.n_removed == 0u,
	      "S-15.013 no system menu, no upgrade screen");
	ev(0x06, false);
	CHECK(M.screen == S15_SCR_MAIN, "S-15.013 the up does nothing");
	ev(0x06, true);
	CHECK(M.screen == S15_SCR_UPGRADE, "S-15.013 up then down acts");

	/* S-15.013: a key the events never told of, down as S-07 knows it
	 * (held from power-on): a down of it is no press; once seen released,
	 * its press acts. */
	reset_fake(); default_boot();
	s15_menu_init(&M, &BOOT, &env, &LOG);
	{
		uint8_t known[(S15_NKEYS + 7u) / 8u] = { 0 };

		known[0x0d >> 3] = (uint8_t)(1u << (0x0d & 7u));
		s15_menu_keys_down(&M, known);
	}
	s15_menu_start(&M);
	ev(0x0d, true);
	CHECK(M.screen == S15_SCR_MAIN, "S-15.013 a known held key is no press");
	ev(0x0d, false);
	CHECK(M.screen == S15_SCR_MAIN, "S-15.013 its release does nothing");
	ev(0x0d, true);
	CHECK(M.screen == S15_SCR_INFO, "S-15.013 released, then pressed: it acts");

	/* S-15.012, S-15.031: shift held changes nothing on the main
	 * screen: 8 does nothing, 7 continues to the OS as without it. */
	reset_fake(); default_boot(); start();
	ev(0x37, true);
	press(0x15);
	CHECK(M.screen == S15_SCR_MAIN, "S-15.031 shift + 8: nothing");
	press(0x14);
	CHECK(M.screen == S15_SCR_STARTING && F.cont == 1u && F.n_removed == 0u,
	      "S-15.031 shift + 7: continues, nothing removed");
	ev(0x37, false);
	reset_fake(); default_boot(); start();

	/* S-15.014, S-16.012: keys out of range do nothing. */
	press(200);
	CHECK(M.screen == S15_SCR_MAIN, "S-15.014 unknown key");

	/* Digit 3 needs the setting. */
	reset_fake(); default_boot();
	BOOT.settings.dev_mode = false; start();
	press(0x08);
	CHECK(M.screen == S15_SCR_MAIN, "off: 3 does nothing");
	press(0x07);
	press(0x06);
	CHECK(F.saves == 1u && F.saved.dev_mode && M.dev_mode,
	      "S-15.172 on, saved");
	press(0x15);
	expect_screen("main_plain");
	press(0x08);
	CHECK(M.screen == S15_SCR_FACTORY, "on: 3 opens it");

	/* S-15.015, S-15.050: 7 held; another key's up keeps the hold, its
	 * own early release cancels it, any other key down only cancels. */
	reset_fake(); default_boot(); start();
	press(0x12);
	ev(0x14, true);
	ev(0x06, false);
	CHECK(M.armed == S15_ARM_RESET, "S-15.015 another key's up keeps the hold");
	expect_screen("system_armed");
	tick(S15_CONFIRM_HOLD_MS - 1u);
	ev(0x14, false);
	CHECK(M.armed == S15_ARM_NONE && F.n_removed == 0u,
	      "S-15.015 released at 4999 ms: canceled");
	expect_screen("system");
	tick(S15_CONFIRM_HOLD_MS);
	CHECK(F.n_removed == 0u, "S-15.015 nothing runs later");
	ev(0x14, true);
	press(0x0b);   /* digit 2: the calibration reset, were it not held */
	CHECK(M.armed == S15_ARM_NONE && F.volcal == 0u && F.n_removed == 0u &&
	      M.screen == S15_SCR_SYSTEM, "S-15.015 a cancelling key only cancels");
	expect_screen("system");
	tick(S15_CONFIRM_HOLD_MS);
	ev(0x14, false);
	CHECK(F.n_removed == 0u, "S-15.015 a canceled hold's key runs nothing");
	ev(0x14, true);
	ev(0x37, true);   /* shift is another key too */
	CHECK(M.armed == S15_ARM_NONE, "S-15.015 shift cancels");
	ev(0x37, false);
	ev(0x14, false);
	/* the white knob confirms nothing: unheld it does nothing, held it
	 * only cancels */
	reset_fake(); default_boot(); start();
	press(0x12);
	press(0x0d);
	CHECK(M.screen == S15_SCR_SYSTEM && F.n_removed == 0u,
	      "S-15.051 the white knob does nothing");
	ev(0x14, true);
	press(0x0d);
	tick(S15_CONFIRM_HOLD_MS);
	CHECK(M.armed == S15_ARM_NONE && F.n_removed == 0u,
	      "S-15.015 the white knob only cancels");
	ev(0x14, false);
	/* precomp (S-07.032) is not acted on: the hold goes on, and at 5 s
	 * the reset runs once; the time and the release do nothing more */
	ev(0x14, true);
	ev(0x17, true);
	ev(0x17, false);
	tick(S15_CONFIRM_HOLD_MS - 1u);
	CHECK(F.n_removed == 0u && M.armed == S15_ARM_RESET, "S-15.015 precomp: held on");
	tick(1u);
	CHECK(F.n_removed == 3u && M.screen == S15_SCR_RESULT, "S-15.015 at 5 s the reset");
	tick(S15_CONFIRM_HOLD_MS);
	ev(0x14, false);
	ev(0x14, true);
	tick(S15_CONFIRM_HOLD_MS);
	CHECK(F.n_removed == 3u && F.erase_fat == 1u && M.screen == S15_SCR_RESULT,
	      "S-15.015 once: no repeat, the release and the next down do nothing");
}

static void test_packs(void)
{
	char last[ROWLEN];
	size_t n;
	const char *s;

	/* S-15.043: removal, the last pack, then the system menu's keys. */
	reset_fake(); default_boot(); F.dlc = true; F.npacks = 1;
	F.pack_names[0] = "bass_pack"; F.manifests[0] = "{\"name\": \"Bass\"}";
	start();
	press(0x12);
	press(0x08);
	hold(0x06);
	expect_tail("pack_removed_last");
	CHECK(removed("/yaffs2/content/dlc/bass_pack") &&
	      removed("/yaffs2/content/dlc") && M.screen == S15_SCR_RESULT,
	      "S-15.043 the pack and content/dlc removed");
	clear_records();
	press(0x07);   /* digit 2: no calibration reset here */
	press(0x14);
	CHECK(M.screen == S15_SCR_RESULT && F.volcal == 0u,
	      "S-15.043 the result's keys: only 8");
	press(0x15);
	CHECK(M.screen == S15_SCR_SYSTEM && !M.packs_item,
	      "S-15.043 then 8 opens the system menu");

	/* A failure is one line naming the pack, the list again, no fault. */
	reset_fake(); default_boot(); F.dlc = true; F.npacks = 2;
	F.pack_names[0] = "a"; F.pack_names[1] = "b";
	F.fail_remove = "/yaffs2/content/dlc/a";
	start();
	press(0x12);
	press(0x08);
	hold(0x06);
	s = s15_console_row(&M.con, M.con.count - 1u, &n);
	snprintf(last, sizeof last, "%.*s", (int)n, s);
	CHECK(M.screen == S15_SCR_PACKS && strstr(last, "a") != NULL &&
	      strstr(last, "remove") != NULL && strncmp(s, "Could not", 9) == 0 &&
	      s15_console_row_style(&M.con, M.con.count - 1u) == S15_ST_ERROR,
	      "S-15.043 failure line: '%s'", last);

	/* A pack directory whose name is the longest allowed is removed. */
	{
		static char longname[S15_NAME_MAX + 1u];
		char want[300];

		memset(longname, 'p', S15_NAME_MAX);
		reset_fake(); default_boot(); F.dlc = true; F.npacks = 2;
		F.pack_names[0] = longname; F.pack_names[1] = "b";
		start();
		press(0x12);
		press(0x08);
		hold(0x06);
		snprintf(want, sizeof want, "/yaffs2/content/dlc/%s", longname);
		CHECK(removed(want) && F.npacks == 1u,
		      "S-15.043 a %u-character name is removed", S15_NAME_MAX);
	}

	/* No packs: the system menu again without item 3. */
	reset_fake(); default_boot(); F.dlc = true; start();
	press(0x12);
	clear_records();
	press(0x08);
	CHECK(M.screen == S15_SCR_SYSTEM && F.n_usb == 0u && !M.packs_item,
	      "no packs: drawn again without item 3");
	expect_screen("system");

	/* S-15.042: 8 back draws the system menu; nothing pending after. */
	reset_fake(); default_boot(); F.dlc = true; F.npacks = 2;
	F.pack_names[0] = "a"; F.pack_names[1] = "b"; start();
	press(0x12);
	press(0x08);
	ev(0x06, true);
	clear_records();
	press(0x15);   /* cancels */
	ev(0x06, false);
	press(0x15);
	CHECK(M.screen == S15_SCR_SYSTEM && F.dir_checks == 0u,
	      "S-15.042 redraw");
	expect_screen("system_packs");
	press(0x08);
	press(0x0a);
	CHECK(M.armed == S15_ARM_NONE, "S-15.015 a knob click starts no hold");
	ev(0x06, true);
	CHECK(F.n_removed == 0u && M.armed == S15_ARM_PACK && M.pending == 1u,
	      "S-15.042 its digit starts the hold");
	ev(0x06, false);
	CHECK(F.n_removed == 0u && M.armed == S15_ARM_NONE,
	      "S-15.015 its early release only cancels");
	ev(0x07, true);
	press(0x06);
	CHECK(F.n_removed == 0u && M.armed == S15_ARM_NONE &&
	      M.screen == S15_SCR_PACKS, "S-15.042 another pack's digit only cancels");
	tick(S15_CONFIRM_HOLD_MS);
	ev(0x07, false);
	CHECK(F.n_removed == 0u, "S-15.015 nothing after the cancel");
	ev(0x07, true);
	press(0x0d);
	tick(S15_CONFIRM_HOLD_MS);
	ev(0x07, false);
	CHECK(F.n_removed == 0u, "S-15.015 the white knob only cancels");
	ev(0x07, true);
	tick(S15_CONFIRM_HOLD_MS);
	CHECK(F.n_removed == 1u && removed("/yaffs2/content/dlc/b"),
	      "S-15.043 held 5 s: removed");
	tick(S15_CONFIRM_HOLD_MS);
	ev(0x07, false);
	CHECK(F.n_removed == 1u, "S-15.015 once");
}

static void test_reset_and_system(void)
{
	/* S-15.052 on a refused volume. */
	reset_fake(); default_boot(); F.refused = true; start();
	press(0x12);
	hold(0x14);
	CHECK(F.n_removed == 0u && appended(s15_reset_unreadable) &&
	      !appended("User data removed. Please restart your OP-XY."),
	      "S-15.052 refused volume");
	CHECK(F.erase_fat == 1u, "S-15.052 the OS FAT erase made all the same");

	/* S-15.052: a failed removal goes on, one line instead of the OK. */
	reset_fake(); default_boot(); F.fail_remove = "/yaffs2/logs"; start();
	press(0x12);
	hold(0x14);
	CHECK(F.n_removed == 3u && F.erase_fat == 1u &&
	      appended(s15_reset_incomplete) &&
	      !appended("User data removed. Please restart your OP-XY.") &&
	      M.screen == S15_SCR_RESULT, "S-15.052 a removal fails");
	clear_records();
	press(0x07);
	CHECK(M.screen == S15_SCR_RESULT && F.volcal == 0u,
	      "S-15.052 the result: no system menu key acts");

	/* S-15.061 */
	reset_fake(); default_boot(); start();
	press(0x12);
	clear_records();
	press(0x07);
	CHECK(F.volcal == 1u && M.screen == S15_SCR_MAIN &&
	      usb_called(S15_USB_MAIN), "S-15.061 0x48 {01}, then main");

	/* S-15.062 - S-15.065 */
	reset_fake(); default_boot(); start();
	press(0x12);
	press(0x06);
	CHECK(M.keytest && F.leds_start == 1u, "S-15.062 key test started");
	press(0x15);
	CHECK(M.keytest && M.screen == S15_SCR_SYSTEM, "S-15.060 no key acts");
	ev(0x37, true);
	CHECK(!M.shift, "S-15.064 shift state not updated in the test");
	ev(0x09, true);
	CHECK(M.keytest, "S-15.065 0x09 does not end it");
	ev(0x09, false);
	ev(0x15, true);
	CHECK(!M.keytest && F.leds_end == 1u && M.screen == S15_SCR_SYSTEM,
	      "S-15.065 shift + 0x15 ends it");
	expect_screen("system");

	/* S-15.012: shift held to end the test reads as it
	 * was when the test began, until its next up and down. */
	ev(0x15, false);
	press(0x15);
	press(0x06);
	CHECK(M.screen == S15_SCR_UPGRADE && F.sysinfo == 0u,
	      "S-15.012 shift as before the test: no system information");
	ev(0x37, false);
	ev(0x37, true);
	CHECK(M.shift, "S-15.012 shift again after it");
}

static void test_format(void)
{
	/* S-15.071 - S-15.073: reached from the system menu, dev mode or
	 * not. */
	reset_fake(); default_boot(); start();
	press(0x12);
	press(0x12);
	ev(0x0a, true);
	tick(S15_CONFIRM_HOLD_MS);
	ev(0x0a, false);
	CHECK(F.format == 0u && M.armed == S15_ARM_NONE,
	      "S-15.071 0x0A held starts no hold");
	ev(0x06, true);
	CHECK(F.format == 0u && M.armed == S15_ARM_FORMAT,
	      "S-15.071 the key starts the hold");
	tick(S15_CONFIRM_HOLD_MS - 1u);
	CHECK(F.format == 0u, "S-15.015 not before 5 s");
	tick(1u);
	ev(0x06, false);
	CHECK(F.format == 1u && M.screen == S15_SCR_UPGRADE &&
	      appended("Formatting the drive. Please wait...") && appended("Done."),
	      "S-15.072 format then the upgrade screen");

	reset_fake(); default_boot(); F.refused = true; start();
	press(0x12);
	press(0x12);
	ev(0x06, true);
	CHECK(F.format == 0u && M.armed == S15_ARM_FORMAT,
	      "S-15.073 its key down erases nothing");
	press(0x15);
	CHECK(M.armed == S15_ARM_NONE && F.format == 0u &&
	      M.screen == S15_SCR_FORMAT, "S-15.073 8 only cancels");
	ev(0x06, false);
	ev(0x06, true);
	press(0x0e);
	CHECK(M.armed == S15_ARM_NONE && F.format == 0u,
	      "S-15.073 the other format key only cancels");
	ev(0x06, false);
	ev(0x06, true);
	press(0x0d);
	tick(S15_CONFIRM_HOLD_MS);
	ev(0x06, false);
	CHECK(F.format == 0u && M.armed == S15_ARM_NONE,
	      "S-15.015 the white knob only cancels");
	hold(0x0e);
	CHECK(F.format == 1u, "S-15.073 held 5 s: erased");

	/* S-15.072: a failed format shows its error line, the format menu
	 * stays current, and the remount alone sets the refused state */
	reset_fake(); default_boot(); start();
	press(0x12);
	press(0x12);
	F.format_fails = true;
	F.refused_after = true;
	hold(0x06);
	CHECK(F.format == 1u && M.screen == S15_SCR_FORMAT &&
	      appended("The drive could not be formatted") && !appended("Done."),
	      "S-15.072 a failed format stays in the format menu");
	ev(0x06, true);
	CHECK(F.format == 1u && M.armed == S15_ARM_FORMAT,
	      "S-15.072 the next format asks first");
	expect_screen("format_refused_pending");
	tick(S15_CONFIRM_HOLD_MS);
	ev(0x06, false);
	CHECK(F.format == 2u, "S-15.073 then held 5 s: erased");

	/* a confirmed format of a refused volume the remount reads: no
	 * longer refused */
	reset_fake(); default_boot(); F.refused = true; start();
	press(0x12);
	press(0x12);
	F.format_fails = true;
	hold(0x06);
	CHECK(F.format == 1u && M.screen == S15_SCR_FORMAT && !M.refused,
	      "S-15.072 the remount reads it: not refused");
	hold(0x06);
	CHECK(F.format == 2u, "S-15.073 held and confirmed as before");

	reset_fake(); default_boot(); start();
	press(0x12);
	press(0x12);
	clear_records();
	press(0x15);
	CHECK(M.screen == S15_SCR_SYSTEM && F.dir_checks == 1u,
	      "S-15.071 8 opens the system menu");
}

static void test_factory(void)
{
	/* S-15.093: 8 turns MTP off; 2 does nothing while it is on. */
	reset_fake(); default_boot(); start();
	press(0x08);
	clear_records();
	press(0x07);
	press(0x07);
	CHECK(F.n_usb == 1u && M.mtp == S15_MTP_VENDOR,
	      "S-15.093 2 does nothing while MTP is on");
	press(0x15);
	CHECK(M.screen == S15_SCR_FACTORY && M.mtp == S15_MTP_OFF,
	      "S-15.093 8 back to the developer menu");
	expect_screen("factory");
	CHECK(F.n_usb == 2u && F.usb[0] == S15_USB_MTP_VENDOR &&
	      F.usb[1] == S15_USB_SERIAL, "S-15.174 macOS, the vendor interface");
	press(0x08);
	CHECK(F.n_usb == 2u && M.mtp == S15_MTP_OFF && F.once_checks == 1u,
	      "S-15.194 3 is the file start, no MTP");
	/* S-15.174: Windows, the standard interface, and the item says so */
	reset_fake(); default_boot(); BOOT.settings.mtp_macos = false; start();
	press(0x08);
	expect_screen("factory_windows");
	clear_records();
	press(0x07);
	press(0x15);
	CHECK(F.n_usb == 2u && F.usb[0] == S15_USB_MTP &&
	      F.usb[1] == S15_USB_SERIAL, "S-15.174 Windows, the standard one");
	/* S-15.174: Settings' 5 turns it, and saves */
	reset_fake(); default_boot(); start();
	press(0x07);
	press(0x12);
	expect_screen("settings_mtp_windows");
	CHECK(!M.mtp_macos && F.saves == 1u && !F.saved.mtp_macos,
	      "S-15.174 Windows, saved");
	press(0x12);
	CHECK(M.mtp_macos && F.saves == 2u && F.saved.mtp_macos,
	      "S-15.174 macOS again");
	reset_fake(); default_boot(); start();
	press(0x08);
	/* S-15.092: no factory or calibration reset here; digits 1 and 4
	 * are no items. */
	press(0x06);
	press(0x0d);
	press(0x09);
	CHECK(F.n_removed == 0u && F.volcal == 0u &&
	      M.screen == S15_SCR_FACTORY,
	      "S-15.092 1, 0x0D and 0x09 do nothing");

	/* S-15.094, S-16.123: every result leaves the menu usable; 8 goes
	 * back to the developer menu, whose 6 restarts. */
	for (int r = 0; r < 3; r++) {
		reset_fake(); default_boot();
		BOOT.start.why = S15_START_ENCRYPTED; start();
		F.osw = (enum s15_os_write)r;
		press(0x08);
		press(0x12);
		CHECK(appended("Looking for /os.ldr...") && F.os_write == 1u,
		      "S-15.094 reads /os.ldr");
		CHECK(F.osw_in.result == S15_OSW_FAILED && F.osw_in.why == S15_OSW_WHY_NONE &&
		      F.osw_in.emmc == S15_OSW_EMMC_UNTOUCHED && !F.osw_in.select0_failed,
		      "S-15.094 the end comes in as a failure naming nothing");
		CHECK(appended(s15_osw_written) == (r == S15_OSW_OK),
		      "S-15.094 ok line only on success (%d)", r);
		CHECK(appended(s15_os_ldr_unreadable) == (r == S15_OSW_UNREADABLE) &&
		      appended(s15_os_write_failed) == (r == S15_OSW_FAILED),
		      "S-15.094 the error line of the result (%d)", r);
		CHECK(!M.busy && M.screen == S15_SCR_FACTORY && M.osw == S15_OSW_VIEW_DONE,
		      "S-16.123 not busy after the write");
		n_app = 0;
		press(0x15);
		CHECK(M.screen == S15_SCR_FACTORY && M.osw == S15_OSW_VIEW_NONE,
		      "S-15.094 8 goes back to the developer menu");
		expect_screen("factory");
		expect_title("factory");
		press(0x15);
		CHECK(M.screen == S15_SCR_MAIN, "S-15.092 8 opens main");
		/* S-15.161: the line stays until the next reset. */
		CHECK(appended(s15_os_reinstall), "S-15.161 still shown");
		press(0x08);
		press(0x13);
		CHECK(F.reset_w7 == 1u, "S-15.095 digit 6 resets");
		CHECK(F.restart_line, "the restart is on the screen first");
	}
	/* The restart's line under the developer menu. */
	reset_fake(); default_boot(); start();
	press(0x08);
	press(0x13);
	expect_screen("factory_restarting");
	CHECK(F.reset_w7 == 1u && F.restart_line, "S-15.095 Restarting, then the reset");
}

/* S-15.105: the host releases the disk and the delay passes. */
static void scan_by_release(void)
{
	s15_up_released(&M, 100000);
	s15_up_tick(&M, 102000);
}

/* The newest rows of the console are the last of these lines (at most
 * S15_ROWS of them). */
static void expect_last_lines(const char *name, const char *const *lines)
{
	const char *got[S15_ROWS];
	char buf[S15_ROWS][ROWLEN];
	size_t n = rows(got, buf), k = 0, from;

	while (lines[k] != NULL)
		k++;
	from = k > S15_ROWS ? k - S15_ROWS : 0u;
	CHECK(n == k - from, "%s: %zu rows, want %zu", name, n, k - from);
	for (size_t i = 0; i < n && from + i < k; i++)
		CHECK(strcmp(got[i], lines[from + i]) == 0, "%s row %zu: '%s', want '%s'",
		      name, i, got[i], lines[from + i]);
}

/* A vectors line as the log has it (S-15.004): the text after its style,
 * a tab as two spaces. */
static const char *logged(const char *line, char *out)
{
	const char *t = strchr(line, ':');
	size_t n = 0;

	if (t == NULL || strncmp(line, "   ", 3) == 0)
		t = line;
	else
		t++;
	for (; *t != '\0' && n + 2u < ROWLEN; t++) {
		if (*t == '\t') {
			out[n++] = ' ';
			out[n++] = ' ';
		} else {
			out[n++] = *t;
		}
	}
	out[n] = '\0';
	return out;
}

/* S-15.094: the OS write's screen for every case of os_write.json. */
static void test_os_write(void)
{
	char row[ROWLEN], want[ROWLEN];

	for (size_t i = 0; i < S15V_N(s15v_osws); i++) {
		const struct s15v_osw *c = &s15v_osws[i];
		uint32_t before;

		reset_fake(); default_boot(); start();
		F.osw_case = c;
		press(0x08);
		clear_records();
		n_app = 0;
		press(0x12);
		CHECK(F.os_write == 1u, "%s: one write", c->name);
		expect_last_lines(c->name, c->lines);
		expect_title("factory_oswrite");
		CHECK(!M.busy && M.osw == S15_OSW_VIEW_DONE && M.screen == S15_SCR_FACTORY,
		      "%s: done, not busy", c->name);
		/* S-15.004: the lines appended are logged, the rows by their
		 * labels; what replaces a row is not. */
		for (size_t k = 0; c->lines[k] != NULL; k++) {
			const char *l = c->lines[k];

			if (strncmp(l, "step-", 5) == 0) {
				snprintf(want, sizeof want, "%s", strchr(l, ':') + 1);
				*strchr(want, '\t') = '\0';
				CHECK(appended(want) && !appended(logged(l, row)),
				      "%s: row '%s' logged by its label", c->name, want);
			} else {
				CHECK(appended(logged(l, want)), "%s: '%s' logged", c->name, want);
			}
		}
		/* A progress row, replaced only when p changes. */
		if (F.osw_progress[0] != '\0') {
			CHECK(strcmp(F.osw_progress, "step-run:Write\t50%") == 0 && F.osw_same,
			      "%s: progress row '%s'", c->name, F.osw_progress);
			CHECK(!appended("Write  50%"), "%s: progress not logged", c->name);
		}
		/* S-15.092: digit 8 acts once the back line is shown, and
		 * digit 6 after a success. */
		before = s15_menu_changes(&M);
		for (unsigned key = 0; key < S15_NKEYS; key++)
			if (s15_digit(key) != 8u && s15_digit(key) != 6u)
				press(key);
		if (strcmp(c->result, "ok") != 0)
			press(0x13);
		CHECK(F.os_write == 1u && F.reset_w7 == 0u && F.n_usb == 0u && F.volcal == 0u &&
		      M.osw == S15_OSW_VIEW_DONE && s15_menu_changes(&M) == before,
		      "%s: other keys do nothing", c->name);
		press(0x15);
		CHECK(M.screen == S15_SCR_FACTORY && M.osw == S15_OSW_VIEW_NONE,
		      "%s: 8 back to the developer menu", c->name);
		expect_screen("factory");
		expect_title("factory");
		/* S-15.190: digit 6 on the screen of a success continues to
		 * the OS, also when select 0 failed (the restart resets the
		 * slave, S-05.033). */
		if (strcmp(c->result, "ok") == 0) {
			press(0x12);
			press(0x13);
			CHECK(F.cont == 1u && F.cont_view && F.reset_w7 == 0u &&
			      M.screen == S15_SCR_STARTING, "%s: 6 continues", c->name);
			expect_screen("continue_restarting");
		}
	}
	/* A board with no continue_os: digit 6 there is the restart W7. */
	reset_fake(); default_boot(); F.osw = S15_OSW_OK;
	s14_log_init(&LOG, S14_MASTER, NULL, &rec_link, NULL, NULL);
	s15_menu_init(&M, &BOOT, &env_bare, &LOG);
	s15_menu_start(&M);
	press(0x08);
	press(0x12);
	CHECK(M.osw_written && M.osw == S15_OSW_VIEW_DONE, "bare: the write succeeds");
	press(0x13);
	CHECK(F.reset_w7 == 1u && F.restart_line && F.cont == 0u &&
	      M.screen == S15_SCR_FACTORY, "bare: 6 restarts (S-15.095)");

	/* Reports outside the OS write change nothing. */
	reset_fake(); default_boot(); start();
	{
		uint32_t before = s15_menu_changes(&M);

		s15_osw_step(&M, S15_OSW_STEP_READ);
		s15_osw_progress(&M, 1, 2);
		s15_osw_done(&M, 1, 2);
		CHECK(s15_menu_changes(&M) == before && M.screen == S15_SCR_MAIN,
		      "S-15.094 no report outside the write");
		expect_screen("main_plain");
	}
}

static void test_upgrade(void)
{
	char row[ROWLEN];
	size_t n;
	const char *s;

	/* S-15.100, S-16.161: shift writes the system information before
	 * the disk is offered. */
	reset_fake(); default_boot(); start();
	ev(0x37, true);
	press(0x06);
	CHECK(F.sysinfo == 1u && F.sysinfo_at < F.disk_at,
	      "S-15.100 system information first");

	/* S-15.105: 2 s after the release, not before 5 s. */
	reset_fake(); default_boot(); BOOT.reason = 30; start();
	s15_up_released(&M, 1000);
	s15_up_tick(&M, 3500);
	CHECK(F.scan == 0u, "S-15.105 not before 5 s");
	s15_up_tick(&M, 5000);
	CHECK(F.scan == 1u && M.activity == S15_ACT_SCAN,
	      "S-15.105 at 5 s");
	s = s15_console_row(&M.con, 0, &n);
	snprintf(row, sizeof row, "%.*s", (int)n, s);
	CHECK(strcmp(row, s15v_prepare_header + 5) == 0 &&
	      M.con.count == 1u, "preparation lines");
	s15_up_not_found(&M);
	CHECK(strcmp(row_back(row, 2), s15v_not_found) == 0 && back_last(),
	      "not found, then the back line");
	expect_screen("upgrade_not_found");

	reset_fake(); default_boot(); BOOT.reason = 30; start();
	s15_up_released(&M, 10000);
	s15_up_acquired(&M);
	s15_up_tick(&M, 20000);
	CHECK(F.scan == 0u, "S-15.105 acquired again: no scan");
	s15_up_released(&M, 20000);
	s15_up_tick(&M, 21999);
	CHECK(F.scan == 0u, "S-15.105 not before 2 s");
	s15_up_tick(&M, 22000);
	CHECK(F.scan == 1u, "S-15.105 2 s after the release");

	/* S-15.103: digit 8 goes back from the idle disk form, withdrawing
	 * the disk and dropping a scan due after a release. */
	reset_fake(); default_boot(); start();
	press(0x06);
	s15_up_released(&M, 20000);
	clear_records();
	press(0x15);
	CHECK(M.screen == S15_SCR_MAIN && F.n_usb == 1u &&
	      F.usb[0] == S15_USB_MAIN, "S-15.103 8 back, the disk withdrawn");
	expect_screen("main_plain");
	s15_up_tick(&M, 30000);
	CHECK(F.scan == 0u && M.screen == S15_SCR_MAIN,
	      "S-15.103 no scan after going back");
	reset_fake(); default_boot(); BOOT.reason = 40; start();
	press(0x15);
	CHECK(M.screen == S15_SCR_UPGRADE, "S-15.103 the MIDI form stays");

	/* S-15.103: neither the USB key nor digit 6 does anything here; only
	 * a release starts a scan. */
	reset_fake(); default_boot(); BOOT.reason = 30; start();
	press(0x18);
	press(0x13);
	CHECK(F.scan == 0u && M.armed != S15_ARM_OPTION && !M.option_on &&
	      M.screen == S15_SCR_UPGRADE, "S-15.103 USB key and 6: no effect");
	scan_by_release();
	CHECK(F.scan == 1u, "S-15.105 a release scans");
	press(0x15);
	CHECK(M.screen == S15_SCR_UPGRADE, "S-15.103 no way back during a scan");
	s15_up_refused(&M, "signature");
	CHECK(appended("   Reason: signature") &&
	      strcmp(row_back(row, 2), "dim:   Reason: signature") == 0 &&
	      back_last(), "refusal reason");

	/* S-15.103: idle again after a refusal, and after no package. */
	scan_by_release();
	CHECK(F.scan == 2u, "S-15.103 a release scans again");
	s15_up_not_found(&M);
	scan_by_release();
	CHECK(F.scan == 3u, "S-15.103 idle after no package");

	/* S-15.103: a refusal at checks 16 - 17 ends idleness. */
	s15_up_extract_refused(&M, "content", false);
	press(0x18);
	press(0x13);
	press(0x15);
	CHECK(F.scan == 3u && M.armed != S15_ARM_OPTION && M.screen == S15_SCR_UPGRADE,
	      "S-15.103 not idle after 16 - 17");
	s15_up_released(&M, 30000);
	s15_up_tick(&M, 40000);
	CHECK(F.scan == 3u, "S-15.103 no scan after the zip was read");
	CHECK(!s15_up_disk_offerable(&M), "S-15.115 read from the disk");

	/* S-15.103: back again once a scan has found nothing. */
	reset_fake(); default_boot(); BOOT.reason = 30; start();
	scan_by_release();
	s15_up_not_found(&M);
	press(0x15);
	CHECK(M.screen == S15_SCR_MAIN, "S-15.103 8 back after no package");

	/* S-15.104: a host begin from the main screen. */
	reset_fake(); default_boot(); start();
	clear_records();
	s15_up_transfer_begin(&M, 2000);
	CHECK(M.screen == S15_SCR_UPGRADE && F.n_usb == 0u,
	      "S-15.104 no USB set");
	CHECK(strcmp(last_row(row), s15v_prepare_header) == 0,
	      "no Transfer row before the transfer starts: '%s'", row);
	s15_up_transfer_started(&M);
	n_app = 0;
	for (uint32_t c = 1; c <= 45; c++)
		s15_up_transfer_chunk(&M, c, c * 40u, c == 45u);
	CHECK(strcmp(last_row(row), "step-run:Transfer\t90%") == 0,
	      "transfer: '%s'", row);
	CHECK(n_app == 0u, "S-15.004 progress lines not logged");

	/* A run: header, steps, results, outcome. */
	reset_fake(); default_boot(); BOOT.reason = 30; start();
	scan_by_release();
	s15_up_phase(&M, S15_PH_DECRYPT);
	n_log_frames = 0;
	s15_up_progress(&M, 0, 3);
	s15_up_progress(&M, 0, 3);
	s15_up_progress(&M, 1, 3);
	CHECK(n_log_frames == 0u, "progress not logged");
	CHECK(strcmp(last_row(row), "step-run:Decrypting\t33%") == 0,
	      "decrypting: '%s'", row);
	s15_up_run_start(&M, S15_RUN_FIRMWARE, "1.3.0", NULL, true);
	CHECK(appended("Upgrading to 1.3.0") &&
	      appended(s15_stock_line) && M.con.count == 4u,
	      "header");
	s15_up_step_start(&M, S15_STEP_OS);
	s15_up_step_progress(&M, S15_STEP_OS, 5, 10);
	CHECK(strcmp(last_row(row), "step-run:Main DSP\t50%") == 0,
	      "the step's row: '%s'", row);
	s15_up_step_result(&M, S15_STEP_OS, S15_RES_OK, "1.3.0", false, NULL);
	CHECK(strcmp(last_row(row), "step-ok:Main DSP\tInstalled 1.3.0") == 0,
	      "the row replaced: '%s'", row);
	s15_up_step_start(&M, S15_STEP_BOOTLOADER);
	s15_up_step_result(&M, S15_STEP_BOOTLOADER, S15_RES_FAILURE, NULL, true, NULL);
	CHECK(appended("Operation 10 failed") && appended(s15_reset_needed),
	      "refused start");
	s = s15_console_row(&M.con, M.con.count - 3u, &n);
	CHECK(strncmp(s, "Bootloader\tFailed", n) == 0 &&
	      s15_console_row_style(&M.con, M.con.count - 3u) == S15_ST_STEP_FAIL,
	      "the failed step's row");
	/* S-11.069: the note's lines under a Nordic step's failure; none
	 * without one */
	{
		const char *note[S15_NOTE_LINES];
		enum s15_style nst[S15_NOTE_LINES];
		unsigned before;

		CHECK(s15_note_lines(note, nst, S15_NOTE_NO_APP) == 2u, "two lines");
		s15_up_step_start(&M, S15_STEP_BLE);
		s15_up_step_result(&M, S15_STEP_BLE, S15_RES_FAILURE, NULL, false,
				   "chunk 2/3 refused");
		s15_up_step_note(&M, S15_NOTE_NO_APP);
		CHECK(appended("Operation 7 failed") && appended(note[0]) &&
		      appended(note[1]), "no application");
		s = s15_console_row(&M.con, M.con.count - 1u, &n);
		CHECK(strncmp(s, note[1], n) == 0 && n == strlen(note[1]) &&
		      s15_console_row_style(&M.con, M.con.count - 2u) == S15_ST_ERROR,
		      "the note last");
		before = M.con.count;
		s15_up_step_note(&M, S15_NOTE_NONE);
		CHECK(M.con.count == before, "no note");
	}
	s15_up_run_end(&M, S15_OUT_FAILED, 0);
	CHECK(appended("Upgrade failed."), "failed");
	press(0x18);
	press(0x13);
	s15_up_released(&M, 100000);
	s15_up_tick(&M, 200000);
	CHECK(F.scan == 1u && M.screen == S15_SCR_UPGRADE && M.armed != S15_ARM_OPTION,
	      "S-15.115 no further scan, no option, no exit");
	CHECK(!s15_up_disk_offerable(&M), "S-15.115 no disk after a disk run");
	s15_up_transfer_begin(&M, 2000);
	s15_up_transfer_started(&M);
	s15_up_run_end(&M, S15_OUT_FAILED, 0);
	CHECK(!s15_up_disk_offerable(&M), "S-15.115 nor after a later host run");

	/* S-15.103: a begin while the host holds
	 * the disk ends the scans, its package refused at checks 1 - 15. */
	reset_fake(); default_boot(); BOOT.reason = 30; start();
	s15_up_acquired(&M);
	s15_up_transfer_begin(&M, 2000);
	s15_up_transfer_started(&M);
	s15_up_released(&M, 6000);              /* the transfer's withdrawal */
	s15_up_refused(&M, "header");
	s15_up_tick(&M, 9000);
	s15_up_released(&M, 12000);             /* the host's next eject */
	s15_up_tick(&M, 20000);
	press(0x18);
	CHECK(F.scan == 0u, "S-15.103 no scan after a begin");
	CHECK(s15_up_disk_offerable(&M), "S-15.115 offered after a refusal");
	press(0x15);
	CHECK(M.screen == S15_SCR_MAIN,
	      "S-15.103 back once a refused begin left it idle");

	/* S-15.103: a begin refused for its product word, too; a MIDI begin's
	 * length gets no Transfer row when no transfer starts. */
	reset_fake(); default_boot(); start();
	s15_up_transfer_begin(&M, 2000);
	s15_up_refused(&M, "PRODUCT");
	CHECK(M.con.count == 5u &&
	      strcmp(row_back(row, 2), "dim:   Reason: PRODUCT") == 0 &&
	      back_last(), "refused begin: no Transfer row: '%s'", row);
	press(0x18);
	s15_up_released(&M, 10000);
	s15_up_tick(&M, 20000);
	CHECK(M.screen == S15_SCR_UPGRADE && F.scan == 0u,
	      "S-15.103 no scan after a refused begin");

	/* (S-10c.071, S-10c.088): a begin whose transfer cannot
	 * start: the error line once, no not-found or reason line, no
	 * Transfer row; the screen idle again (S-15.103). */
	reset_fake(); default_boot(); start();
	s15_up_transfer_begin(&M, 2000);
	s15_up_no_transfer(&M);
	CHECK(M.con.count == 4u &&
	      strcmp(row_back(row, 2), s15v_no_transfer) == 0 && back_last(),
	      "no transfer: '%s'", row);
	press(0x18);
	s15_up_released(&M, 10000);
	s15_up_tick(&M, 20000);
	CHECK(M.screen == S15_SCR_UPGRADE && F.scan == 0u,
	      "S-15.103 no scan after a begin that could not start");

	/* After a host-link run the disk is
	 * offered again (S-15.115), but no scan runs (S-15.103). */
	reset_fake(); default_boot(); BOOT.reason = 30; start();
	s15_up_transfer_begin(&M, 2000);
	s15_up_phase(&M, S15_PH_DECRYPT);
	s15_up_run_start(&M, S15_RUN_FIRMWARE, "1.3.0", NULL, false);
	s15_up_run_end(&M, S15_OUT_FAILED, 0);
	CHECK(s15_up_disk_offerable(&M), "S-15.115 offered after a host run");
	s15_up_released(&M, 30000);
	s15_up_tick(&M, 40000);
	press(0x18);
	CHECK(F.scan == 0u, "S-15.103 no scan after a host run");

	/* A content-only install's other steps show nothing. */
	reset_fake(); default_boot(); BOOT.reason = 30; start();
	s15_up_run_start(&M, S15_RUN_CONTENT, NULL, "Bass", false);
	n = M.con.count;
	s15_up_step_start(&M, S15_STEP_OS);
	s15_up_step_progress(&M, S15_STEP_OS, 1, 2);
	s15_up_step_result(&M, S15_STEP_OS, S15_RES_NOT_FOUND, NULL, false, NULL);
	CHECK(M.con.count == n, "content-only: no line for os");
	s15_up_run_end(&M, S15_OUT_DONE, 0);
	CHECK(appended("Done!"), "done");
	expect_tail("upgrade_done_tail");

	/* A step that stops the run says why on its row. */
	reset_fake(); default_boot(); BOOT.reason = 30; start();
	s15_up_run_start(&M, S15_RUN_FIRMWARE, "1.3.0", NULL, false);
	s15_up_step_start(&M, S15_STEP_BLE_BOOTLOADER);
	s15_up_step_result(&M, S15_STEP_BLE_BOOTLOADER, S15_RES_FAILURE, NULL,
			   false, "no slot answer");
	s = s15_console_row(&M.con, M.con.count - 2u, &n);
	snprintf(row, sizeof row, "%.*s", (int)n, s);
	CHECK(strcmp(row, "BLE bootloader\tFailed: no slot answer") == 0,
	      "the cause on the failed row: '%s'", row);
	CHECK(appended("Operation 8 failed"), "and the operation's line");
}

/*
 * S-15.116: a protected component's confirmation on the upgrade screen,
 * under its own title: the component, the package's version and what it
 * risks; the bar's row replaced as the hold goes on, and not logged. The
 * menus' keys do nothing while it is shown (the confirmation's keys are
 * s15_confirm's).
 */
static void test_confirm_screen(void)
{
	const struct s15_confirm_info boot = { "1.0.12",
						"1.0.10", 1 };
	const struct s15_confirm_info unknown = { "1.0.3", NULL, -1 };
	static const unsigned keys[] = { 0x15, 0x06, 0x0d, 0x18, 0x13, 0x0a };
	char before[S15_ROWS][ROWLEN], after[S15_ROWS][ROWLEN];
	const char *got[S15_ROWS];
	unsigned nlogged, scans;
	size_t n;

	reset_fake(); default_boot(); BOOT.reason = 30; start();
	scan_by_release();
	s15_up_phase(&M, S15_PH_EXTRACT_FIRMWARE);
	s15_up_progress(&M, 1, 1);
	s15_up_confirm(&M, &boot);
	expect_screen("upgrade_confirm_bootloader");
	expect_title("confirm");
	CHECK(appended("Install the bootloader?"), "S-15.004 its lines logged");
	nlogged = n_app;
	s15_up_confirm_bar(&M, 40, 52);
	expect_screen("upgrade_confirm_bootloader_held");
	CHECK(n_app == nlogged, "S-15.004 the bar's row not logged");
	n = rows(got, before);
	clear_records();
	scans = F.scan;
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		press(keys[i]);
	CHECK(rows(got, after) == n, "S-15.116 the menus' keys add no row");
	for (size_t i = 0; i < n; i++)
		CHECK(strcmp(before[i], after[i]) == 0,
		      "S-15.116 the menus' keys change no row: '%s'", after[i]);
	CHECK(M.screen == S15_SCR_UPGRADE && F.scan == scans && F.n_usb == 0u &&
	      M.armed != S15_ARM_OPTION && !M.option_on,
	      "S-15.116 no way back, no scan, no option");
	expect_title("confirm");

	reset_fake(); default_boot(); BOOT.reason = 30; start();
	scan_by_release();
	s15_up_confirm(&M, &unknown);
	CHECK(appended("The other slot is replaced. Recovery remains."),
	      "no selector read: the other slot");
	/* the run's own header follows on the upgrade screen's title */
	s15_up_run_start(&M, S15_RUN_FIRMWARE, "1.3.0", NULL, false);
	expect_title("upgrade");
	CHECK(appended("Upgrading to 1.3.0"), "the run's header after it");
}

/*
 * The end of a run: a completed one names the steps it skipped before its
 * outcome line; a failed one does not. Skipped rows give their reason.
 */
static void test_run_end_skipped(void)
{
	char row[ROWLEN];

	reset_fake(); default_boot(); BOOT.reason = 30; start();
	scan_by_release();
	s15_up_run_start(&M, S15_RUN_FIRMWARE, "1.3.0", NULL, false);
	s15_up_step_start(&M, S15_STEP_OS);
	s15_up_step_result(&M, S15_STEP_OS, S15_RES_OK, "1.3.0", false, NULL);
	s15_up_step_start(&M, S15_STEP_BLE);
	s15_up_step_result(&M, S15_STEP_BLE, S15_RES_SKIPPED_OFF, NULL, false, NULL);
	s15_up_step_start(&M, S15_STEP_KEYBOARD);
	s15_up_step_result(&M, S15_STEP_KEYBOARD, S15_RES_SKIPPED_OFF, NULL,
			   false, NULL);
	s15_up_run_end(&M, S15_OUT_DONE,
		       (1u << S15_STEP_BLE) | (1u << S15_STEP_KEYBOARD));
	expect_tail("upgrade_done_not_updated");

	reset_fake(); default_boot(); BOOT.reason = 30; start();
	scan_by_release();
	s15_up_run_start(&M, S15_RUN_FIRMWARE, "1.3.0", NULL, false);
	s15_up_step_start(&M, S15_STEP_BOOTLOADER);
	s15_up_step_result(&M, S15_STEP_BOOTLOADER, S15_RES_SKIPPED_UNCONFIRMED,
			   NULL, false, NULL);
	CHECK(strcmp(last_row(row), "step-skip:Bootloader\tSkipped: not confirmed") == 0,
	      "the reason on the row: '%s'", row);
	s15_up_run_end(&M, S15_OUT_NOTHING,
		       1u << S15_STEP_BOOTLOADER);
	expect_tail("upgrade_nothing_not_updated");

	reset_fake(); default_boot(); BOOT.reason = 30; start();
	scan_by_release();
	s15_up_run_start(&M, S15_RUN_FIRMWARE, "1.3.0", NULL, false);
	s15_up_step_start(&M, S15_STEP_BLE);
	s15_up_step_result(&M, S15_STEP_BLE, S15_RES_SKIPPED_OFF, NULL, false, NULL);
	s15_up_step_start(&M, S15_STEP_OS);
	s15_up_step_result(&M, S15_STEP_OS, S15_RES_FAILURE, NULL, false, NULL);
	s15_up_run_end(&M, S15_OUT_FAILED, 1u << S15_STEP_BLE);
	CHECK(appended("Upgrade failed.") && !appended("Not updated: BLE"),
	      "a failed run names no part not updated");
	for (unsigned i = 0; i < n_app; i++)
		CHECK(strncmp(app[i], "Not updated", 11) != 0,
		      "failed run: '%s'", app[i]);
}

/* ---- S-15.190, S-05.033: continue to the OS ------------------------ */

/* S-15.194: Start /os.ldr once, digit 3 on the developer menu. */
static void test_os_once(void)
{
	static const struct {
		enum s15_once rc;
		enum s15_osw_why why;
		uint32_t value, limit;
		const char *screen;
	} refused[] = {
		{ S15_ONCE_FILE, S15_OSW_WHY_MISSING, 2u, 0u, "factory_once_missing" },
		{ S15_ONCE_FILE, S15_OSW_WHY_TOO_LARGE, 8388609u, 8388608u,
		  "factory_once_too_large" },
		{ S15_ONCE_SLAVE, S15_OSW_WHY_NONE, 0u, 0u, "factory_once_slave" },
	};
	uint32_t before;

	/* The file there: the restarting view whole, then the board's W12. */
	reset_fake(); default_boot(); start();
	press(0x08);
	expect_screen("factory");
	F.once_rc = S15_ONCE_STARTED;
	clear_records();
	press(0x08);
	CHECK(F.once_checks == 1u && F.once == 1u && F.once_view &&
	      M.screen == S15_SCR_STARTING && F.reset_w7 == 0u && F.cont == 0u,
	      "S-15.194 the check, the view, then the restart");
	expect_screen("factory_once_restarting");
	expect_title("restarting");
	CHECK(appended(s15_once_restarting), "S-15.004 the line is logged");
	/* No key acts on the restarting view. */
	before = s15_menu_changes(&M);
	clear_records();
	for (unsigned key = 0; key < S15_NKEYS; key++)
		press(key);
	CHECK(F.once == 0u && F.once_checks == 0u && F.reset_w7 == 0u &&
	      M.screen == S15_SCR_STARTING && s15_menu_changes(&M) == before,
	      "S-15.194 no key on the restarting view");

	/* Each refusal: no restart, the developer menu with the reason, which
	 * stays usable. */
	for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
		reset_fake(); default_boot(); start();
		press(0x08);
		F.once_rc = refused[i].rc;
		F.once_file.why = refused[i].why;
		F.once_file.value = refused[i].value;
		F.once_file.limit = refused[i].limit;
		clear_records();
		press(0x08);
		CHECK(F.once_checks == 1u && F.once == 0u && M.screen == S15_SCR_FACTORY,
		      "S-15.194 %s: no restart", refused[i].screen);
		expect_screen(refused[i].screen);
		press(0x08);			/* again: drawn again, not stacked */
		expect_screen(refused[i].screen);
		press(0x13);
		CHECK(F.reset_w7 == 1u, "S-15.095 %s: 6 still restarts", refused[i].screen);
	}

	/* Start in boot menu on: the restart all the same, its start boots
	 * the file whatever the setting says (S-04.014). */
	reset_fake(); default_boot(); BOOT.settings.start_menu = true; start();
	press(0x08);
	F.once_rc = S15_ONCE_STARTED;
	clear_records();
	press(0x08);
	CHECK(F.once_checks == 1u && F.once == 1u && F.once_view && M.start_menu,
	      "S-15.194 restarts with start in boot menu on, the setting unchanged");
	CHECK(F.saves == 0u, "S-15.194 no setting saved");

	/* MTP on, or an OS write's result shown: digit 3 does nothing. */
	reset_fake(); default_boot(); start();
	press(0x08);
	F.once_rc = S15_ONCE_STARTED;
	press(0x07);
	clear_records();
	press(0x08);
	CHECK(F.once_checks == 0u && F.once == 0u && M.mtp != S15_MTP_OFF,
	      "S-15.092 busy with MTP: digit 3 does nothing");
	reset_fake(); default_boot(); start();
	press(0x08);
	F.once_rc = S15_ONCE_STARTED;
	press(0x12);
	clear_records();
	press(0x08);
	CHECK(F.once_checks == 0u && F.once == 0u && M.osw == S15_OSW_VIEW_DONE,
	      "S-15.092 the OS write's result: digit 3 does nothing");

	/* A build that starts no OS (the test OS): refused, with why. */
	reset_fake(); default_boot();
	s14_log_init(&LOG, S14_MASTER, NULL, &rec_link, NULL, NULL);
	s15_menu_init(&M, &BOOT, &env_bare, &LOG);
	s15_menu_start(&M);
	press(0x08);
	clear_records();
	press(0x08);
	CHECK(F.once_checks == 0u && F.once == 0u && M.screen == S15_SCR_FACTORY,
	      "S-15.194 no file start in this build");
	expect_screen("factory_once_disabled");
}

static void test_continue(void)
{
	uint32_t before;

	/* The main screen offers no Continue: digit 6 does nothing. */
	reset_fake(); default_boot(); start();
	expect_screen("main_plain");
	before = s15_menu_changes(&M);
	press(0x13);
	CHECK(F.cont == 0u && F.reset_w7 == 0u && M.screen == S15_SCR_MAIN &&
	      s15_menu_changes(&M) == before, "main: digit 6 does nothing");
	/* S-15.031: the main screen's digit 7 continues to the OS */
	reset_fake(); default_boot(); start();
	clear_records();
	press(0x14);
	CHECK(F.cont == 1u && F.cont_view && F.reset_w7 == 0u && M.screen == S15_SCR_STARTING,
	      "S-15.190 main: digit 7 continues to the OS");
	expect_screen("continue_restarting");
	/* also with the developer menu hidden, and with a failed start shown */
	reset_fake(); default_boot(); BOOT.settings.dev_mode = false;
	BOOT.start.why = S15_START_NO_ACTIVE; start();
	clear_records();
	press(0x14);
	CHECK(F.cont == 1u && M.screen == S15_SCR_STARTING, "main: digit 7, notice shown");
	/* back on main after another screen, the item again */
	reset_fake(); default_boot(); start();
	press(0x12);
	press(0x15);
	expect_screen("main_plain");
	clear_records();
	press(0x14);
	CHECK(F.cont == 1u, "main again: digit 7 continues");
	reset_fake(); default_boot(); start();
	/* The host's request: the restarting view, then the board. */
	CHECK(s15_menu_continue(&M) == S15_CONT_STARTED && F.cont == 1u &&
	      F.cont_view && M.screen == S15_SCR_STARTING, "S-15.190 the host continues");
	expect_screen("continue_restarting");
	expect_title("restarting");
	CHECK(appended(s15_cont_restarting), "S-15.004 the line is logged");
	/* No key acts on the restarting view. */
	before = s15_menu_changes(&M);
	clear_records();
	for (unsigned key = 0; key < S15_NKEYS; key++)
		press(key);
	CHECK(F.cont == 0u && F.n_usb == 0u && F.reset_w7 == 0u && F.leds_start == 0u &&
	      M.screen == S15_SCR_STARTING && s15_menu_changes(&M) == before,
	      "S-15.190 no key on the restarting view");
	CHECK(s15_menu_continue_check(&M) == S15_CONT_BUSY &&
	      s15_menu_continue(&M) == S15_CONT_BUSY && F.cont == 0u,
	      "S-05.033 not started twice");

	/* The slave absent, lost at the start or stopped since: it restarts
	 * all the same (the board picks the restart). */
	for (int k = 0; k < 4; k++) {
		reset_fake(); default_boot();
		BOOT.slave = k == 0 ? S15_SLAVE_ABSENT : k == 1 ? S15_SLAVE_LOST :
			     k == 2 ? S15_SLAVE_NO_START : S15_SLAVE_OK;
		F.lost = k == 3;
		start();
		clear_records();
		CHECK(s15_menu_continue(&M) == S15_CONT_STARTED, "no slave (%d)", k);
		CHECK(F.cont == 1u && F.cont_view && M.screen == S15_SCR_STARTING,
		      "S-05.033 restarts without the slave (%d)", k);
	}
	/* No such action on the board: the request is refused, digit 6 does
	 * nothing. */
	reset_fake(); default_boot();
	s14_log_init(&LOG, S14_MASTER, NULL, &rec_link, NULL, NULL);
	s15_menu_init(&M, &BOOT, &env_bare, &LOG);
	s15_menu_start(&M);
	clear_records();
	n_app = 0;
	press(0x13);
	CHECK(M.screen == S15_SCR_MAIN && s15_menu_continue(&M) == S15_CONT_BUSY &&
	      F.cont == 0u && F.n_usb == 0u, "no continue_os: refused, digit 6 does nothing");
	expect_screen("main_bare");
	/* ... and digit 7 restarts with W7, as a build without Continue's
	 * other keys do (S-15.190) */
	press(0x14);
	CHECK(F.reset_w7 == 1u && F.restart_line && F.cont == 0u,
	      "no continue_os: digit 7 restarts with W7");

	/* Busy: a scan, host transfer, run or OS write under way. */
	reset_fake(); default_boot(); start();
	M.activity = S15_ACT_SCAN;
	CHECK(s15_menu_continue(&M) == S15_CONT_BUSY && F.cont == 0u &&
	      M.screen == S15_SCR_MAIN, "busy: scan");
	M.activity = S15_ACT_TRANSFER;
	CHECK(s15_menu_continue_check(&M) == S15_CONT_BUSY, "busy: transfer");
	M.activity = S15_ACT_RUN;
	CHECK(s15_menu_continue_check(&M) == S15_CONT_BUSY, "busy: run");
	M.activity = S15_ACT_IDLE;
	M.busy = true;
	CHECK(s15_menu_continue_check(&M) == S15_CONT_BUSY, "busy: OS write");
	M.busy = false;
	CHECK(s15_menu_continue_check(&M) == S15_CONT_STARTED, "idle");
	expect_screen("main_plain");

	/* A key test under way ends with its LED messages before the
	 * board's restart; anything armed is canceled. */
	reset_fake(); default_boot(); start();
	press(0x12);
	press(0x06);			/* the key test */
	CHECK(M.keytest && F.leds_start == 1u, "key test on");
	CHECK(s15_menu_continue(&M) == S15_CONT_STARTED && F.cont_view &&
	      F.leds_end_at_cont == 1u && F.leds_end == 1u && !M.keytest,
	      "S-07.045 the key test's end, once, before the restart");
	reset_fake(); default_boot(); start();
	press(0x12);
	ev(0x14, true);			/* the factory reset held */
	CHECK(M.armed == S15_ARM_RESET, "held");
	CHECK(s15_menu_continue(&M) == S15_CONT_STARTED && M.armed == S15_ARM_NONE,
	      "the hold canceled");
	CHECK(F.leds_end == 0u, "no key test: no LED message");
	tick(S15_CONFIRM_HOLD_MS);
	ev(0x14, false);
	CHECK(F.n_removed == 0u, "the hold's time runs nothing after");

}

/* The upgrade screen: after a completed run, not reason 40, digit 6
 * continues to the OS (S-15.190); on a board with no continue_os it is the
 * restart W7 (S-15.095). */
static void test_done_restart(void)
{
	for (int o = 0; o < 3; o++) {
		for (int r = 0; r < 2; r++) {
			for (int bare = 0; bare < 2; bare++) {
				const bool item = o != S15_OUT_FAILED && !r;

				reset_fake(); default_boot(); BOOT.reason = r ? 40 : 30;
				if (bare) {
					s14_log_init(&LOG, S14_MASTER, NULL, &rec_link, NULL, NULL);
					s15_menu_init(&M, &BOOT, &env_bare, &LOG);
					s15_menu_start(&M);
				} else {
					start();
				}
				press(0x13);
				CHECK(F.reset_w7 == 0u && F.cont == 0u, "upgrade: no 6 before a run");
				s15_up_run_start(&M, S15_RUN_FIRMWARE, "1.3.0", NULL, false);
				press(0x13);
				CHECK(F.reset_w7 == 0u && F.cont == 0u, "upgrade: no 6 while it runs");
				s15_up_run_end(&M, (enum s15_outcome)o, 0);
				if (o == S15_OUT_DONE && !r)
					expect_tail("upgrade_done_tail");
				if (o == S15_OUT_NOTHING && !r)
					expect_tail("upgrade_nothing_tail");
				CHECK(appended(s15_restart_item) == item,
				      "upgrade: the item (%d, %d, %d)", o, r, bare);
				press(0x13);
				CHECK(F.reset_w7 == (item && bare ? 1u : 0u) &&
				      F.restart_line == (item && bare) &&
				      F.cont == (item && !bare ? 1u : 0u) &&
				      F.cont_view == (item && !bare) &&
				      (M.screen == S15_SCR_STARTING) == (item && !bare),
				      "upgrade: digit 6 after the run (%d, %d, %d)", o, r, bare);
			}
		}
	}
}

/* S-15.175: Settings' digit 6, saved with the others in the running
 * slot's settings sector; the item shows what a start reads after it. */
static void test_start_menu(void)
{
	reset_fake(); default_boot(); start();
	press(0x07);
	expect_screen("settings");
	press(0x13);
	CHECK(F.saves == 1u && F.saved.start_menu && M.start_menu && F.saved.dev_mode &&
	      !F.saved.color && F.saved.mtp_macos, "S-15.175 on, saved with the others");
	expect_screen("settings_start_menu_on");
	press(0x13);
	CHECK(F.saves == 2u && !F.saved.start_menu && !M.start_menu, "S-15.175 off");
	expect_screen("settings");
	/* A failed save: the line, and what a start reads after it. */
	F.store_rc = S15_STORE_FAILED;
	F.store_after = false;
	press(0x13);
	CHECK(F.saves == 3u && F.saved.start_menu && !M.start_menu, "failed: off");
	expect_screen("settings_store_not_saved");
	/* A slot that keeps no settings: refused, the item as a start reads */
	F.store_rc = S15_STORE_UNUSABLE;
	press(0x13);
	CHECK(F.saves == 4u && F.saved.start_menu && !M.start_menu, "unusable: off");
	expect_screen("settings_store_unusable");
	/* S-15.193: a key a host sent leaves it alone; the unit's saves it. */
	reset_fake(); default_boot(); start();
	press(0x07);
	s15_menu_host_event(&M, 0x13, true);
	s15_menu_host_event(&M, 0x13, false);
	CHECK(F.saves == 0u && !M.start_menu && M.screen == S15_SCR_SETTINGS &&
	      !M.host_event, "host key: nothing");
	s15_menu_host_event(&M, 0x15, true);	/* other host keys act as before */
	s15_menu_host_event(&M, 0x15, false);
	CHECK(M.screen == S15_SCR_MAIN, "host key: 8 goes back");
	press(0x07);
	press(0x13);
	CHECK(F.saves == 1u && M.start_menu, "unit key: saved");
	/* As read at the start. */
	reset_fake(); default_boot(); BOOT.settings.start_menu = true; start();
	press(0x07);
	expect_screen("settings_start_menu_on");
	/* Without a settings sector (the test OS): no item, digit 6 does
	 * nothing, and a change lasts for this start. */
	reset_fake(); default_boot();
	s14_log_init(&LOG, S14_MASTER, NULL, &rec_link, NULL, NULL);
	s15_menu_init(&M, &BOOT, &env_bare, &LOG);
	s15_menu_start(&M);
	press(0x07);
	CHECK(!appended("6 Start in boot menu  Off"), "no sector: no item");
	press(0x13);
	CHECK(F.saves == 0u && M.screen == S15_SCR_SETTINGS, "no sector: nothing");
	press(0x08);
	CHECK(M.color && F.saves == 0u && appended(s15_settings_this_start),
	      "no sector: this start only");
}

/* S-02.105, S-15.052: a factory reset clears the running slot's settings
 * sector, also when the drive is refused, and the settings take their
 * defaults; a failed clear says so; a slot with no settings, nothing. */
static void test_reset_settings(void)
{
	for (int way = 0; way < 4; way++) {
		reset_fake(); default_boot();
		BOOT.settings.color = true;
		BOOT.settings.start_menu = true;
		F.refused = way == 3;
		F.clear_rc = way == 1 ? S15_STORE_FAILED :
			     way == 2 ? S15_STORE_UNUSABLE : S15_STORE_SAVED;
		start();
		press(0x12);
		hold(0x14);
		CHECK(F.clears == 1u && F.erase_fat == 1u, "S-02.105 cleared (%d)", way);
		CHECK(F.saves == 0u, "S-02.105 a clear, not a save (%d)", way);
		CHECK(appended(s15_reset_settings_failed) == (way == 1),
		      "S-02.105 the line (%d)", way);
		CHECK((!M.color && !M.dev_mode && M.mtp_macos && !M.start_menu) ==
		      (way == 0 || way == 3), "S-02.105 defaults after a clear (%d)", way);
	}
	/* without a settings sector: nothing to clear */
	reset_fake(); default_boot();
	s14_log_init(&LOG, S14_MASTER, NULL, &rec_link, NULL, NULL);
	s15_menu_init(&M, &BOOT, &env_bare, &LOG);
	s15_menu_start(&M);
	press(0x12);
	hold(0x14);
	CHECK(F.clears == 0u && F.erase_fat == 1u, "no sector: no clear");
}

/* The indicator on every frame once on, the key test included. */
static unsigned ind_seen;

static void d_fill(void *c, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
		   uint16_t v)
{
	(void)c; (void)x; (void)y; (void)w; (void)h; (void)v;
}

static void d_text(void *c, uint16_t x, uint16_t y, const char *s, size_t n,
		   uint16_t fg, uint16_t bg)
{
	(void)c; (void)x; (void)y; (void)fg; (void)bg;
	if (n == strlen(s15_indicator_text) && memcmp(s, s15_indicator_text, n) == 0)
		ind_seen++;
}

static void test_indicator(void)
{
	static const struct s15_geometry g = {
		.width = 480, .height = 222, .edge = 8, .margin = 16, .top = 27,
		.pitch = 19, .glyph_w = 8, .glyph_h = 16, .bar_h = 24,
		.title_w = 10, .title_h = 17, .title_y = 4,
		.status_y = 202,
	};
	static const struct s15_draw_ops d = { d_fill, d_text, NULL, NULL, NULL };

	reset_fake(); default_boot(); start();
	ind_seen = 0;
	s15_menu_render(&M, &g, &d);
	CHECK(ind_seen == 0u, "off at the start");
	press(0x07);
	ev(0x07, true);
	press(0x15);           /* only cancels */
	ev(0x07, false);
	CHECK(!M.option_on && M.screen == S15_SCR_SETTINGS,
	      "S-15.140 another key only cancels");
	press(0x15);
	press(0x07);
	hold(0x07);
	press(0x15);
	press(0x06);
	CHECK(M.option_on && M.screen == S15_SCR_UPGRADE,
	      "S-15.140 on from Settings, kept on the upgrade screen");
	s15_menu_render(&M, &g, &d);
	CHECK(ind_seen == 1u, "indicator");

	reset_fake(); default_boot(); start();
	press(0x07);
	hold(0x07);
	CHECK(M.option_on && M.screen == S15_SCR_SETTINGS,
	      "S-15.140 on from Settings");
	press(0x15);
	press(0x13);
	CHECK(M.option_on, "S-15.140 0x13 on the main screen does nothing");
	press(0x12);
	press(0x06);
	ind_seen = 0;
	s15_menu_render(&M, &g, &d);
	CHECK(M.keytest && ind_seen == 1u, "in the key test");
}

/* S-15.065: the black frame, then the system menu drawn without another
 * key: what the board's loop does, a render whenever the changes differ. */
static bool kt_item_seen;

static void kt_fill(void *c, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
		    uint16_t rgb)
{
	(void)c; (void)x; (void)y; (void)w; (void)h; (void)rgb;
}

static void kt_text(void *c, uint16_t x, uint16_t y, const char *s, size_t n,
		    uint16_t fg, uint16_t bg)
{
	(void)c; (void)x; (void)y; (void)fg; (void)bg;
	if (n == 8u && memcmp(s, "Key test", 8) == 0)
		kt_item_seen = true;
}

static void test_keytest_end_draws(void)
{
	static const struct s15_geometry g = {
		.width = 480, .height = 222, .edge = 8, .margin = 16, .top = 27,
		.pitch = 19, .glyph_w = 8, .glyph_h = 16, .bar_h = 24,
		.title_w = 10, .title_h = 17, .title_y = 4, .status_y = 202,
	};
	static const struct s15_draw_ops d = { kt_fill, kt_text, NULL, NULL, NULL };
	uint32_t shown;

	reset_fake(); default_boot(); start();
	press(0x12);
	press(0x06);
	ev(0x37, true);
	ev(0x15, true);
	CHECK(!M.keytest && M.screen == S15_SCR_SYSTEM, "S-15.065 the test ended");
	kt_item_seen = false;
	shown = s15_menu_changes(&M);
	s15_menu_render(&M, &g, &d);
	CHECK(!kt_item_seen, "S-15.065 first the black frame");
	CHECK(s15_menu_changes(&M) != shown, "S-15.065 another render is due");
	shown = s15_menu_changes(&M);
	s15_menu_render(&M, &g, &d);
	CHECK(kt_item_seen, "S-15.065 then the system menu");
	CHECK(s15_menu_changes(&M) == shown, "S-15.065 and nothing more is due");

	/* S-15.104: a host transfer's begin leaves the key test, which ends
	 * with its LEDs (S-07.045) as at any other screen change. */
	reset_fake(); default_boot(); start();
	press(0x12);
	press(0x06);
	CHECK(M.keytest && F.leds_start == 1u, "the key test runs");
	s15_up_transfer_begin(&M, 2000);
	CHECK(!M.keytest && F.leds_end == 1u && M.screen == S15_SCR_UPGRADE,
	      "S-15.104 the key test ended, its LEDs too");
	s15_up_run_start(&M, S15_RUN_FIRMWARE, "1.3.0", NULL, false);
	CHECK(F.leds_end == 1u, "ended once");
}

/* S-15.173: while the color is off every pixel drawn is gray. */
static unsigned color_px, gray_px;

static void count(uint16_t v)
{
	if (v >> 11 == (v & 0x1fu) && v >> 11 == ((v >> 5) & 0x3fu) >> 1)
		gray_px++;
	else
		color_px++;
}

static void g_fill(void *c, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
		   uint16_t v)
{
	(void)c; (void)x; (void)y; (void)w; (void)h;
	count(v);
}

static void g_text(void *c, uint16_t x, uint16_t y, const char *s, size_t n,
		   uint16_t fg, uint16_t bg)
{
	(void)c; (void)x; (void)y; (void)s; (void)n;
	count(fg);
	count(bg);
}

static void g_ink(void *c, uint16_t x, uint16_t y, const char *s, size_t n,
		  uint16_t fg)
{
	(void)c; (void)x; (void)y; (void)s; (void)n;
	count(fg);
}

static void test_gray(void)
{
	static const struct s15_geometry g = {
		.width = 480, .height = 222, .edge = 8, .margin = 16, .top = 27,
		.pitch = 19, .glyph_w = 8, .glyph_h = 16, .bar_h = 24,
		.title_w = 10, .title_h = 17, .title_y = 4,
		.status_y = 202,
	};
	static const struct s15_draw_ops d = { g_fill, g_text, NULL, g_ink, g_ink };
	struct s15_frame f;

	/* gray by default; in color: the amber bar and digits are not gray */
	reset_fake(); default_boot(); start();
	color_px = gray_px = 0;
	s15_menu_render(&M, &g, &d);
	CHECK(color_px == 0u && gray_px != 0u, "S-15.173 gray by default");
	reset_fake(); default_boot(); BOOT.settings.color = true; start();
	color_px = gray_px = 0;
	s15_menu_render(&M, &g, &d);
	CHECK(color_px != 0u, "S-15.173 in color when on");

	/* off: Settings, the main screen, the option's red bar, the key test */
	press(0x07);
	press(0x08);
	s15_menu_frame(&M, &f);
	CHECK(f.gray, "S-15.173 the frame says gray");
	color_px = gray_px = 0;
	s15_menu_render(&M, &g, &d);
	CHECK(color_px == 0u && gray_px != 0u, "S-15.173 Settings all gray");
	hold(0x07);            /* protected writes on: the red bar */
	press(0x15);
	color_px = 0;
	s15_menu_render(&M, &g, &d);
	CHECK(M.option_on && color_px == 0u, "S-15.173 the main screen, red bar, gray");
	press(0x12);           /* System */
	press(0x06);
	ev(0x10, true);
	color_px = 0;
	s15_menu_render(&M, &g, &d);
	CHECK(M.keytest && color_px == 0u, "S-15.173 the key test gray");
}

/* A long operation holds the keys until the board has read what arrived
 * during it: their downs have no effect, then the next press acts once. */
static void raw_press(unsigned key)
{
	s15_menu_event(&M, key, true);
	s15_menu_event(&M, key, false);
}

static void test_hold(void)
{
	/* S-15.092: the OS write; digits 6 and 8 read after it, before the
	 * board's release, have no effect. */
	reset_fake(); default_boot(); F.osw = S15_OSW_OK; start();
	press(0x08);
	raw_press(0x12);
	CHECK(s15_menu_held(&M) && M.osw_written, "the OS write holds the keys");
	raw_press(0x13);
	raw_press(0x15);
	CHECK(F.reset_w7 == 0u && F.cont == 0u && M.osw == S15_OSW_VIEW_DONE,
	      "S-15.092 no effect while held");
	ev(0x37, true);
	CHECK(M.shift, "S-15.012 shift is tracked while held");
	ev(0x37, false);
	CHECK(!s15_menu_held(&M), "released");
	press(0x13);
	CHECK(F.cont == 1u && F.reset_w7 == 0u, "S-15.190 the next 6 continues");

	/* A key held across the release acts only after an up and a down. */
	reset_fake(); default_boot(); F.osw = S15_OSW_OK; start();
	press(0x08);
	raw_press(0x12);
	s15_menu_event(&M, 0x13, true);
	s15_menu_release(&M);
	s15_menu_event(&M, 0x13, true);   /* a repeated down */
	CHECK(F.cont == 0u, "a down from the hold does not act at release");
	ev(0x13, false);
	press(0x13);
	CHECK(F.cont == 1u, "the next press does");

	/* The format, the factory reset and a pack's removal hold them too. */
	reset_fake(); default_boot(); start();
	press(0x12);
	press(0x12);
	s15_menu_event(&M, 0x06, true);
	tick(S15_CONFIRM_HOLD_MS);
	s15_menu_event(&M, 0x06, false);
	CHECK(F.format == 1u && s15_menu_held(&M), "the format holds the keys");
	raw_press(0x15);
	CHECK(M.screen == S15_SCR_UPGRADE, "8 read before the release");
	s15_menu_release(&M);
	press(0x15);
	CHECK(M.screen == S15_SCR_MAIN, "8 after it");

	reset_fake(); default_boot(); start();
	press(0x12);
	s15_menu_event(&M, 0x14, true);
	tick(S15_CONFIRM_HOLD_MS);
	s15_menu_event(&M, 0x14, false);
	CHECK(F.n_removed == 3u && s15_menu_held(&M), "the reset holds them");
	raw_press(0x15);
	CHECK(M.screen == S15_SCR_RESULT, "8 read before the release");
	s15_menu_release(&M);
	press(0x15);
	CHECK(M.screen == S15_SCR_SYSTEM, "8 after it");

	reset_fake(); default_boot(); F.dlc = true; F.npacks = 1;
	F.pack_names[0] = "p"; start();
	press(0x12);
	press(0x08);
	s15_menu_event(&M, 0x06, true);
	tick(S15_CONFIRM_HOLD_MS);
	s15_menu_event(&M, 0x06, false);
	CHECK(F.n_removed == 2u && s15_menu_held(&M), "a removal holds them");
	s15_menu_release(&M);

	/* Short calls hold nothing: a setting saved, the key test, Info. */
	reset_fake(); default_boot(); start();
	raw_press(0x07);
	raw_press(0x06);
	CHECK(F.saves == 1u && !s15_menu_held(&M), "a saved setting");
	raw_press(0x15);
	raw_press(0x12);
	raw_press(0x06);
	CHECK(M.keytest && !s15_menu_held(&M), "the key test");
}

/* A step row's label ends before its bar or result: the phases' and the steps'. */
static void test_step_labels_fit(void)
{
	static const enum s15_phase ph[] = {
		S15_PH_DECRYPT, S15_PH_EXTRACT_FIRMWARE, S15_PH_EXTRACT_CONTENT,
		S15_PH_TRANSFER,
	};
	char row[ROWLEN];

	reset_fake(); default_boot(); BOOT.reason = 30; start();
	scan_by_release();
	for (size_t i = 0; i < sizeof(ph) / sizeof(ph[0]); i++) {
		const char *label;

		s15_up_phase(&M, ph[i]);
		s15_up_progress(&M, 1, 2);
		label = strchr(last_row(row), ':');
		CHECK(label != NULL && strcspn(label + 1, "\t") <= S15_STEP_LABEL_MAX,
		      "phase %zu's label fits: '%s'", i, row);
	}
	for (unsigned k = 0; k < S15_STEP_COUNT; k++)
		CHECK(strlen(s15_step_label((enum s15_step)k)) <= S15_STEP_LABEL_MAX,
		      "step %u's label fits: '%s'", k,
		      s15_step_label((enum s15_step)k));
}

int main(void)
{
	test_rules();
	test_screens();
	test_status();
	test_boot_prompt();
	test_keys();
	test_packs();
	test_reset_and_system();
	test_format();
	test_factory();
	test_os_once();
	test_os_write();
	test_hold();
	test_upgrade();
	test_confirm_screen();
	test_run_end_skipped();
	test_continue();
	test_done_restart();
	test_start_menu();
	test_reset_settings();
	test_step_labels_fit();
	test_indicator();
	test_keytest_end_draws();
	test_gray();
	return check_done("test_s15_menu");
}

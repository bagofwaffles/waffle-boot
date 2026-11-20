/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15 held mode: screens and transitions.
 */
#include "waffle/s15_menu.h"

#include "waffle/bytes.h"

#define DLC "/yaffs2/content/dlc"

static void open_main(struct s15_menu *m);
static void open_system(struct s15_menu *m);
static void draw_system(struct s15_menu *m, bool armed);
static void open_upgrade(struct s15_menu *m, bool midi);

/* ---- small helpers -------------------------------------------------- */

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

static void line(struct s15_menu *m, const char *s)
{
	s15_console_append(&m->con, s);
}

/* A line in a style. */
static void sline(struct s15_menu *m, enum s15_style st, const char *s)
{
	s15_console_add(&m->con, st, s);
}

static void blank(struct s15_menu *m)
{
	line(m, "");
}

/* The console cleared under a new name in the title bar; the
 * screen and its state stay. */
static void view(struct s15_menu *m, const char *title)
{
	m->title = title;
	m->changes++;
	s15_console_clear(&m->con);
}

/* Leaving a screen: the key test ends with its LEDs (S-07.045), and
 * a hold under way is canceled. */
static void leave(struct s15_menu *m)
{
	if (m->keytest) {
		m->keytest = false;
		m->changes++;
		m->env->keytest_leds(m->env->ctx, false);
	}
	m->armed = S15_ARM_NONE;
	m->pending = 0;
	m->osw = S15_OSW_VIEW_NONE;
}

/* A screen is opened or drawn: what belongs to the screen it leaves goes. */
static void enter(struct s15_menu *m, enum s15_screen s, const char *title)
{
	leave(m);
	m->screen = s;
	view(m, title);
}

/* An erasing action's result: its view, left with digit 8 alone. */
static void result_back(struct s15_menu *m)
{
	m->screen = S15_SCR_RESULT;
	blank(m);
	sline(m, S15_ST_BACK, "8 Back to System");
}

/* S-10a.010: a USB set started; the status bar's USB word follows it. */
/* The set first: starting it can draw the screen (S-15.100), and the status
 * word says USB only once the set is up. */
static void usb(struct s15_menu *m, enum s15_usb_set set)
{
	m->env->usb_set(m->env->ctx, set);
	m->usb = set;
	m->changes++;
}

static bool refused(struct s15_menu *m)
{
	return m->env->volume_refused(m->env->ctx);
}

static bool slave_lost(struct s15_menu *m)
{
	return m->env->slave_lost != NULL && m->env->slave_lost(m->env->ctx);
}

/* S-15.191, S-12.062: before the session's first write to the drive, a
 * mount restored from a checkpoint gives way to a scan, under this line. */
static bool files_restored(struct s15_menu *m)
{
	return m->boot->files_way == S15_FILES_RESTORED && m->env->files_scan != NULL &&
	       !slave_lost(m);
}

static void check_files(struct s15_menu *m)
{
	if (!files_restored(m))
		return;
	sline(m, S15_ST_DIM, s15_checking_files);
	m->env->files_scan(m->env->ctx);
}

void s15_check_files(struct s15_menu *m)
{
	if (m->boot != NULL && m->env != NULL)
		check_files(m);
}

/* The same as the first thing a screen shows, before its own lines. */
static void check_files_on(struct s15_menu *m, enum s15_screen s, const char *title)
{
	if (!files_restored(m))
		return;
	enter(m, s, title);
	blank(m);
	check_files(m);
}

static bool path_join(char *out, size_t cap, const char *a, const char *b)
{
	size_t n = 0, i;
	bool fit = true;

	for (i = 0; a[i] != '\0'; i++)
		if (n + 1u < cap)
			out[n++] = a[i];
		else
			fit = false;
	if (n + 1u < cap)
		out[n++] = '/';
	else
		fit = false;
	for (i = 0; b[i] != '\0'; i++)
		if (n + 1u < cap)
			out[n++] = b[i];
		else
			fit = false;
	out[n] = '\0';
	return fit;
}

/* ---- holds: S-15.015 ----------------------------------------------- */

#define HOLD_MS S15_HOLD_MS
#define KEY_PRECOMP 0x17u	/* S-07.032: not acted on */

/* A knob click (0x0A - 0x0D) starts no hold: it is the easiest key to
 * press by accident, and the white knob is one. */
static bool knob_click(unsigned key)
{
	return key >= 0x0au && key <= 0x0du;
}

/* The hold's row, the screen's last: added, or replaced (not logged,
 * S-15.004) when what it shows has changed. */
static void hold_row(struct s15_menu *m, bool add)
{
	uint32_t t = m->now_ms - m->arm_ms;
	struct s15_buf b;
	unsigned fill, left;

	if (t > HOLD_MS)
		t = HOLD_MS;
	fill = (unsigned)(t * 100u / HOLD_MS);
	left = (unsigned)((HOLD_MS - t + 999u) / 1000u);
	if (!add && fill == m->arm_fill && left == m->arm_left)
		return;
	m->arm_fill = fill;
	m->arm_left = left;
	s15_hold_bar(&b, fill, left);
	if (add)
		sline(m, S15_ST_STEP_RUN, b.s);
	else
		s15_console_set(&m->con, S15_ST_STEP_RUN, b.s);
}

/* The item's key went down, the screen drawn with the item held: the hold
 * starts now. */
static void hold_begin(struct s15_menu *m, enum s15_arm a, unsigned key)
{
	m->armed = a;
	m->arm_key = key;
	m->arm_ms = m->now_ms;
	hold_row(m, true);
}

/* The held item's row: its label, the hold as its hint. */
static void held_item(struct s15_menu *m, unsigned digit, const char *label)
{
	struct s15_buf b, h;

	s15_buf_init(&b);
	s15_buf_dec(&b, digit);
	s15_buf_str(&b, " ");
	s15_buf_str(&b, label);
	s15_buf_str(&b, "\t");
	s15_hold_hint(&h, digit);
	s15_buf_str(&b, h.s);
	sline(m, S15_ST_ARMED, b.s);
}

/* ---- the option and its screen: S-15.140 ---------------------------- */

static void set_option(struct s15_menu *m, bool on)
{
	m->option_on = on;
	m->env->option(m->env->ctx, on);   /* erases, programs, sends nothing */
}

static void draw_settings(struct s15_menu *m);

/* The option's screen: what it allows, its item, the way back; the item
 * held in place, its hold's row to follow. */
static void draw_option(struct s15_menu *m, bool armed)
{
	struct s15_buf b, h;

	enter(m, S15_SCR_OPTION, "Protected Writes");
	for (size_t i = 0; i < S15_OPTION_PROMPT_LINES; i++)
		sline(m, s15_option_prompt_style[i], s15_option_prompt[i]);
	if (armed) {
		held_item(m, 2u, s15_option_turn_on);
	} else {
		s15_buf_init(&b);
		s15_buf_str(&b, "2 ");
		s15_buf_str(&b, s15_option_turn_on);
		s15_buf_str(&b, "\t");
		s15_hold_hint(&h, 2u);
		s15_buf_str(&b, h.s);
		sline(m, S15_ST_ITEM, b.s);
	}
	sline(m, S15_ST_BACK, "8 Back to Settings");
}

/* Settings' digit 2: off at once; while off, the option's screen, with
 * no hold. The key that opened it is down already, so only a new key down
 * starts one. */
static void option_key(struct s15_menu *m)
{
	if (m->option_on) {
		set_option(m, false);
		draw_settings(m);
		return;
	}
	draw_option(m, false);
}

/* The option's screen: digit 2 starts the hold that turns it on
 * (S-15.015), 8 goes back with nothing changed. */
static void option_screen_key(struct s15_menu *m, unsigned key)
{
	const unsigned d = s15_digit(key);

	if (d == 8u) {
		draw_settings(m);
	} else if (d == 2u && !knob_click(key)) {
		draw_option(m, true);
		hold_begin(m, S15_ARM_OPTION, key);
	}
}

/* S-11.080: this boot found an install a reset had interrupted. */
static void interrupted(struct s15_menu *m)
{
	if (!m->boot->interrupted)
		return;
	for (size_t i = 0; i < S15_INTERRUPTED_LINES; i++)
		sline(m, S15_ST_ERROR, s15_interrupted[i]);
}

/* S-15.163: a failed plain start, then why. An image marked encrypted
 * gives S-15.161's line as its why. A boot that found an interrupted
 * install attempted no start, so the two notices never meet. */
static void start_lines(struct s15_menu *m, const struct s15_start *s)
{
	struct s15_buf why, b;

	if (s->why == S15_START_OK)
		return;
	sline(m, S15_ST_ERROR, s->file ? s15_start_failed_file : s15_start_failed);
	if (s->why == S15_START_ENCRYPTED) {               /* S-15.161 */
		sline(m, S15_ST_ERROR, s15_os_reinstall);
		return;
	}
	s15_start_reason(&why, s);
	s15_reason_line(&b, why.s);
	sline(m, S15_ST_DIM, b.s);
}

/* A notice under the items, the failed start's or the interrupted
 * install's (they never meet); false when there is none. */
static bool start_failed(struct s15_menu *m)
{
	start_lines(m, &m->boot->start);
	return m->boot->start.why != S15_START_OK;
}

/* ---- the main screen: S-15.031 - S-15.032 --------------------------- */

/* Drawing the main screen (again). */
static void draw_main(struct s15_menu *m)
{
	enter(m, S15_SCR_MAIN, "Main Menu");
	blank(m);
	sline(m, S15_ST_ITEM, "1 Install firmware");
	sline(m, S15_ST_ITEM, "2 Settings");
	if (m->dev_mode)
		sline(m, S15_ST_ITEM, "3 Developer menu");
	sline(m, S15_ST_ITEM, "4 Device info");
	sline(m, S15_ST_ITEM, "5 System");
	/* S-15.031: digit 7 set apart at the bottom, by a notice when there
	 * is one, else by a blank row; nine rows either way */
	if (!start_failed(m) && !m->boot->interrupted)
		blank(m);
	interrupted(m);
	sline(m, S15_ST_ITEM, s15_continue_item);
}

/* S-15.032: opening starts the main screen's USB set. */
static void open_main(struct s15_menu *m)
{
	usb(m, S15_USB_MAIN);
	draw_main(m);
}

/* ---- settings: S-15.170 - S-15.172 ---------------------------------- */

static void option_item(struct s15_menu *m)
{
	if (m->option_on)
		sline(m, S15_ST_ITEM_ON, s15_option_item_on);
	else
		sline(m, S15_ST_ITEM, s15_option_item_off);
}

/* S-15.170 */
static void draw_settings(struct s15_menu *m)
{
	enter(m, S15_SCR_SETTINGS, "Settings");
	blank(m);
	sline(m, S15_ST_ITEM, m->dev_mode ? "1 Developer mode\tOn" :
					    "1 Developer mode\tOff");
	option_item(m);
	sline(m, S15_ST_ITEM, m->color ? "3 Color\tOn" :      /* S-15.173 */
					  "3 Color\tOff");
	sline(m, S15_ST_ITEM, m->mtp_macos ? "5 MTP target\tmacOS" : /* S-15.174 */
					      "5 MTP target\tWindows");
	if (m->env->settings_save != NULL)                 /* S-15.175 */
		sline(m, S15_ST_ITEM, m->start_menu ? "6 Start in boot menu\tOn" :
						       "6 Start in boot menu\tOff");
	blank(m);
	sline(m, S15_ST_BACK, "8 Back to Main Menu");
}

/* S-15.172, S-15.193: the settings saved in the running slot's settings
 * sector, by a key on the unit alone. A setting changed by a host's key,
 * or one that cannot be saved, lasts for this start; one line says so.
 * Start in boot menu is read only at a start, so its item shows what a
 * start reads after the save. */
static void settings_changed(struct s15_menu *m)
{
	struct s15_settings s, now;
	enum s15_store_save r = S15_STORE_SAVED;

	s.dev_mode = m->dev_mode;
	s.color = m->color;
	s.mtp_macos = m->mtp_macos;
	s.start_menu = m->start_menu;
	if (m->env->settings_save != NULL && !m->host_event) {
		now = s;
		r = m->env->settings_save(m->env->ctx, &s, &now);
		m->start_menu = now.start_menu;
	}
	draw_settings(m);
	if (m->host_event)
		sline(m, S15_ST_WARN, s15_settings_host);
	else if (m->env->settings_save == NULL)
		sline(m, S15_ST_WARN, s15_settings_this_start);
	else if (r == S15_STORE_FAILED)
		sline(m, S15_ST_WARN, s15_settings_failed);
	else if (r == S15_STORE_UNUSABLE)
		sline(m, S15_ST_WARN, s15_settings_unusable);
}

static void settings_key(struct s15_menu *m, unsigned key)
{
	switch (s15_digit(key)) {
	case 1:
		m->dev_mode = !m->dev_mode;
		settings_changed(m);
		break;
	case 2:
		option_key(m);              /* S-15.140 */
		break;
	case 3:
		m->color = !m->color;
		m->changes++;   /* every pixel changes, not only the lines */
		settings_changed(m);
		break;
	case 5:
		m->mtp_macos = !m->mtp_macos;
		settings_changed(m);
		break;
	case 6:
		/* S-15.193: a host's key leaves Start in boot menu as it is */
		if (m->env->settings_save != NULL && !m->host_event) {
			m->start_menu = !m->start_menu;
			settings_changed(m);
		}
		break;
	case 8:
		open_main(m);
		break;
	default:
		break;
	}
}

/* ---- the factory reset: S-15.050 - S-15.052 ------------------------- */

/* S-02.105: the running slot's settings sector cleared, and the settings
 * at their defaults from here on; a clear that failed says so. */
static void clear_settings(struct s15_menu *m)
{
	struct s15_settings d;

	if (m->env->settings_clear == NULL)
		return;
	switch (m->env->settings_clear(m->env->ctx)) {
	case S15_STORE_SAVED:
		s15_settings_defaults(&d);
		if (m->color != d.color)
			m->changes++;	/* every pixel changes, not only the lines */
		m->dev_mode = d.dev_mode;
		m->color = d.color;
		m->mtp_macos = d.mtp_macos;
		m->start_menu = d.start_menu;
		break;
	case S15_STORE_FAILED:
		sline(m, S15_ST_WARN, s15_reset_settings_failed);
		break;
	case S15_STORE_UNUSABLE:
	default:
		break;
	}
}

static void factory_reset(struct s15_menu *m)
{
	static const char *const dirs[] = { "/yaffs2/user", "/yaffs2/logs", DLC };
	bool ok = true;

	enter(m, S15_SCR_RESULT, "Factory Reset");
	check_files(m);
	line(m, "Removing user data...");
	if (refused(m) || slave_lost(m)) {
		/* S-12.052: nothing removed or erased on the yaffs2 volume;
		 * S-15.052: the OS FAT volume's erase and the settings' clear
		 * are made all the same. */
		m->env->erase_os_fat(m->env->ctx);
		clear_settings(m);
		sline(m, S15_ST_ERROR, slave_lost(m) ? s15_reset_lost :
						       s15_reset_unreadable);
		result_back(m);
		return;
	}
	for (size_t i = 0; i < 3u; i++)
		ok = m->env->remove_tree(m->env->ctx, dirs[i]) && ok;
	m->env->erase_os_fat(m->env->ctx);                /* S-16.014 */
	clear_settings(m);
	/* S-15.052: a failed removal does not stop the reset; one line of
	 * required information replaces the OK line. */
	if (ok)
		sline(m, S15_ST_OK, "User data removed. Please restart your OP-XY.");
	else
		sline(m, S15_ST_ERROR, s15_reset_incomplete);
	result_back(m);
}

/* ---- content packs: S-15.041 - S-15.043 ----------------------------- */

static void read_packs(struct s15_menu *m)
{
	const struct s15_env_ops *e = m->env;
	uint8_t manifest[511];

	m->npacks = 0;
	while (m->npacks < S15_PACKS_MAX &&
	       e->pack_dir(e->ctx, m->npacks, m->dirs[m->npacks])) {
		char name[64];
		size_t n;

		m->dirs[m->npacks][S15_NAME_MAX] = '\0';
		n = e->read_manifest(e->ctx, m->dirs[m->npacks], manifest,
				     sizeof manifest);
		(void)s15_manifest_name(n != 0u ? manifest : NULL, n, name);
		(void)s15_pack_label(name, m->dirs[m->npacks],
				     m->labels[m->npacks]);
		m->npacks++;
	}
}

/* S-15.042: the list; pack `pending` (0: none) held, its hold's row to
 * follow, or `after` rows of the caller's. With none held, the hold line
 * under the packs when it fits. The blank rows go first when they would
 * not fit. */
static void draw_packs(struct s15_menu *m, unsigned pending, unsigned after)
{
	struct s15_buf b;
	const unsigned base = m->npacks + 1u + (pending != 0u ? 1u : after);
	const bool hint = pending == 0u && base + 1u <= S15_ROWS;
	const unsigned need = base + (hint ? 1u : 0u);

	enter(m, S15_SCR_PACKS, "Content Packs");
	m->pending = pending;
	if (need + 2u <= S15_ROWS)
		blank(m);
	for (unsigned i = 0; i < m->npacks; i++) {
		if (pending == i + 1u) {
			held_item(m, i + 1u, m->labels[i]);
			continue;
		}
		s15_buf_init(&b);
		s15_buf_dec(&b, i + 1u);
		s15_buf_str(&b, " ");
		s15_buf_str(&b, m->labels[i]);
		sline(m, S15_ST_ITEM, b.s);
	}
	if (hint)
		sline(m, S15_ST_DIM, s15_hold_line);
	if (need + 1u <= S15_ROWS)
		blank(m);
	sline(m, S15_ST_BACK, "8 Back to System");
}

/* The list, or the system menu again without item 3. */
static void open_packs(struct s15_menu *m)
{
	read_packs(m);
	if (m->npacks == 0u) {
		m->packs_item = false;
		draw_system(m, false);
		return;
	}
	draw_packs(m, 0, 0);
}

/* S-15.043 */
static void remove_pack(struct s15_menu *m, unsigned n)
{
	char path[sizeof DLC + S15_NAME_MAX + 1u];
	char label[64];
	struct s15_buf b;
	bool ok;

	for (size_t i = 0; i < sizeof label; i++)
		label[i] = m->labels[n - 1u][i];
	view(m, "Content Packs");
	check_files(m);
	s15_buf_init(&b);
	s15_buf_str(&b, "Removing ");
	s15_buf_str(&b, label);
	s15_buf_str(&b, "...");
	line(m, b.s);
	ok = path_join(path, sizeof path, DLC, m->dirs[n - 1u]) &&
	     m->env->remove_tree(m->env->ctx, path);
	if (!ok) {
		/* S-15.043: the list drawn again, then the line after it. */
		read_packs(m);
		if (m->npacks == 0u) {
			m->packs_item = false;
			draw_system(m, false);
		} else {
			draw_packs(m, 0, 1);
		}
		s15_pack_failed_line(&b, label);
		sline(m, S15_ST_ERROR, b.s);
		return;
	}
	s15_buf_init(&b);
	s15_buf_str(&b, label);
	s15_buf_str(&b, " removed.");
	sline(m, S15_ST_OK, b.s);
	read_packs(m);
	if (m->npacks != 0u) {
		draw_packs(m, 0, 0);
		return;
	}
	(void)m->env->remove_tree(m->env->ctx, DLC);
	sline(m, S15_ST_OK, "All content packs removed.");
	line(m, "Please restart your OP-XY.");
	m->packs_item = false;
	result_back(m);
}

/* S-15.042: a pack's digit starts the hold of its removal. */
static void packs_key(struct s15_menu *m, unsigned key)
{
	unsigned d = s15_digit(key);

	if (d == 8u) {
		m->packs_item = true;     /* S-15.042: drawn again, not opened */
		draw_system(m, false);
		return;
	}
	if (d < 1u || d > 7u || d > m->npacks || knob_click(key))
		return;
	draw_packs(m, d, 0);
	hold_begin(m, S15_ARM_PACK, key);
}

/* ---- the Info screen and its second page: S-15.081, S-15.082 -------- */

/* A value of a Device Info row; a tab in it (a serial the host set,
 * S-10c.056) would move the next column, so it shows as '?'. */
static void info_value(struct s15_buf *b, const char *v)
{
	for (size_t i = 0; v != NULL && v[i] != '\0'; i++)
		s15_buf_strn(b, v[i] == '\t' ? "?" : &v[i], 1);
}

/* A Device Info row: "Key\tValue" or "Key\tValue\tKey\tValue". */
static void info_row(struct s15_menu *m, const char *k1, const char *v1,
		     const char *k2, const char *v2)
{
	struct s15_buf b;

	s15_buf_init(&b);
	s15_buf_str(&b, k1);
	s15_buf_str(&b, "\t");
	info_value(&b, v1);
	if (k2 != NULL) {
		s15_buf_str(&b, "\t");
		s15_buf_str(&b, k2);
		s15_buf_str(&b, "\t");
		info_value(&b, v2);
	}
	sline(m, S15_ST_KV, b.s);
}

/* The start log's fill: entries used of S15_LOG_ENTRIES, or its state. */
static void log_fill(struct s15_buf *v, const struct s15_boot *b)
{
	s15_buf_init(v);
	switch ((enum s15_log)b->start_log) {
	case S15_LOG_OK:
		s15_buf_dec(v, b->start_log_used);
		s15_buf_str(v, " of ");
		s15_buf_dec(v, S15_LOG_ENTRIES);
		break;
	case S15_LOG_FULL:
		s15_buf_str(v, "Full");
		break;
	case S15_LOG_ODD:
		s15_buf_str(v, "Not valid");
		break;
	case S15_LOG_NONE:
		s15_buf_str(v, "Not used");
		break;
	case S15_LOG_UNREAD:
	default:
		s15_buf_str(v, "Not read");
		break;
	}
}

/* Serials, then the unit's state on the left, versions on the right;
 * digit 1 opens the second page (S-15.082). */
static void open_info(struct s15_menu *m)
{
	const struct s15_boot *b = m->boot;
	struct s15_buf start;
	/* A slave that answered at the start can stop later in the boot. */
	const enum s15_slave slave = b->slave == S15_SLAVE_OK && slave_lost(m) ?
				     S15_SLAVE_LOST : b->slave;
	const char *dsp2 = slave == S15_SLAVE_NO_OS ? s15_slave_no_os :
			   slave == S15_SLAVE_ABSENT ? s15_slave_absent :
			   slave == S15_SLAVE_NO_START ? s15_slave_no_start :
			   slave == S15_SLAVE_LOST ? s15_slave_lost :
			   "OK";

	enter(m, S15_SCR_INFO, "Device Info");
	info_row(m, "Unit S/N", b->unit_serial, "Firmware", b->os_version);
	info_row(m, "DSP S/N", b->chip_serial, "Blackfin", b->sw_version);
	info_row(m, "Backplane S/N", b->psoc_serial, "Bootloader", b->version);
	info_row(m, "Keys", b->provisioned ? "Provisioned" : "None",
		 "BLE", b->ble_version);
	info_row(m, "DSP2", dsp2, "Backplane", b->psoc_version);
	blank(m);
	sline(m, S15_ST_ITEM, "1 More\tStart log");
	if (b->start.why != S15_START_OK) {                /* S-15.163 */
		s15_start_info(&start, &b->start);
		info_row(m, "OS start", start.s, NULL, NULL);
	} else {
		blank(m);
	}
	sline(m, S15_ST_BACK, "8 Back to Main Menu");
}

/* S-15.082: the selected slot's start log, read again as the page opens.
 * The rows below it are free for more of the unit's state. */
static void open_info2(struct s15_menu *m)
{
	const struct s15_boot *b = m->boot;
	struct s15_buf v;

	if (m->env->start_log_read != NULL)
		m->env->start_log_read(m->env->ctx);
	enter(m, S15_SCR_INFO2, "Device Info 2");
	s15_buf_init(&v);
	if ((enum s15_log)b->start_log == S15_LOG_UNREAD) {
		s15_buf_str(&v, "Not read");
	} else {
		s15_buf_str(&v, "Slot ");
		s15_buf_dec(&v, b->start_log_slot + 1u);
	}
	info_row(m, "Start log", v.s, NULL, NULL);
	log_fill(&v, b);
	info_row(m, "Starts logged", v.s, NULL, NULL);
	if ((enum s15_log)b->start_log == S15_LOG_OK) {
		s15_buf_init(&v);
		s15_buf_dec(&v, b->start_log_unfinished);
		s15_buf_str(&v, " at the end");
		info_row(m, "Unfinished", v.s, NULL, NULL);
	}
	if (b->start_log_erase != 0u) {
		s15_buf_init(&v);
		s15_buf_dec(&v, b->start_log_erase);
		s15_buf_str(&v, " used");
		info_row(m, "Erased at", v.s, NULL, NULL);
	}
	if (b->files_way != S15_FILES_NONE) {			/* S-15.192 */
		s15_files_text(&v, b->files_way, b->files_objects);
		info_row(m, "Files", v.s, NULL, NULL);
	}
	blank(m);
	sline(m, S15_ST_BACK, "8 Back to Device Info");
}

/* ---- the system menu and the key test: S-15.060 - S-15.065 ---------- */

/* S-15.060; S-15.050: item 7 held in place, its hold's row to follow. */
static void draw_system(struct s15_menu *m, bool armed)
{
	enter(m, S15_SCR_SYSTEM, "System");
	blank(m);
	sline(m, S15_ST_ITEM, "1 Key test");
	sline(m, S15_ST_ITEM, "2 Reset volume calibration");
	if (m->packs_item)
		sline(m, S15_ST_ITEM, "3 Remove content packs");
	sline(m, S15_ST_ITEM, "5 Format drive\tWipes the drive: reinstall after");
	if (armed) {
		held_item(m, 7u, "Factory reset");
	} else {
		sline(m, S15_ST_ITEM, "7 Factory reset\tRemoves your data only");
		sline(m, S15_ST_DIM, s15_hold_line);
	}
	/* No pack item then: dir_exists() does not answer. */
	if (slave_lost(m))
		sline(m, S15_ST_WARN, s15_drive_lost);
	blank(m);
	sline(m, S15_ST_BACK, "8 Back to Main Menu");
}

/* S-15.060: opening checks content/dlc again; a refused volume has
 * none. */
static void open_system(struct s15_menu *m)
{
	m->packs_item = !refused(m) && !slave_lost(m) &&
			m->env->dir_exists(m->env->ctx, DLC);
	draw_system(m, false);
}

static void keytest_start(struct s15_menu *m)
{
	s15_keytest_start(&m->kt);
	m->keytest = true;
	m->changes++;
	m->env->keytest_leds(m->env->ctx, true);
}

/* S-15.064, S-15.065 */
static void keytest_event(struct s15_menu *m, unsigned key, bool down)
{
	m->changes++;
	if (!s15_keytest_event(&m->kt, key, down))
		return;
	m->black_once = true;          /* the screen cleared to black */
	open_system(m);                /* leave() ends the test and its LEDs */
}

static void open_format(struct s15_menu *m);

/* S-15.060, S-15.050 */
static void system_key(struct s15_menu *m, unsigned key)
{
	switch (s15_digit(key)) {
	case 1:
		keytest_start(m);
		break;
	case 2:
		m->env->volcal_reset(m->env->ctx);    /* S-15.061 */
		open_main(m);
		break;
	case 3:
		if (m->packs_item)
			open_packs(m);
		break;
	case 5:
		open_format(m);
		break;
	case 7:
		draw_system(m, true);                 /* S-15.050 */
		hold_begin(m, S15_ARM_RESET, key);
		break;
	case 8:
		open_main(m);
		break;
	default:
		break;
	}
}

/* ---- the format menu: S-15.071 - S-15.073 --------------------------- */

/* S-15.071, S-15.073: the menu, its item held, or offered (the hold
 * line under it), or neither while the format runs. */
static void draw_format(struct s15_menu *m, bool armed, bool offer)
{
	enter(m, S15_SCR_FORMAT, "Format Drive");
	blank(m);
	if (slave_lost(m))
		sline(m, S15_ST_WARN, s15_drive_lost);
	else if (m->refused)
		sline(m, S15_ST_WARN, s15_format_unreadable);
	if (armed)
		held_item(m, 1u, "Format drive");
	else
		sline(m, S15_ST_ITEM, "1 Format drive\tWipes the drive: reinstall after");
	if (!armed && offer)
		sline(m, S15_ST_DIM, s15_hold_line);
	blank(m);
	sline(m, S15_ST_BACK, "8 Back to System");
}

static void open_format(struct s15_menu *m)
{
	m->refused = refused(m);
	draw_format(m, false, true);
}

/* S-15.072: the mount after the format alone sets the refused state; a
 * failed format shows its error line and the format menu stays current. */
static void run_format(struct s15_menu *m)
{
	bool ok;

	draw_format(m, false, false);
	line(m, "Formatting the drive. Please wait...");
	ok = m->env->format(m->env->ctx);
	m->refused = refused(m);
	if (!ok) {
		sline(m, S15_ST_ERROR, "The drive could not be formatted");
		return;
	}
	sline(m, S15_ST_OK, "Done.");
	open_upgrade(m, false);
}

static void format_key(struct s15_menu *m, unsigned key)
{
	if (key == 0x06u || key == 0x0eu) {     /* not 0x0A: S-15.071 */
		draw_format(m, true, false);
		hold_begin(m, S15_ARM_FORMAT, key);
	}
	else if (s15_digit(key) == 8u)
		open_system(m);
}

/* ---- the factory menu: S-15.092 - S-15.095 -------------------------- */

/* The items keep their digits; digits 1 and 4 are no items. */
static void draw_factory(struct s15_menu *m)
{
	enter(m, S15_SCR_FACTORY, "Developer");
	blank(m);
	sline(m, S15_ST_ITEM, m->mtp_macos ? "2 MTP file transfer\tmacOS" :
					      "2 MTP file transfer\tWindows");
	sline(m, S15_ST_ITEM, s15_once_item);
	sline(m, S15_ST_ITEM, "5 Write /os.ldr to boot partition");
	sline(m, S15_ST_ITEM, s15_restart_item);
	blank(m);
	sline(m, S15_ST_BACK, "8 Back to Main Menu");
}

static void open_factory(struct s15_menu *m)
{
	m->mtp = S15_MTP_OFF;
	m->busy = false;
	draw_factory(m);
}

/* S-15.093 */
static void mtp_on(struct s15_menu *m, enum s15_mtp mtp)
{
	m->mtp = mtp;
	check_files_on(m, S15_SCR_FACTORY, "MTP File Transfer");	/* before the drive */
	usb(m, mtp == S15_MTP_VENDOR ? S15_USB_MTP_VENDOR : S15_USB_MTP);
	enter(m, S15_SCR_FACTORY, "MTP File Transfer");
	blank(m);
	line(m, "The drive is shared with the computer over USB.");
	blank(m);
	sline(m, S15_ST_BACK, "8 Back to Developer");
}

/* S-15.094: a step's row, appended (and logged) with its label alone. */
void s15_osw_step(struct s15_menu *m, enum s15_osw_step step)
{
	if (m->osw != S15_OSW_VIEW_RUNNING || (unsigned)step >= S15_OSW_STEP_COUNT)
		return;
	sline(m, S15_ST_STEP_RUN, s15_osw_label(step));
	m->osw_step = (int)step;
	m->osw_p = ~0u;
}

/* S-15.094: replaced only when p changes; not logged. */
void s15_osw_progress(struct s15_menu *m, uint32_t done, uint32_t total)
{
	struct s15_buf b;
	unsigned p = s15_progress(done, total);

	if (m->osw != S15_OSW_VIEW_RUNNING || m->osw_step < 0 || p == m->osw_p)
		return;
	m->osw_p = p;
	s15_osw_progress_row(&b, (enum s15_osw_step)m->osw_step, p);
	s15_console_set(&m->con, S15_ST_STEP_RUN, b.s);
}

void s15_osw_done(struct s15_menu *m, uint32_t a, uint32_t b)
{
	struct s15_buf row;

	if (m->osw != S15_OSW_VIEW_RUNNING || m->osw_step < 0)
		return;
	s15_osw_done_row(&row, (enum s15_osw_step)m->osw_step, a, b);
	s15_console_set(&m->con, S15_ST_STEP_OK, row.s);
	m->osw_step = -1;
}

/* S-15.094: the outcome's lines under the steps, after a success the
 * restart, then the back line. */
static void osw_outcome(struct s15_menu *m, const struct s15_osw_end *e)
{
	struct s15_buf b, why;
	enum s15_style st;

	if (e->result == S15_OSW_OK) {
		sline(m, S15_ST_OK, s15_osw_written);
	} else {
		if (m->osw_step >= 0) {
			s15_osw_failed_row(&b, (enum s15_osw_step)m->osw_step);
			s15_console_set(&m->con, S15_ST_STEP_FAIL, b.s);
		}
		sline(m, S15_ST_ERROR, e->result == S15_OSW_UNREADABLE ?
				       s15_os_ldr_unreadable : s15_os_write_failed);
		if (e->why != S15_OSW_WHY_NONE) {
			s15_osw_reason(&why, e->why, e->part, e->value, e->limit);
			s15_reason_line(&b, why.s);
			sline(m, S15_ST_DIM, b.s);
		}
		s15_osw_emmc_line(&b, &st, e->emmc, e->target);
		sline(m, st, b.s);
	}
	if (e->select0_failed)
		sline(m, S15_ST_WARN, s15_osw_select0_failed);
	m->osw_step = -1;
	/* S-15.190: a success offers the restart into the OS */
	if (e->result == S15_OSW_OK)
		sline(m, S15_ST_ITEM, s15_restart_item);
	sline(m, S15_ST_BACK, s15_osw_back);
}

/* S-15.094, S-16.123: busy only while the write runs; the screen then
 * waits for digit 8, or digit 6 (restart) after a success. */
static void os_write(struct s15_menu *m)
{
	struct s15_osw_end e = { S15_OSW_FAILED, S15_OSW_WHY_NONE,
				 S15_OSW_EMMC_UNTOUCHED, 0, 0, 0, 0, false };

	m->busy = true;
	enter(m, S15_SCR_FACTORY, "Write OS");
	m->osw = S15_OSW_VIEW_RUNNING;
	m->osw_step = -1;
	line(m, "Looking for /os.ldr...");
	m->env->os_write(m->env->ctx, &e);
	osw_outcome(m, &e);
	m->osw_written = e.result == S15_OSW_OK;
	m->osw = S15_OSW_VIEW_DONE;
	m->busy = false;
}

/* S-15.095: the line is on the panel before the shutdown begins (the
 * board's reset_w7 sends the frame first), so the seconds until the unit
 * comes back do not look like a key that did nothing. */
static void restart(struct s15_menu *m)
{
	sline(m, S15_ST_WARN, s15_restarting);
	m->env->reset_w7(m->env->ctx);
}

/* S-15.194: the restart into a start from /yaffs2/os.ldr, or the reason
 * there is none, under the developer menu drawn again. That start boots
 * whatever start in boot menu says (S-04.014). */
static void os_once(struct s15_menu *m)
{
	struct s15_os_file file = { S15_OSW_WHY_NONE, 0, 0 };
	struct s15_buf why, b;
	enum s15_once r;

	if (m->env->os_once_check == NULL || m->env->os_once == NULL)
		r = S15_ONCE_DISABLED;
	else
		r = m->env->os_once_check(m->env->ctx, &file);
	if (r == S15_ONCE_STARTED) {
		/* the view whole on the panel before the shutdown begins */
		enter(m, S15_SCR_STARTING, s15_cont_title);
		sline(m, S15_ST_WARN, s15_once_restarting);
		m->env->os_once(m->env->ctx);
		return;
	}
	draw_factory(m);
	sline(m, S15_ST_ERROR, s15_once_not_started);
	s15_once_reason(&why, r, &file);
	s15_reason_line(&b, why.s);
	sline(m, S15_ST_DIM, b.s);
}

/* S-15.190: the restart offered after an install or an OS write starts the
 * OS, whatever reason this boot took. */
static void restart_to_os(struct s15_menu *m)
{
	if (m->env->continue_os != NULL)
		(void)s15_menu_continue(m);
	else
		restart(m);
}

/* S-15.092 */
static void factory_key(struct s15_menu *m, unsigned key)
{
	unsigned d = s15_digit(key);

	if (m->mtp != S15_MTP_OFF) {
		if (d == 8u) {                     /* S-15.093: 8 turns it off */
			m->mtp = S15_MTP_OFF;
			usb(m, S15_USB_SERIAL);
			draw_factory(m);
		}
		return;
	}
	if (m->busy)
		return;
	if (m->osw != S15_OSW_VIEW_NONE) {        /* S-15.094 */
		if (d == 8u)
			draw_factory(m);           /* back, drawn again */
		else if (d == 6u && m->osw_written)
			restart_to_os(m);                  /* S-15.190 */
		return;
	}
	switch (d) {
	case 2:                                    /* S-15.174 */
		mtp_on(m, m->mtp_macos ? S15_MTP_VENDOR : S15_MTP_STANDARD);
		break;
	case 3:
		os_once(m);                        /* S-15.194 */
		break;
	case 5:
		os_write(m);
		break;
	case 6:
		restart(m);                        /* S-15.095 */
		break;
	case 8:
		open_main(m);
		break;
	default:
		break;
	}
}

/* ---- the upgrade screen: S-15.100 - S-15.115 ------------------------ */

/* S-15.100 - S-15.102: the opening lines. */
static void draw_upgrade(struct s15_menu *m)
{
	enter(m, S15_SCR_UPGRADE, "Install Firmware");
	m->restart_ok = false;
	if (m->midi) {                                     /* S-15.102 */
		interrupted(m);
		line(m, "Preparing the update. Please wait.");
		return;
	}
	if (m->boot->interrupted)                          /* S-11.080 */
		interrupted(m);
	else
		blank(m);                                  /* S-15.100 */
	line(m, "1. Connect the OP-XY to a computer");
	line(m, "2. Copy the firmware to the disk that appears");
	line(m, "3. Eject the disk");
	sline(m, S15_ST_DIM, "Waiting for the package...");
	blank(m);
	sline(m, S15_ST_BACK, "8 Back to Main Menu");
}

static void open_upgrade(struct s15_menu *m, bool midi)
{
	m->midi = midi;
	m->scan_due_set = false;
	check_files_on(m, S15_SCR_UPGRADE, "Install Firmware");
	if (midi) {
		usb(m, S15_USB_MAIN);  /* S-15.102 */
		draw_upgrade(m);
		return;
	}
	draw_upgrade(m);
	/* S-15.100, S-16.161: the system information before the disk. */
	if (m->shift)
		m->env->system_info(m->env->ctx);
	usb(m, S15_USB_DISK);
}

/* S-15.103: idle while no scan, host transfer or run is in progress and
 * no zip has been read in this boot. A scan that found nothing and a
 * refusal at checks 1 - 15 or for the product word leave it idle; a run
 * or a refusal at checks 16 - 17 ends it (S-15.115). */
static bool upgrade_idle(const struct s15_menu *m)
{
	return m->activity == S15_ACT_IDLE && !m->final;
}

/* S-15.103: digit 8 leaves the disk form whenever it is idle. */
static bool can_leave(const struct s15_menu *m)
{
	return m->screen == S15_SCR_UPGRADE && !m->midi && upgrade_idle(m);
}

/* S-15.103: a host transfer's begin, refused or not, ends the scans for
 * the rest of the boot. */
static bool can_scan(const struct s15_menu *m)
{
	return can_leave(m) && !m->begun;
}

/* A run, or a refusal at checks 16 - 17, has read the package's zip. */
static void zip_read(struct s15_menu *m)
{
	m->final = true;
	if (m->from_disk)
		m->disk_read = true;                /* S-11.049, S-15.115 */
}

/* S-15.104: the upgrade screen made current without its opening lines or
 * a USB set. */
static void make_upgrade_current(struct s15_menu *m)
{
	enter(m, S15_SCR_UPGRADE, "Install Firmware");
	m->row_step = -1;
	m->restart_ok = false;
}

static void prepare(struct s15_menu *m)
{
	make_upgrade_current(m);
	m->prog_key = -1;
	sline(m, S15_ST_HEAD, "Preparing the installation");
}

static void start_scan(struct s15_menu *m)
{
	m->scan_due_set = false;
	m->activity = S15_ACT_SCAN;
	m->from_disk = true;
	prepare(m);
	m->env->scan_now(m->env->ctx);
}

static void upgrade_key(struct s15_menu *m, unsigned key)
{
	if (s15_digit(key) == 8u && can_leave(m)) {
		/* S-15.103: back while nothing is under way; the main
		 * screen's USB set withdraws the disk. */
		m->scan_due_set = false;
		open_main(m);
	} else if (s15_digit(key) == 6u && m->restart_ok &&
		   m->activity == S15_ACT_IDLE) {
		restart_to_os(m);                   /* S-15.190 */
	}
}

void s15_up_acquired(struct s15_menu *m)
{
	m->scan_due_set = false;                    /* not released now */
}

/* S-15.105: 2 s after the release, not before 5 s of uptime. */
void s15_up_released(struct s15_menu *m, uint32_t now_ms)
{
	uint32_t due = now_ms + 2000u;

	if (!can_scan(m))
		return;
	if (due < 5000u)
		due = 5000u;
	m->scan_due_ms = due;
	m->scan_due_set = true;
}

void s15_up_tick(struct s15_menu *m, uint32_t now_ms)
{
	if (m->scan_due_set && can_scan(m) &&
	    (int32_t)(now_ms - m->scan_due_ms) >= 0)
		start_scan(m);
}

/* S-15.104 */
void s15_up_transfer_begin(struct s15_menu *m, uint32_t len)
{
	m->scan_due_set = false;
	m->activity = S15_ACT_TRANSFER;
	m->begun = true;                            /* S-15.103 */
	m->from_disk = false;
	m->transfer_total = len;
	prepare(m);
	check_files(m);		/* the host waits for the begin's reply */
}

/* The Transfer row is appended at a begin that starts a
 * transfer, not at one refused. */
void s15_up_transfer_started(struct s15_menu *m)
{
	if (m->transfer_total != 0u)
		s15_up_phase(m, S15_PH_TRANSFER);
}

/* After every 20th chunk and after the last. */
void s15_up_transfer_chunk(struct s15_menu *m, uint32_t n, uint32_t done,
			   bool last)
{
	if (m->transfer_total == 0u || (n % 20u != 0u && !last))
		return;
	s15_up_progress(m, done, m->transfer_total);
}

/* Nothing under way: the back line again when digit 8 leaves. */
static void ended(struct s15_menu *m)
{
	m->activity = S15_ACT_IDLE;
	if (can_leave(m)) {
		blank(m);
		sline(m, S15_ST_BACK, "8 Back to Main Menu");
	}
}

static const char *const no_package = "No valid package found";

void s15_up_not_found(struct s15_menu *m)
{
	sline(m, S15_ST_WARN, no_package);
	ended(m);
}

/* No package exists to refuse, so neither
 * the not-found line nor a reason line. */
void s15_up_no_transfer(struct s15_menu *m)
{
	sline(m, S15_ST_ERROR, "The transfer could not start");
	ended(m);
}

void s15_up_refused(struct s15_menu *m, const char *reason)
{
	struct s15_buf b;

	sline(m, S15_ST_WARN, no_package);           /* once */
	s15_reason_line(&b, reason);
	sline(m, S15_ST_DIM, b.s);
	ended(m);
}

/* The phases' rows (S15_ST_STEP_RUN); a label is at most
 * S15_STEP_LABEL_MAX characters, or it meets the row's bar. */
static const char *const phase_label[] = {
	[S15_PH_DECRYPT] = "Decrypting",
	[S15_PH_EXTRACT_FIRMWARE] = "Unpacking",
	[S15_PH_EXTRACT_CONTENT] = "Unpacking",
	[S15_PH_TRANSFER] = "Transfer",
};

void s15_up_phase(struct s15_menu *m, enum s15_phase phase)
{
	sline(m, S15_ST_STEP_RUN, phase_label[phase]);
	m->prog_key = (int)phase;
	m->prog_p = ~0u;
}

static void progress_line(struct s15_menu *m, const char *label, unsigned p)
{
	struct s15_buf b;

	s15_buf_init(&b);
	s15_buf_str(&b, label);
	s15_buf_str(&b, "\t");
	s15_buf_dec(&b, p);
	s15_buf_str(&b, "%");
	/* not logged (S-15.004) */
	s15_console_set(&m->con, S15_ST_STEP_RUN, b.s);
}

/* Replaced only when p changes. */
void s15_up_progress(struct s15_menu *m, uint32_t done, uint32_t total)
{
	unsigned p = s15_progress(done, total);

	if (m->prog_key < 0 || m->prog_key > (int)S15_PH_TRANSFER ||
	    p == m->prog_p)
		return;
	m->prog_p = p;
	progress_line(m, phase_label[m->prog_key], p);
}

static void outcome(struct s15_menu *m, enum s15_outcome o, unsigned skipped,
		    bool content_only, bool cont)
{
	const char *out[2];
	enum s15_style st[2];
	struct s15_buf parts[2];
	size_t n;

	/* a completed run names the parts it did not update, then ends */
	n = o == S15_OUT_FAILED ? 0u : s15_not_updated(parts, st, skipped);
	for (size_t i = 0; i < n; i++)
		sline(m, st[i], parts[i].s);
	n = s15_outcome_lines(out, st, o, skipped != 0u,
			      m->boot->reason == 40u, content_only, cont);
	for (size_t i = 0; i < n; i++)
		sline(m, st[i], out[i]);
}

void s15_up_extract_refused(struct s15_menu *m, const char *reason,
			    bool content_only)
{
	struct s15_buf b;

	sline(m, S15_ST_ERROR, "Operation 3 failed");
	s15_reason_line(&b, reason);
	sline(m, S15_ST_DIM, b.s);
	outcome(m, S15_OUT_FAILED, 0, content_only, false);
	zip_read(m);
	ended(m);
}

/* Step progress keys are kept apart from the phases. */
#define STEP_KEY(step) (16 + (int)(step))

void s15_up_run_start(struct s15_menu *m, enum s15_run_kind kind,
		      const char *version, const char *name, bool stock)
{
	struct s15_buf hdr[4];
	enum s15_style st[4];
	size_t n;

	m->scan_due_set = false;
	m->activity = S15_ACT_RUN;
	m->content_only = kind == S15_RUN_CONTENT;
	m->prog_key = -1;
	make_upgrade_current(m);
	n = s15_run_header(hdr, st, kind, version, name, stock);
	for (size_t i = 0; i < n; i++)
		sline(m, st[i], hdr[i].s);
}

void s15_up_step_start(struct s15_menu *m, enum s15_step step)
{
	if (m->content_only && step != S15_STEP_CONTENT) {
		m->prog_key = -1;                   /* no line of its own */
		m->row_step = -1;
		return;
	}
	sline(m, S15_ST_STEP_RUN, s15_step_label(step));
	m->row_step = (int)step;
	m->prog_key = STEP_KEY(step);
	m->prog_p = ~0u;
}

void s15_up_step_progress(struct s15_menu *m, enum s15_step step,
			  uint32_t done, uint32_t total)
{
	struct s15_buf b;
	unsigned p = s15_progress(done, total);

	if (m->prog_key != STEP_KEY(step) || p == m->prog_p)
		return;
	m->prog_p = p;
	if (s15_step_progress_line(&b, step, p) == S15_OP_REPLACE)
		s15_console_set(&m->con, S15_ST_STEP_RUN, b.s);
}

void s15_up_step_result(struct s15_menu *m, enum s15_step step,
			enum s15_result result, const char *version,
			bool refused_start, const char *cause)
{
	struct s15_buf b, row;
	enum s15_style st = S15_ST_TEXT;

	switch (s15_result_line(&b, &st, step, result, version,
				m->content_only)) {
	case S15_OP_REPLACE:
		if (m->row_step == (int)step)
			s15_console_set(&m->con, st, b.s);
		else
			sline(m, st, b.s);
		break;
	case S15_OP_APPEND:
		if (m->row_step == (int)step) {
			s15_failed_row(&row, step, cause);
			s15_console_set(&m->con, S15_ST_STEP_FAIL, row.s);
		}
		sline(m, st, b.s);
		if (step == S15_STEP_BOOTLOADER && refused_start)
			sline(m, S15_ST_ERROR, s15_reset_needed);  /* S-02.090 */
		break;
	case S15_OP_NONE:
	default:
		break;
	}
	m->prog_key = -1;
	m->row_step = -1;
}

void s15_up_step_note(struct s15_menu *m, enum s15_note note)
{
	const char *out[S15_NOTE_LINES];
	enum s15_style st[S15_NOTE_LINES];
	const size_t n = s15_note_lines(out, st, note);

	for (size_t i = 0; i < n; i++)
		sline(m, st[i], out[i]);
}

/* S-15.116: the selection's rows. */
void s15_up_select(struct s15_menu *m, const struct s15_select_info *info,
		   const struct s15_select *s, uint32_t now_ms)
{
	struct s15_buf b[S15_ROWS];
	enum s15_style st[S15_ROWS];
	const size_t n = s15_select_lines(info, s, now_ms, b, st);

	make_upgrade_current(m);
	m->prog_key = -1;	/* S-15.116: no key but the selection's */
	for (size_t i = 0; i < n; i++)
		sline(m, st[i], b[i].s);
}

/* The rows whose text the time changed; not logged (S-15.004). */
void s15_up_select_time(struct s15_menu *m, const struct s15_select_info *info,
			const struct s15_select *s, uint32_t now_ms)
{
	struct s15_buf b[S15_ROWS];
	enum s15_style st[S15_ROWS];
	const size_t n = s15_select_lines(info, s, now_ms, b, st);

	for (size_t i = 0; i < n; i++) {
		size_t len;
		const char *row = s15_console_row(&m->con, (unsigned)i, &len);

		if (len != b[i].n || s15_console_row_style(&m->con, (unsigned)i) !=
		    st[i] || !wb_equal((const uint8_t *)row,
				       (const uint8_t *)b[i].s, len))
			s15_console_put(&m->con, (unsigned)i, st[i], b[i].s);
	}
}

/* S-15.115: as a run that did nothing; the zip was read. */
void s15_up_canceled(struct s15_menu *m)
{
	const char *out[2];
	enum s15_style st[2];
	size_t n;

	make_upgrade_current(m);
	m->prog_key = -1;
	m->restart_ok = m->boot->reason != 40u;
	n = s15_outcome_lines(out, st, S15_OUT_NOTHING, true, false, false,
			      m->restart_ok);
	for (size_t i = 0; i < n; i++)
		sline(m, st[i], out[i]);
	if (m->restart_ok)
		sline(m, S15_ST_ITEM, s15_restart_item);
	zip_read(m);
	ended(m);
}

/* S-15.115 */
void s15_up_run_end(struct s15_menu *m, enum s15_outcome o,
		    unsigned skipped)
{
	/* a completed run (S-11.060), outside reason 40, offers the restart */
	m->restart_ok = o != S15_OUT_FAILED && m->boot->reason != 40u;
	outcome(m, o, skipped, m->content_only, m->restart_ok);
	if (m->restart_ok)
		sline(m, S15_ST_ITEM, s15_restart_item);
	zip_read(m);
	ended(m);
}

bool s15_up_disk_offerable(const struct s15_menu *m)
{
	return !m->disk_read;
}

/* ---- continue to the OS: S-15.190, S-05.033 ------------------------ */

enum s15_continue s15_menu_continue_check(const struct s15_menu *m)
{
	if (m->busy || m->hold || m->activity != S15_ACT_IDLE ||
	    m->env->continue_os == NULL ||
	    m->screen == S15_SCR_STARTING || m->screen == S15_SCR_NONE)
		return S15_CONT_BUSY;
	return S15_CONT_STARTED;
}

enum s15_continue s15_menu_continue(struct s15_menu *m)
{
	const enum s15_continue c = s15_menu_continue_check(m);

	if (c == S15_CONT_BUSY)
		return c;
	/* the view whole on the panel before the shutdown begins (the
	 * board's continue_os sends the frame first) */
	enter(m, S15_SCR_STARTING, s15_cont_title);
	sline(m, S15_ST_WARN, s15_cont_restarting);
	m->env->continue_os(m->env->ctx);
	return c;
}

/* ---- keys: S-15.010 - S-15.013, S-15.031 ---------------------------- */

static void main_key(struct s15_menu *m, unsigned key)
{
	unsigned d = s15_digit(key);

	/* S-15.031: digit 8 is the back key below this screen, no item here */
	if (key == S15_KEY_HELP || d == 4u)
		open_info(m);
	else if (d == 1u)
		open_upgrade(m, false);
	else if (d == 2u)
		draw_settings(m);
	else if (d == 3u && m->dev_mode)
		open_factory(m);
	else if (d == 5u)
		open_system(m);
	else if (d == 7u)
		restart_to_os(m);	/* S-15.031, S-15.190 */
}

/* S-15.015: a hold ended early: the screen as it was. */
static void hold_cancel(struct s15_menu *m)
{
	const enum s15_arm a = m->armed;

	m->armed = S15_ARM_NONE;
	m->pending = 0;
	switch (a) {
	case S15_ARM_RESET:
		draw_system(m, false);
		break;
	case S15_ARM_FORMAT:
		draw_format(m, false, true);
		break;
	case S15_ARM_PACK:
		draw_packs(m, 0, 0);
		break;
	case S15_ARM_OPTION:
		draw_option(m, false);
		break;
	case S15_ARM_NONE:
	default:
		break;
	}
}

/* S-15.015: the hold reached its time: its action, once. The key still
 * down then does nothing more. */
static void hold_run(struct s15_menu *m)
{
	const enum s15_arm a = m->armed;
	const unsigned pack = m->pending;

	m->armed = S15_ARM_NONE;
	m->pending = 0;
	switch (a) {
	case S15_ARM_RESET:
		factory_reset(m);                   /* S-15.052 */
		break;
	case S15_ARM_FORMAT:
		run_format(m);                      /* S-15.072 */
		break;
	case S15_ARM_PACK:
		remove_pack(m, pack);               /* S-15.043 */
		break;
	case S15_ARM_OPTION:
		set_option(m, true);                /* S-15.140 */
		draw_settings(m);
		break;
	case S15_ARM_NONE:
	default:
		break;
	}
}

void s15_menu_tick(struct s15_menu *m, uint32_t now_ms)
{
	m->now_ms = now_ms;
	if (m->armed == S15_ARM_NONE)
		return;
	if (now_ms - m->arm_ms >= HOLD_MS)
		hold_run(m);
	else
		hold_row(m, false);
}

static void key_down(struct s15_menu *m, unsigned key)
{
	enum s15_screen s = m->screen;

	/* S-15.015: during a hold any other key down only cancels it */
	if (m->armed != S15_ARM_NONE) {
		if (key != KEY_PRECOMP)
			hold_cancel(m);
		return;
	}
	switch (s) {
	case S15_SCR_MAIN:
		main_key(m, key);
		break;
	case S15_SCR_PACKS:
		packs_key(m, key);
		break;
	case S15_SCR_INFO:
		if (s15_digit(key) == 8u)           /* S-15.081 */
			open_main(m);
		else if (s15_digit(key) == 1u)      /* S-15.082 */
			open_info2(m);
		break;
	case S15_SCR_INFO2:
		if (s15_digit(key) == 8u)           /* S-15.082 */
			open_info(m);
		break;
	case S15_SCR_SYSTEM:
		system_key(m, key);
		break;
	case S15_SCR_SETTINGS:
		settings_key(m, key);
		break;
	case S15_SCR_OPTION:
		option_screen_key(m, key);  /* S-15.140 */
		break;
	case S15_SCR_FORMAT:
		format_key(m, key);
		break;
	case S15_SCR_FACTORY:
		factory_key(m, key);
		break;
	case S15_SCR_UPGRADE:
		upgrade_key(m, key);
		break;
	case S15_SCR_RESULT:
		if (s15_digit(key) == 8u)
			open_system(m);
		break;
	case S15_SCR_STARTING:                      /* S-15.190: no key */
	case S15_SCR_NONE:
	default:
		break;                              /* S-15.014 */
	}
}

void s15_menu_key(struct s15_menu *m, unsigned key, bool down)
{
	bool was_down;

	if (key >= S15_NKEYS)                       /* S-15.014 */
		return;
	was_down = bit(m->down, key);
	set_bit(m->down, key, down);
	/* S-15.013: nothing acts before the first screen. */
	if (m->screen == S15_SCR_NONE)
		return;
	/* A key down across the start acts only after an up and a down. */
	if (bit(m->stale, key)) {
		if (!down)
			set_bit(m->stale, key, false);
		return;
	}
	/* S-15.012: shift is not tracked during the key test; after it the
	 * state is what it was when the test began, until the next shift
	 * event. */
	if (m->keytest) {                           /* S-15.064 */
		keytest_event(m, key, down);
		return;
	}
	if (key == S15_KEY_SHIFT)                   /* S-15.012 */
		m->shift = down;
	/* S-15.015: the held key's release before the time cancels */
	if (!down && was_down && m->armed != S15_ARM_NONE && key == m->arm_key) {
		hold_cancel(m);
		return;
	}
	/* S-15.010: key-down events only; a repeated down is no new event. */
	if (down && !was_down)
		key_down(m, key);
}

/* A key down while held counts as one across the start (S-15.013): it acts
 * only after an up and a down. Shift is still tracked (S-15.012). */
void s15_menu_event(struct s15_menu *m, unsigned key, bool down)
{
	if (m->hold && down && key < S15_NKEYS && key != S15_KEY_SHIFT &&
	    m->screen != S15_SCR_NONE && !bit(m->down, key))
		set_bit(m->stale, key, true);
	s15_menu_key(m, key, down);
}

void s15_menu_host_event(struct s15_menu *m, unsigned key, bool down)
{
	m->host_event = true;
	s15_menu_event(m, key, down);
	m->host_event = false;
}

void s15_menu_hold(struct s15_menu *m)
{
	m->hold = true;
}

bool s15_menu_held(const struct s15_menu *m)
{
	return m->hold;
}

void s15_menu_release(struct s15_menu *m)
{
	m->hold = false;
}

/* ---- start and frames ---------------------------------------------- */

void s15_menu_init(struct s15_menu *m, const struct s15_boot *boot,
		   const struct s15_env_ops *env, struct s14_log *log)
{
	unsigned i;

	s15_console_init(&m->con, log);
	m->boot = boot;
	m->env = env;
	m->screen = S15_SCR_NONE;
	m->title = "Starting";
	m->usb = S15_USB_MAIN;
	for (i = 0; i < sizeof m->down; i++) {
		m->down[i] = 0;
		m->stale[i] = 0;
	}
	m->shift = false;
	m->option_on = false;            /* Off after any reset */
	m->dev_mode = false;
	m->color = false;
	m->mtp_macos = true;
	m->start_menu = false;
	m->packs_item = false;
	m->armed = S15_ARM_NONE;
	m->arm_key = 0;
	m->arm_ms = 0;
	m->arm_fill = 0;
	m->arm_left = 0;
	m->now_ms = 0;
	m->npacks = 0;
	m->pending = 0;
	m->refused = false;
	m->mtp = S15_MTP_OFF;
	m->busy = false;
	m->osw = S15_OSW_VIEW_NONE;
	m->osw_step = -1;
	m->osw_p = 0;
	m->osw_written = false;
	m->restart_ok = false;
	m->keytest = false;
	m->black_once = false;
	m->changes = 0;
	m->midi = false;
	m->activity = S15_ACT_IDLE;
	m->final = false;
	m->begun = false;
	m->from_disk = false;
	m->disk_read = false;
	m->scan_due_set = false;
	m->scan_due_ms = 0;
	m->content_only = false;
	m->prog_key = -1;
	m->row_step = -1;
	m->prog_p = 0;
	m->transfer_total = 0;
}

void s15_menu_keys_down(struct s15_menu *m, const uint8_t bitmap[(S15_NKEYS + 7u) / 8u])
{
	for (unsigned i = 0; i < sizeof m->down; i++)
		m->down[i] = (uint8_t)(m->down[i] | bitmap[i]);
}

/* S-15.020 */
void s15_menu_start(struct s15_menu *m)
{
	/* S-15.172: the settings as this start read them; the board
	 * fills the boot's values after s15_menu_init(). */
	m->dev_mode = m->boot->settings.dev_mode;
	m->color = m->boot->settings.color;          /* S-15.173 */
	m->mtp_macos = m->boot->settings.mtp_macos;  /* S-15.174 */
	m->start_menu = m->boot->settings.start_menu; /* S-15.175 */
	for (unsigned i = 0; i < sizeof m->down; i++)
		m->stale[i] = m->down[i];         /* S-15.013 */
	m->hold = false;	/* here: s15_menu_init() runs before the key read */
	switch (m->boot->reason) {
	case 30:
		open_upgrade(m, false);
		break;
	case 40:
		open_upgrade(m, true);
		break;
	default:
		open_main(m);
		break;
	}
}

void s15_menu_frame(const struct s15_menu *m, struct s15_frame *f)
{
	const struct s15_boot *b = m->boot;

	f->title = m->keytest ? "Key Test" : m->title;
	f->version = b->version;
	f->status = 0;
	f->slot = b->running_known ? (uint8_t)b->running_slot : S15_SLOT_UNKNOWN;
	if (m->usb == S15_USB_DISK || m->usb == S15_USB_MTP ||
	    m->usb == S15_USB_MTP_VENDOR)
		f->status |= S15_STATUS_USB;
	f->option_on = m->option_on;
	f->fatal = false;
	f->gray = !m->color;                   /* S-15.173 */
}

void s15_menu_render(struct s15_menu *m, const struct s15_geometry *g,
		     const struct s15_draw_ops *d)
{
	struct s15_frame f;

	s15_menu_frame(m, &f);
	if (m->keytest) {
		s15_render_keytest(&m->kt, g, d, &f);
		return;
	}
	if (m->black_once) {
		/* S-15.065: cleared to black at the key test's end; the frame
		 * stays. The console follows in the next render. */
		m->black_once = false;
		m->changes++;
		s15_render_black(g, d);
		s15_render_frame(&f, g, d);
		return;
	}
	s15_render_console(&m->con, g, d, &f);
}

uint32_t s15_menu_changes(const struct s15_menu *m)
{
	return m->con.changes + m->changes;
}

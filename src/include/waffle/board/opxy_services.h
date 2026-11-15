/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_BOARD_OPXY_SERVICES_H
#define WAFFLE_BOARD_OPXY_SERVICES_H

/*
 * The master's services around the boot flow: the log (S-14), the screen
 * console (S-15), reboots (S-04), the host commands (S-10c), coprocessor
 * revisions (S-17), USB in held mode (opxy_usb.c), and the hooks of the
 * features whose runs live elsewhere (installs, menus). Each ops struct of
 * those documents is filled here, from a driver or adapter where one exists and
 * otherwise from a weak builder its owner overrides; the weak defaults
 * refuse, so a missing feature fails safe.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "blockdev.h"

#include "waffle/boot.h"
#include "waffle/s02_store.h"
#include "waffle/s04_records.h"
#include "waffle/s06_link.h"
#include "waffle/s08_nordic.h"
#include "waffle/s09_files.h"
#include "waffle/s10a_desc.h"
#include "waffle/s10a_usb.h"
#include "waffle/s10b_mtp.h"
#include "waffle/s10c_host.h"
#include "waffle/s11_package.h"
#include "waffle/s11_run.h"
#include "waffle/s14_log.h"
#include "waffle/s15_console.h"
#include "waffle/s15_menu.h"
#include "waffle/board/opxy.h"
#include "waffle/s17_revisions.h"
#include "waffle/wire_fat.h"

struct opxy_master;

/*
 * S-17.032: the bootloader's own version, never the forced marker 0.1.0.
 * The exact version is the release's to set here. A test build may fix
 * all three, so that what its screens show does not move with a release
 * (Makefile OPXY_BL_VERSION).
 */
#ifndef OPXY_BL_VERSION_MAJOR
#define OPXY_BL_VERSION_MAJOR 0u
#define OPXY_BL_VERSION_MINOR 9u
#define OPXY_BL_VERSION_PATCH 17u
#endif
/* A test build may give slot 0 another build number, so that the
 * two slots differ and S-01.002 finds the running one (Makefile
 * OPXY_BL_BUILD). */
#ifndef OPXY_BL_VERSION_BUILD
#define OPXY_BL_VERSION_BUILD 0u
#endif
/* The same four numbers in the image, for its descriptor. */
extern const uint16_t opxy_bl_version[4];

/* S-04.065: the PSoC reset request's id; S-17's info request and reply. */
#define OPXY_PSOC_RESET_ID 0x97u
#define OPXY_INFO_REQUEST  0x09u
#define OPXY_INFO_REPLY    0x0Au

/*
 * The screen's draw ops until the display driver (S-13c) lands: a record
 * of what was drawn, which the host tests read (S-15 needs no more). It
 * is opxy_display_draw_ops()'s default.
 */
#define OPXY_DRAW_TEXT_MAX 48u
struct opxy_draw_rec {
	uint32_t fills, texts;
	uint16_t x, y, w, h, color;		/* the last call's */
	uint8_t text_len;
	char text[OPXY_DRAW_TEXT_MAX];		/* the last text, cut */
};

/*
 * S-12.001's RAM disk over its parts in DDR (src/board/opxy_disk.c): the
 * sectors of part 0, then those of part 1, as one device.
 */
#define OPXY_DISK_PARTS 2u
struct opxy_disk_part {
	uint8_t *mem;
	uint32_t sectors;
};
struct opxy_disk {
	struct wb_blockdev dev;
	struct opxy_disk_part part[OPXY_DISK_PARTS];
};

struct opxy_services {
	/* S-06 */
	struct s06_node_ops node_ops;
	/* S-14 */
	struct s14_clock_ops log_clock;
	struct s14_link_ops log_psoc;
	struct s14_link_ops log_cdc;
	struct s14_screen_ops log_screen;
	struct s14_log log;
	/* S-15: the menus and their console, which is also S-14's screen */
	struct s15_menu menu;
	struct s15_boot boot;
	struct boot_held held;			/* how held mode was entered */
	char bl_text[S11_VERSION_TEXT_MAX + 1];	/* S-17.032 */
	char os_text[S11_VERSION_TEXT_MAX + 1];	/* S-04's record */
	char sw_text[S11_VERSION_TEXT_MAX + 1];
	char ble_text[S11_VERSION_TEXT_MAX + 1];	/* S-17.030, S-17's */
	char psoc_text[S11_VERSION_TEXT_MAX + 1];
	char psoc_serial[S17_SERIAL_MAX + 1];
	/* S-04.047, S-16.095 item 1, read at start-up (opxy_startup_read) */
	char chip_serial[OPXY_CHIP_SERIAL_MAX + 1u];
	bool chip_ok;				/* item 1 read (S-17.015) */
	struct opxy_draw_rec drawn;
	struct s15_draw_ops draw;
	uint32_t menu_shown;			/* s15_menu_changes() last drawn */
	struct s15_env_ops env;
	bool option;				/* S-02.055, for this session */
	bool provisioned;			/* S-16: OTP GP1 */
	struct s15_settings settings;		/* S-15.172, read as held mode starts */
	/* S-09: the slave's /yaffs2 and eMMC, through the bridge */
	struct s09_files files;
	/* S-17 */
	struct s17_link_ops copro_link;
	struct s17_copro copro;
	struct s17_rev rev;
	/* S-04 */
	struct s04_reboot_ops reboot;
	/* S-10c, and S-10a under it (opxy_usb.c) */
	struct s10c_ops host_ops;
	struct s10c_host host;
	struct s10a_usb_board usb_board;
	/* S-12's RAM disk, offered by the disk sets (S-10a.049) */
	struct opxy_disk disk;
	struct wire_fat fat;
	bool disk_formatted;			/* S-12.002: once per boot */
	bool disk_acquired;			/* As last seen */
	enum s10a_state usb_state;		/* the state last started */
	/* S-10b, carried by the MTP sets (opxy_mtp.c) */
	struct s10b mtp;
	struct s10b_volume_ops mtp_volume;
	struct s10b_device_ops mtp_device;
	/* S-11 (the unit ops are opxy_unit_ops; the run's are opxy_install.c's) */
	struct s11_run_ops run;
	struct s11_step_ops step;
	struct s11_fs_ops staging;		/* /yaffs2 through files */
	/* S-11.095: the member's stated size and CRC-32 (S-11.030) of each
	 * image this boot's extraction staged, kept for the run that follows
	 * it (opxy_install.c). `extracted` is false from the moment an
	 * extraction begins until it has staged every member. */
	struct s11_staged_sizes extracted_sizes;
	bool extracted;
	bool scan_due;				/* S-15 started a scan (S-11.045) */
	bool working;				/* a scan or an install is running */
	/* S-02.097: the boot store as this start read it (opxy_store.c) */
	struct s02_store store;
};

/* The log and the menus' console, from the PSoC link's start on (S-14.011,
 * S-14.022): S-07's key events may come from then on (S-15.013). */
void opxy_log_start(struct opxy_master *m);
/* Held mode's services, once its links are up (S-01.016). */
/*
 * The start-up metadata read's second half (S-17.010, S-17.015): the chip
 * serial through get (S-04.047; not read when the record was not), then
 * the reported revision's power-on and outcome. Called once per power-on,
 * after the record's read and before the PSoC link starts.
 */
void opxy_startup_read(struct opxy_master *m, wb_otp_get_fn get);

void opxy_services_start(struct opxy_master *m);
/* One pass: USB, the menus' time, the screen when it changed. */
void opxy_services_poll(struct opxy_master *m);
/* S-15.015: the menus told the time (s15_menu_tick()); before each key
 * event and on every pass. */
void opxy_menu_tick(struct opxy_master *m);
/* Held mode's screen drawn when it changed, then sent to the panel. */
void opxy_menu_show(struct opxy_master *m);
/* The same, with the frame ended on the return: before a long call. */
void opxy_menu_show_now(struct opxy_master *m);
/* S-15.020: held mode's first screen, for the boot h describes. */
void opxy_held_screen(struct opxy_master *m, const struct boot_held *h);
/* S-05.005-S-05.007: the master's stage result as S-05's cause. */
enum s05_fail opxy_stage_fail(const struct s03_load *ld);
/* S-15.163: a failed plain start in S-15's terms; S15_START_OK for
 * S05_FAIL_NONE. */
void opxy_start_view(enum s05_fail cause, const struct boot_start *st,
		     struct s15_start *out);
/* S-05.006: the failed start's error line, with everything st holds. */
void opxy_start_log(struct opxy_master *m, enum s05_fail cause,
		    const struct boot_start *st);
/*
 * The failed start's account for a developer host (waffle/devhost.h),
 * little-endian: attempted:1 cause:1 (enum s05_fail) result:1
 * (S03_LOAD_...) step:1 (S03_STEP_...) step_part:1 part:1 select0_failed:1
 * sector:4 count:4 (a failed image read's) format:4 length:4 crc:4 (the
 * header's) image_crc:4 boot_size:4 refusal:1 (S05_REFUSE_...) replied:1
 * reply:1 lost:1 cmd:1 at:1 (S09_AT_..., S09_LOST_IN_POLL) arg0:4 fault:16
 * (the driver's four words). attempted 0: no start was attempted, and the
 * rest is zeros. Returns OPXY_START_REPORT.
 */
#define OPXY_START_REPORT 61u
size_t opxy_start_report(const struct opxy_master *m,
			 uint8_t out[OPXY_START_REPORT]);
/* S-17.021 step 1: the Nordic's exchange, then the PSoC's reply given. */
void opxy_copro_held(struct opxy_master *m, const uint8_t *psoc,
		     int psoc_len);
/* S-08's ops but the CRC: its log lines, power messages (S-07's), common
 * messages and the revision byte (S-17.011). */
void opxy_nordic_ops(struct opxy_master *m, struct s08_ops *ops);
/*
 * S-07.076, S-08.004: a message from the PSoC or the Nordic outside its
 * own table, on link (S10C_LINK_PSOC or S10C_LINK_NORDIC): a log line is
 * relayed by its source (S-14.040, S-14.041), anything else goes to
 * S-10c's common set, and an id it does not serve is logged.
 */
void opxy_copro_common(struct opxy_master *m, enum s10c_link link,
		       const struct s06_frame *f);
/* S-04.020: a reboot request from writer w with its argument. */
void opxy_reboot(struct opxy_master *m, enum s04_writer w, uint16_t arg);
/* S-04.063's shutdown, its reports on the PSoC line when it returns
 * (S-04.028), waiting at most OPXY_PSOC_DRAIN_US for them. */
void opxy_shutdown(struct opxy_master *m);
/* S-07.051 step 3 after a power-off's 0x32: each enabled UART drained
 * (S-13b.072), the 0x32 the last bytes, then idle until the PSoC removes
 * the power. Does not return. */
_Noreturn void opxy_powered_off(struct opxy_master *m);

/* S-15.095: how long the restart's line stays on the screen before the
 * shutdown blanks it. */
#define OPXY_RESTART_HOLD_MS 750u
/* After a reboot's shutdown reports, the wait before the record and the
 * reset, so that the next start finds the backlight dark: on a unit it was
 * still lit about 400 ms after 0x51 {00 00 02} reached the PSoC. */
#define OPXY_BACKLIGHT_SETTLE_MS 500u

/* The draw ops that record (struct opxy_draw_rec). */
void opxy_draw_record(struct opxy_draw_rec *r, struct s15_draw_ops *ops);

/*
 * USB in held mode (src/board/opxy_usb.c): U1's device port and S-10a, and
 * the master's CDC and MIDI links on it (S-06.011), whose frames for the
 * master go to S-10c.
 */
/* S-10c's transports: the CDC and MIDI links, the CDC port's state, and
 * S-11.050's transfer storage (opxy_transfer.c). */
void opxy_usb_host_ops(struct opxy_master *m, struct s10c_ops *ops);
/* The links, S-10a and the controller; after s10c_host_init(). */
void opxy_usb_start(struct opxy_master *m);
/* One pass: the device stack, S-10a's bytes, the links' frames to S-10c. */
void opxy_usb_poll(struct opxy_master *m);
/* S-15's set names as S-10a.010's states. */
enum s10a_state opxy_usb_state(enum s15_usb_set set);
/* S-10a.010: start the set the state starts, if any (detach, select,
 * attach). Also for the battery wait (S10A_STATE_BATTERY). */
void opxy_usb_select(struct opxy_master *m, enum s10a_state state);
/* Whether the controller came up. */
bool opxy_usb_up(void);
/*
 * The bus serviced from inside a long operation, at most about once a
 * millisecond: the device stack's own work, no frame taken. Safe anywhere:
 * nothing before USB is up, nothing from inside the stack itself.
 */
void opxy_usb_breathe(void);
/* The longest time between two services of the bus from _begin() to
 * _end(), in microseconds. */
void opxy_usb_gap_begin(void);
uint32_t opxy_usb_gap_end(void);
/*
 * S-10a.049: the host hold, from the begin to the end of a long operation.
 * Nothing the host sends on CDC or MIDI is read, forwarded or handed to
 * S-10c meanwhile, and no MTP request runs: it waits in USB's receive
 * rings, the host held by flow control, and the loop takes it in order
 * afterwards. What goes to the host is not affected. Pairs nest.
 */
void opxy_host_hold_begin(struct opxy_master *m);
void opxy_host_hold_end(struct opxy_master *m);
bool opxy_host_held(void);
/* S-07.054: the battery wait's USB mass storage on (true) or off. */
void opxy_usb_battery(struct opxy_master *m, bool on);

/* The disk over the parts given; opxy_disk_ddr() gives opxy.h's. */
void opxy_disk_init(struct opxy_disk *d,
		    const struct opxy_disk_part part[OPXY_DISK_PARTS]);
void opxy_disk_ddr(struct opxy_disk_part part[OPXY_DISK_PARTS]);
/* S-12.002: formats the volume if this boot has not (FatFs is unmounted
 * again); false when the format fails. Write it only while the disk is not
 * offered (S-12.018): S-12.017's files call it first, since S-15 asks for
 * them before it starts the disk's set (S-15.100). */
bool opxy_disk_ready(struct opxy_master *m);
/* S-10a.049: withdraws the disk and waits for the host's command in
 * progress; false when it does not end. Nothing of a WRITE is stored after
 * the withdrawal either way; on false the volume is not used. */
bool opxy_disk_take(struct opxy_master *m, enum s10a_withdrawal why);

/* S-11.050, S-12.024: a host-link transfer's file on the RAM disk. */
bool opxy_transfer_start(struct opxy_master *m);
bool opxy_transfer_append(struct opxy_master *m, const uint8_t *p, size_t n);
void opxy_transfer_discard(struct opxy_master *m);
void opxy_transfer_submit(struct opxy_master *m, bool fw_command,
			  struct s11_outcome *out);

/*
 * Weak builders, one per feature owner. Each fills its part of an ops
 * struct; the defaults here refuse everything.
 *  - S-11's runs: the install steps and run ops;
 *  - S-15's menus: the environment of the menus (its USB sets are wired);
 *  - S-13c (D2): the screen's geometry (NULL: nothing is drawn yet), its
 *    draw ops (the recorder by default), the display's start in held
 *    mode, before the first screen (S-15.021: no menu frame before the
 *    panel's DISPON of held mode, S-13c.019), and its power-off at the
 *    start of held mode's shutdown (S-13c.022): no frame from there, the
 *    frame in progress ends, and nothing more is sent, no DISPOFF; the
 *    PSoC's reports follow it (S-04.063);
 */
/*
 * S-12.017's system-information files on the RAM disk, then S-14.051's log
 * export from the slave's /yaffs2 logs/ (S-15's trigger, S-15.100). The
 * disk is formatted first if it is not yet, and a disk that is offered is
 * withdrawn while they are written and offered again after (S-12.018).
 */
void opxy_system_info(struct opxy_master *m);
/* The RTC's time, seconds since 1970 (UTC), for S-12.016's stamps; false
 * when it cannot be read. */
bool opxy_rtc_utc(struct opxy_master *m, uint64_t *utc);
/* RTC0's count register as S-04.070 encodes it (src/rtc_board.c); until
 * held mode's opxy_rtc_board_start() the read fails and the write is
 * dropped (S-13a.070). */
bool opxy_rtc_read(uint32_t *clk);
/* RTC0's count register as it holds it now, without waiting for the RTC's
 * first second; false only before opxy_rtc_board_start(). */
bool opxy_rtc_peek(uint32_t *clk);
void opxy_rtc_write(uint32_t clk);
/* S-09.046: the 0xBD clock message to the slave from RTC0 as it reads
 * now (opxy_rtc_peek(): a stopped RTC's value too), sent when its seconds
 * differ from the last sent. */
void opxy_clock_to_slave(struct opxy_master *m);
/* S-10b's store and device on the MTP sets, after s10a_usb_init(). */
void opxy_mtp_start(struct opxy_master *m);
/* S-04.043: the metadata record written from m->meta (M1-M4, M7). */
bool opxy_meta_write(struct opxy_master *m);
/*
 * S-02.096 - S-02.099, S-15.175 (src/board/opxy_store.c): the boot store on
 * the master's flash. _load() reads it into sv.store, once per start, before
 * the boot decision; _save() writes it and gives in *now what a start would
 * read afterwards.
 */
void opxy_boot_store_load(struct opxy_master *m);
enum s15_store_save opxy_boot_store_save(struct opxy_master *m,
					 const struct s02_store *s,
					 struct s02_store *now);
/* S-15.172: the settings file, read once as held mode starts; a missing
 * or unusable file, an access error and a refused volume give the
 * defaults. */
void opxy_settings_read(struct opxy_master *m, struct s15_settings *s);
/* S-10a's serial flag (S-04.007): reason 10, developer mode (S-15.172),
 * an unprovisioned unit or the debug key held at start. */
bool opxy_serial_flag(const struct opxy_master *m);
void opxy_install_ops(struct opxy_master *m, struct s11_run_ops *run,
		      struct s11_step_ops *step);
/* The run's step values (S-11.055, S-11.073, S-11.090, S-11.096) and the
 * staging area as the run finds it, before the run's first step;
 * confirmed has S11_STEP_BIT() of each protected step confirmed on the
 * unit. */
void opxy_install_begin(struct opxy_master *m, bool option, bool forced,
			uint8_t confirmed);
/* S-11.073: the steps this build has switched off, S11_STEP_BIT() each. */
uint8_t opxy_steps_off(void);
/*
 * S-11.020's crypto on this board (src/board/opxy_crypto.c): SHA-256 and
 * AES on the processor's packet engine when it answers a test of each as
 * the software does, the software otherwise and after any failure. Each
 * call tests the engine again; opxy_crypto_engine() is whether it is in
 * use. opxy_crypto_end() ends the use: the engine is left as reset leaves
 * it, with no key, and the ops are the software's from then on.
 */
const struct s11_crypto_ops *opxy_crypto(struct opxy_master *m);
bool opxy_crypto_engine(void);
void opxy_crypto_end(void);
/* The engine's records (tests model the engine on them). */
struct wb_pkte;
extern struct wb_pkte opxy_pkte;
/*
 * The package's CRC-16 (s11_crypto_ops.crc16) and CRC-32
 * (s11_zip_ops.crc32) on the processor's CRC peripheral when
 * opxy_crc_begin() has found it to answer as the software does, the
 * software otherwise and after any failure (src/board/opxy_crc.c); ctx is
 * not used. opxy_crc_engine(OPXY_CRC16 or OPXY_CRC32) is whether the
 * peripheral serves that CRC; opxy_crc_end() leaves it as reset does and
 * the functions the software's.
 */
enum { OPXY_CRC16 = 0, OPXY_CRC32 = 1, OPXY_CRCS = 2 };
uint16_t opxy_crc16(void *ctx, uint16_t crc, const uint8_t *p, size_t n);
uint32_t opxy_crc32(void *ctx, uint32_t crc, const uint8_t *p, size_t n);
void opxy_crc_begin(struct opxy_master *m);
bool opxy_crc_engine(unsigned which);
void opxy_crc_end(void);
/* The CRCs the peripheral serves no more this boot, bit (1 << which), and
 * its driver's record (tests model the peripheral on them). */
extern uint8_t opxy_crc_off;
struct wb_crc;
extern struct wb_crc opxy_crc_dev;

/* S-11.020 for one package, with the upgrade screen's lines: checks 1-17
 * through pkg and zip, then the run (src/board/opxy_install.c). */
void opxy_install_package(struct opxy_master *m,
			  const struct s11_package_ops *pkg,
			  const struct s11_zip_ops *zip,
			  const struct s11_check_in *in,
			  struct s11_install_result *out);
/* S-11.045-S-11.049: the RAM disk's scan, which S-15 starts (S-15.105). */
void opxy_scan(struct opxy_master *m);
/* One package read from src, its zip recovered in place through src->write
 * (S-11.018; wire_fat_open_package) and staged over S-09, then installed. */
void opxy_install_source(struct opxy_master *m, const struct wire_source *src,
			 const struct s11_check_in *in,
			 struct s11_install_result *res);
void opxy_menu_env(struct opxy_master *m, struct s15_env_ops *env);
const struct s15_geometry *opxy_display_geometry(void);
void opxy_display_draw_ops(struct opxy_master *m, struct s15_draw_ops *ops);
void opxy_display_held_start(struct opxy_master *m);
/* The set opxy_usb_select() last started; S10A_SET_COUNT before any. */
enum s10a_set opxy_usb_set(void);
/* The frame buffer (S13C_FRAME_BYTES), NULL while the panel bus is down. */
const uint8_t *opxy_display_fb(void);
/* OPXY_OS_FRAME_START's bytes, where the boot path leaves the logo. */
const uint8_t *opxy_display_os_frame(void);
/* The frame drawn since the last one sent now, and ended on the return. */
void opxy_display_show(void);
/* The frame drawn since the last one sent now, not waited for. */
void opxy_display_send(void);
/* The frame drawn last on the panel, waiting no longer than one frame's
 * timeout (S-13c.016). */
void opxy_display_settle(void);
void opxy_display_power_off(struct opxy_master *m);
/* S-10c.057: the FSN's four words; false while no OTP item is known. */
bool opxy_read_fsn(uint32_t words[4]);

#endif /* WAFFLE_BOARD_OPXY_SERVICES_H */

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The check suite (wiki/Test-OS.md, "The check suite"): the test OS's
 * `check` variant, built with -DOPXY_TESTOS_CHECK=1. It runs hardware tests
 * under the unit's own bootloader, shows a summary, writes check.txt to the
 * USB disk, and changes as little as it can: at most the active byte of the
 * two boot partitions' headers, and one temporary file on the eMMC.
 *
 * The parts here are pure and are tested on the host: the test table and
 * the verdicts, the switch-back over S-03's eMMC operations, the guard
 * those operations go through, the eMMC file test, the summary's pages and
 * the report. The board's runner (src/testos/check_run.c) collects
 * the facts and calls them.
 *
 * What reaches a screen or check.txt is built only by the formatters below
 * from the facts' allowed fields: never a serial, OTP content, the eMMC's
 * CID, calibration, flash bytes, or a CRC or length of the first stage.
 * (The installer's survey build and its lock on an unknown device add the
 * first stage's n and c of S-18.003, and the survey the slots' records,
 * in the install part: waffle/testos/installer.h.)
 *
 * The suite is the device's (S-18.020): on a device without second-cpu
 * the slave and bridge tests are not run and read "not on this device",
 * which counts as a pass, and the eMMC tests run on the main CPU's own
 * storage through the same bridge client (s09_master_local()).
 */
#ifndef WAFFLE_TESTOS_CHECK_H
#define WAFFLE_TESTOS_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/boot/device.h"
#include "waffle/emmc/emmc.h"
#include "waffle/ui/console.h"
#include "waffle/testos.h"

/* --- The tests --- */

enum tchk_id {
	TCHK_OS_PARTITION,	/* the switch-back; not part of the hardware verdict */
	TCHK_CLOCKS,
	TCHK_DDR,
	TCHK_CORE_STATE,
	TCHK_FLASH_ID,
	TCHK_FLASH_LAYOUT,
	TCHK_PSOC,
	TCHK_SLAVE,
	TCHK_BRIDGE,
	TCHK_EMMC_CONFIG,
	TCHK_EMMC_BOOT,
	TCHK_EMMC_VOLUME,
	TCHK_EMMC_FILE,
	TCHK_PANEL,
	TCHK_RTC,
	TCHK_NORDIC,
	TCHK_TIMING,
	TCHK_KEYS,		/* guided: com and the recovery key */
	TCHK_COUNT
};

enum tchk_kind {
	TCHK_KIND_OS,		/* reported apart from the hardware */
	TCHK_KIND_AUTO,		/* not tested: a gap in the verdict */
	TCHK_KIND_GUIDED	/* not tested (skipped): no gap */
};

enum tchk_state {
	TCHK_NOT_RUN = 0,
	TCHK_PASS,
	TCHK_FAIL,
	TCHK_NOT_TESTED,
	TCHK_NOT_PASSED,	/* never set; kept for its number */
	TCHK_NOT_ON_DEVICE	/* the device has no such part (S-18.020): no gap */
};

struct tchk_info {
	const char *id;		/* as in check.txt */
	const char *name;	/* on screen: at most S15_STEP_LABEL_MAX */
	uint8_t kind;
};
extern const struct tchk_info tchk_tests[TCHK_COUNT];

/* A detail line: the screen shows TCHK_DETAIL_COLS; `more` is the report's. */
#define TCHK_DETAIL_COLS S15_COLS
#define TCHK_MORE_MAX 160u
struct tchk_text {
	char s[TCHK_MORE_MAX + 1u];
	uint16_t n, cap;
	bool cut;
};

/* The allow-list of formatters: nothing else writes a detail. */
void tchk_text_init(struct tchk_text *t, uint16_t cap);
void tchk_put_str(struct tchk_text *t, const char *fixed);
void tchk_put_dec(struct tchk_text *t, uint32_t v);
void tchk_put_hex(struct tchk_text *t, uint32_t v, unsigned digits);
/* bytes over us as "N.N MB/s" (10^6 bytes) */
void tchk_put_rate(struct tchk_text *t, uint64_t bytes, uint64_t us);
/* us as "N ms", or "N.N ms" below 100 ms */
void tchk_put_ms(struct tchk_text *t, uint32_t us);

struct tchk_result {
	uint8_t state;
	struct tchk_text detail;	/* cap TCHK_DETAIL_COLS */
	struct tchk_text more;		/* cap TCHK_MORE_MAX, may be empty */
};

struct tchk_table {
	struct tchk_result r[TCHK_COUNT];
	/* emmc-boot's fault lines: [0] the select, [1] the read; may be empty */
	struct tchk_text emmc_fault[2];
};

void tchk_table_init(struct tchk_table *t);
/* A test's outcome: the state and its detail, `more` emptied. */
struct tchk_text *tchk_set(struct tchk_table *t, enum tchk_id id, enum tchk_state s);

/* The hardware verdict: every test but os-partition. FAIL if any failed;
 * else GAPS if an automatic test was not tested; else PASS. */
enum tchk_verdict { TCHK_VERDICT_PASS, TCHK_VERDICT_GAPS, TCHK_VERDICT_FAIL };
enum tchk_verdict tchk_verdict(const struct tchk_table *t);
const char *tchk_verdict_word(enum tchk_verdict v);
/* "pass", "FAIL", "not tested", "not passed", "n/a" */
const char *tchk_state_word(enum tchk_state s);

/* --- The guard on the eMMC operations the suite uses (S-03) --- */

/*
 * What the suite hands the switch-back and the read tests in place of the
 * bridge's eMMC operations: a boot partition's write is sector 0 and one
 * sector or it is refused; every erase is refused; every write while the
 * user area (or an unknown partition) is selected is refused (files go
 * through the file layer, not here); of the switches only S-03.068's flush
 * passes. breathe, if set, runs before each operation (the links' service).
 */
struct tchk_guard {
	const struct s03_emmc_ops *in;
	unsigned sel;		/* the partition selected, as far as known */
	bool sel_known;
	uint32_t refused;
	void (*breathe)(void *ctx);
	void *breathe_ctx;
	struct s03_emmc_ops ops;
};
void tchk_guard_init(struct tchk_guard *g, const struct s03_emmc_ops *in);

/* --- The switch-back (S-03.040, S-03.042, S-03.058) --- */

enum tchk_sw_result {
	TCHK_SW_NOT_RUN = 0,
	TCHK_SW_RESTORED,	/* the loader takes the other partition now */
	TCHK_SW_NO_OTHER_OS,	/* the other header is not an OS's: no write */
	TCHK_SW_OTHER_BAD,	/* its CRC differs: no write */
	TCHK_SW_WRITE_FAILED,	/* a write or its read-back failed; the suite still boots */
	TCHK_SW_IO,		/* a select or read failed: no write */
	TCHK_SW_NOT_ACTIVE,	/* neither active (S-03.040 says the suite cannot run so) */
	TCHK_SW_NO_SLAVE,	/* the eMMC is not reachable */
	TCHK_SW_READY		/* looked: the OS is there, nothing written yet */
};

/* How the other partition's header looked, in words that identify nothing. */
enum tchk_look {
	TCHK_LOOK_UNREAD = 0,
	TCHK_LOOK_ZERO,		/* all 512 bytes 0x00 */
	TCHK_LOOK_FF,		/* all 0xFF */
	TCHK_LOOK_INACTIVE,	/* a header that passes S-03.042, inactive */
	TCHK_LOOK_ACTIVE,	/* the same, active */
	TCHK_LOOK_OTHER		/* anything else */
};

struct tchk_switch {
	uint8_t result;		/* enum tchk_sw_result */
	unsigned mine, other;	/* S03_PART_BP1/BP2, 0 while unknown */
	uint8_t look;		/* enum tchk_look */
	bool unverified;	/* the other is stored encrypted: no CRC check */
	bool wrote_a;		/* the other's byte 0x0C set and read back */
	bool wrote_b;		/* the suite's byte 0x0C cleared and read back */
	bool b_failed;		/* (b) failed with the OS already taken: a note */
	bool undo_failed;	/* (a) failed and writing it back failed too */
	bool select0_failed;	/* the last select of the user area failed */
	unsigned takes;		/* the loader's choice after it: 0 none */
	/* Kept for emmc-boot, never shown: the headers as first read. */
	bool headers_read;
	struct s03_header h[2];	/* [0] BP1, [1] BP2 */
	/* the headers as first read, for the write; on a cache line (S-13b.054) */
	_Alignas(32) uint8_t raw[2][S03_SECTOR];
	bool other_crc_checked, other_crc_ok;
};

/*
 * Over env (whose ops are a guard's), each ending with select 0:
 * tchk_switch_look() reads the headers and checks the OS, writing nothing,
 * and leaves TCHK_SW_READY when the OS can be made active;
 * tchk_switch_write() then makes it active, on a READY look alone;
 * tchk_switch_back() is the two in a row.
 */
void tchk_switch_look(const struct s03_env *env, struct tchk_switch *out);
void tchk_switch_write(const struct s03_env *env, struct tchk_switch *out);
void tchk_switch_back(const struct s03_env *env, struct tchk_switch *out);
/* S-03.040 on two raw headers: S03_PART_BP1, S03_PART_BP2 or 0. */
unsigned tchk_loader_takes(const uint8_t bp1[S03_SECTOR], const uint8_t bp2[S03_SECTOR]);
enum tchk_look tchk_header_look(const uint8_t raw[S03_SECTOR], uint32_t boot_size);

/* --- emmc-boot: every sector of both boot partitions --- */

/* Why the active image's CRC was not checked. */
enum tchk_unchecked {
	TCHK_UNCHECKED_NONE = 0,	/* checked */
	TCHK_UNCHECKED_ENCRYPTED,	/* its header says stored encrypted */
	TCHK_UNCHECKED_NO_IMAGE,	/* no active partition, or its header refused */
	TCHK_UNCHECKED_READ		/* a read inside the image failed */
};

struct tchk_boot {
	bool run;
	uint32_t sectors[2];	/* read without error */
	uint32_t errors[2];
	uint32_t first_bad[2];	/* the first sector of the first failed read */
	uint64_t bytes, us;	/* for the bridge's eMMC rate */
	bool mine_checked, mine_ok;	/* the active image's CRC, when plaintext */
	uint8_t unchecked;	/* enum tchk_unchecked */
	bool select0_failed;
};
/*
 * The first failing boot-partition select ([0]) and sector read ([1]) as
 * the eMMC's store recorded them (struct opxy_emmc_note): where the request
 * stopped, the driver's first fault (struct wb_msi_fault: its step, the
 * command and the one that failed, the response, the controller's status,
 * the busy wait so far) and EXT_CSD[179] before and after.
 */
enum tchk_fault_why { TCHK_FW_DRIVER, TCHK_FW_DOWN, TCHK_FW_UNKNOWN, TCHK_FW_REFUSED };
enum tchk_fault_step {
	TCHK_FS_NONE, TCHK_FS_ARG, TCHK_FS_OFF, TCHK_FS_CMD, TCHK_FS_R1, TCHK_FS_BUSY,
	TCHK_FS_SWITCH, TCHK_FS_STATE, TCHK_FS_DATA, TCHK_FS_DATA_TIME, TCHK_FS_COUNT
};
#define TCHK_PC_BEFORE 0x01u
#define TCHK_PC_AFTER  0x02u
struct tchk_emmc_fault {
	bool seen;
	uint8_t part, why, pc_flags, pc_before, pc_after;
	uint32_t sector, count;		/* a read's */
	uint8_t step, cmd, at, state;	/* state 0xFF: no CMD13 */
	int8_t rc;
	uint32_t arg, r1, istat, idsts, busy_us;
};

/*
 * A fault as TCHK_FAULT_BYTES bytes, little-endian, for the survey's reply:
 * [0] seen, [1] part, [2] why, [3] pc_flags, [4] pc_before, [5] pc_after,
 * [6] step, [7] cmd, [8] at, [9] state, [10] rc, [11] 0, then sector,
 * count, arg, r1, istat, idsts and busy_us, four bytes each.
 */
#define TCHK_FAULT_BYTES 40u
void tchk_fault_bytes(const struct tchk_emmc_fault *e, uint8_t out[TCHK_FAULT_BYTES]);

/* now_us may be NULL (no rate). sw gives the headers and the other's CRC. */
void tchk_emmc_boot(const struct s03_env *env, const struct tchk_switch *sw,
		    uint64_t (*now_us)(void), struct tchk_boot *out);

/* --- emmc-file: one temporary file on /yaffs2 --- */

#define TCHK_FILE_PATH "/yaffs2/waffle-check.tmp"
#define TCHK_FILE_BYTES 0x10000u
#define TCHK_FILE_PIECE 0x1000u

/* The file layer, each call as S-09's file requests answer: negative fails. */
struct tchk_file_ops {
	void *ctx;
	int32_t (*open)(void *ctx, const char *path, uint32_t flags);
	int32_t (*write)(void *ctx, int32_t fd, const uint8_t *p, uint32_t n);
	int32_t (*read)(void *ctx, int32_t fd, uint8_t *p, uint32_t n);
	int32_t (*close)(void *ctx, int32_t fd);
	int32_t (*unlink)(void *ctx, const char *path);
	int32_t (*exists)(void *ctx, const char *path);	/* 1 yes, 0 no, negative failed */
};
enum tchk_file_step {
	TCHK_FILE_OK = 0, TCHK_FILE_LEFTOVER, TCHK_FILE_CREATE, TCHK_FILE_WRITE,
	TCHK_FILE_CLOSE, TCHK_FILE_OPEN, TCHK_FILE_READ, TCHK_FILE_COMPARE,
	TCHK_FILE_UNLINK, TCHK_FILE_GONE
};
struct tchk_file {
	uint8_t failed;		/* enum tchk_file_step, TCHK_FILE_OK when it passed */
	bool leftover;		/* a file from an earlier run was removed first */
	uint32_t written, compared;
};
/* buf: at least TCHK_FILE_PIECE bytes. */
void tchk_file_test(const struct tchk_file_ops *ops, uint8_t *buf, struct tchk_file *out);

/* --- flash-layout: the first stage, as far as spec/ defines it --- */

/*
 * S-02.002, S-01.020, S-05's block headers: an LDR stream at offset 0 whose
 * every header carries the 0xAD signature and a zero XOR, ends with FINAL
 * within the first stage's 0xB000 bytes, and whose every block code has the
 * low nibble 4. read has struct s02_flash_ops's shape. Its length and count
 * stay here: they describe code that is not this project's.
 */
struct tchk_stage {
	bool parsed;		/* headers sound up to FINAL, inside 0xB000 */
	bool codes_ok;		/* every BCODE 4 */
	uint32_t len, blocks;	/* never shown */
};
void tchk_first_stage(int (*read)(void *ctx, uint32_t off, uint8_t *buf, uint32_t len),
		      void *ctx, struct tchk_stage *out);

/* --- The Nordic link's gate --- */

/*
 * Whether a frame written to the Nordic link (its wire bytes, COBS and the
 * delimiter, S-06.004) may go: the link greeting 0x05 {01 03}, the answer
 * 0x06 {40}, the info request 0x09 {}, a ping's answer 0x02. Nothing else:
 * no 0x0D (DFU's enter or exit) and no DFU request (S-08.030 to S-08.046).
 * *id gets the frame's id, 0xFFFF for bytes that are no frame.
 */
bool tchk_nordic_allowed(const uint8_t *wire, size_t n, unsigned *id);
/*
 * S-08.011's state 0, as held mode has it on a unit in use: a greeting from
 * the Nordic's bootloader (0xBB) is answered 0x0D {18}, which has it start
 * its application; every other greeting 0x06 {40}. A boot to the OS answers
 * no greeting (S-08.001), so a Nordic still in its bootloader at the OS's
 * entry is left for the OS to end.
 * SPEC-Q: whether the Nordic's bootloader also starts its application on its
 * own after a time, and why some starts find it in its application and
 * others in its bootloader, is not stated.
 */
#define TCHK_NORDIC_STATE 0

/* --- The facts the runner collects --- */

/* S-13a's OSENTRY rows, split by requirement: clocks .002-.006, DDR
 * .008-.017, the USB PLL .080 (a register no stage writes: reported, not
 * judged), the rest the core state. */
enum { TCHK_REGS_CLOCKS, TCHK_REGS_DDR, TCHK_REGS_CORE, TCHK_REGS_USB_PLL, TCHK_REGS_GROUPS };
struct tchk_regs {
	unsigned checked, differ;
	const struct opxy_testos_reg *first;	/* the first that differs */
	uint32_t first_value;			/* its field as read */
};
unsigned tchk_reg_group(const char *req);
void tchk_regs_judge(const struct opxy_testos_reg *rows, unsigned n, const uint32_t *values,
		     struct tchk_regs out[TCHK_REGS_GROUPS]);

/* Guided outcomes. */
enum { TCHK_G_NOT_RUN = 0, TCHK_G_SKIPPED, TCHK_G_PASSED, TCHK_G_FAILED, TCHK_G_ENDED,
       TCHK_G_EARLIER	/* passed at the start that wrote the trial: not asked again */ };
/* The guided key check's keys (S-07.030's numbers) on the OP-XY: com is
 * its device.key.com, M4 its device.key.recovery; on another device those
 * two roles (tchk_key_com(), tchk_key_recovery()). */
#define TCHK_KEY_COM 0x18u
#define TCHK_KEY_M4  0x09u
static inline unsigned tchk_key_com(const struct wb_device *dev)
{
	return dev != NULL ? dev->keys.com : TCHK_KEY_COM;
}
static inline unsigned tchk_key_recovery(const struct wb_device *dev)
{
	return dev != NULL ? dev->keys.recovery : TCHK_KEY_M4;
}

#define TCHK_INFO_MAX 128u
struct tchk_facts {
	/* the device (S-18): NULL reads as the OP-XY */
	const struct wb_device *dev;
	uint32_t ran;			/* bit per enum tchk_id: its facts are in */
	/* the bridge was up: to the slave, or on a device without second-cpu
	 * to the main CPU's own eMMC, which came up */
	bool slave;
	struct tchk_switch sw;
	struct tchk_regs regs[TCHK_REGS_GROUPS];
	bool ddr_aliased;
	/* flash-id */
	bool flash_id_read;
	uint8_t jedec[3];
	bool flash_sr_read;		/* RDSR answered, after the id */
	uint8_t flash_sr;		/* its block-protect bits are reported */
	/* flash-layout: only words of these reach the report */
	bool flash_read;
	bool meta_magic_ok;
	bool flash_late;		/* the metadata's magic one byte further on */
	uint8_t sel_index, sel_verified;
	bool slot_good[2];
	uint32_t slot_crc[2], slot_len[2];	/* never shown */
	struct tchk_stage fs;			/* the first stage's stream */
	uint64_t flash_bytes, flash_us;
	/* psoc */
	bool psoc_greeted, psoc_ping, psoc_keys, psoc_battery;
	bool psoc_start_greeted;	/* the start-up's greeting answered */
	bool psoc_asked;		/* not greeted at the test: greeted again by the test */
	uint8_t psoc_greet_failed;	/* greeting sequences that ended ungreeted since start-up */
	uint8_t psoc_info[TCHK_INFO_MAX];	/* the raw 0x09 reply: serial included */
	uint16_t psoc_info_len;
	/* slave */
	bool slave_echo;
	int slave_failed_row;			/* -1 none */
	uint8_t slave_failed_code;
	uint16_t slave_ms[OPXY_TESTOS_STEP_COUNT];	/* 0xFFFF not run */
	/* bridge */
	uint64_t link_bytes, link_us;
	uint32_t link_bad, link_err;
	/* emmc-config: EXT_CSD as the slave read it at its bring-up, before
	 * S-03.009's switches; features: the slave's record that they were made */
	bool ext_read;
	uint32_t sec_count, erase_group;
	uint8_t ext33, ext34, ext163, ext179, ext183, ext226, ext265;
	bool features;
	/* emmc-boot */
	struct tchk_boot boot;
	struct tchk_emmc_fault emmc_fault[2];	/* [0] select, [1] read */
	/* emmc-volume */
	bool mounted, space_read;
	uint64_t free_bytes;
	/* emmc-file */
	struct tchk_file file;
	/* panel */
	bool panel_up, panel_id;
	uint8_t rddid[3], rdid4[3];
	bool te_seen;
	uint32_t te_us, te_misses, frame_timeouts;
	bool pattern_drawn;
	/* rtc */
	bool rtc_read, rtc_ticked;
	uint32_t rtc_us;
	/* nordic */
	bool nordic_greeting, nordic_info;
	/* the mode at the suite's start, before the long reads: 0 no answer,
	 * 1 its application, 2 its bootloader */
	uint8_t nordic_start_mode;
	uint8_t nordic_left;			/* 0x0D {18} sent: "leave the bootloader" */
	uint8_t nordic_reply[TCHK_INFO_MAX];	/* the raw info reply */
	uint16_t nordic_reply_len;
	uint32_t nordic_refused;
	/* timing */
	uint32_t held_ms;			/* entry to the suite's start */
	/* guided */
	uint8_t keys;
	uint8_t keys_seen;			/* bit 0 com, bit 1 the recovery key (M4) */
	/* PA2's level, 0 or 1: the strap's on a device with straps, else read
	 * where the device's pins make it an input, never driving PA0 or PA8;
	 * 0xFF not read. The survey's (waffle/testos/installer.h). */
	uint8_t pa2;
};

/* Every test's outcome from the facts. */
void tchk_judge(const struct tchk_facts *f, struct tchk_table *t);

/* --- The summary and the report --- */

/* The first screen's lines while the tests run. */
#define TCHK_FIRST_LINES 2u
extern const char *const tchk_first_lines[TCHK_FIRST_LINES];

/* What a pass does not cover. */
#define TCHK_COVER_LINES 5u
unsigned tchk_cover_lines(const char *out[TCHK_COVER_LINES]);

/* The summary's pages: the verdict, the tests four a page, the cover. */
#define TCHK_PER_PAGE 4u
#define TCHK_TEST_PAGES ((TCHK_COUNT + TCHK_PER_PAGE - 1u) / TCHK_PER_PAGE)
#define TCHK_PAGES (TCHK_TEST_PAGES + 2u)
struct tchk_line {
	uint8_t style;		/* enum s15_style */
	char s[S15_LINE];
};
/* Page p's lines (at most S15_ROWS), each fitting the 56 columns. */
unsigned tchk_page(const struct tchk_table *t, const struct tchk_switch *sw, unsigned page,
		   struct tchk_line out[S15_ROWS]);

/* check.txt: the whole of it into out; returns its length. version is
 * waffle-boot's, "1.0.0". */
#define TCHK_REPORT_MAX 8192u
size_t tchk_report(const struct tchk_table *t, const struct tchk_switch *sw, const char *version,
		   char *out, size_t cap);

/*
 * check.txt is rewritten as the tests go on, while a host may hold the
 * disk; a host that keeps the file's earlier size cuts a longer rewrite
 * short. So the file is always TCHK_REPORT_LEN bytes: the text of n bytes
 * in buf, then a last line of spaces and a newline. Returns the length to
 * write: TCHK_REPORT_LEN, or n when the text does not leave room.
 */
#define TCHK_REPORT_LEN 7168u
size_t tchk_report_pad(char *buf, size_t n, size_t cap);

#endif /* WAFFLE_TESTOS_CHECK_H */

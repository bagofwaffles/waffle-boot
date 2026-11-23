/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The installer (wiki/Installer-Reference.md): the test OS's `installer` variant, built
 * with -DOPXY_TESTOS_CHECK=1 -DOPXY_TESTOS_INSTALLER=1. It runs the check
 * suite (waffle/board/testos_check.h), and only when every check passes it
 * offers to write the waffle-boot image it carries: a trial first, then,
 * after a trial start that reached the new boot menu, the commit. An OS
 * found in the other boot partition is made active again when it ends; what
 * that partition holds plays no part in the offer. Every write is started on
 * the unit by holding its key for 5 s; over USB it only reports.
 *
 * The parts here are pure and are tested on the host: the hold, the lock,
 * the check of the carried image, the screens, and the USB protocol. The
 * board's runner is src/testos/opxy_testos_installer.c; the write engine is
 * src/testos/installer_engine.c.
 *
 * The device (S-18) is decided at the entry, before any pin is touched.
 * The install is offered on a first stage that S-18.003's table names
 * only. One image serves both devices (make installer, S-05.043), so the
 * image's device is the one decided; INST_LOCK_OTHER_DEVICE stays for a
 * runner that names another. On the OP-1F it never writes over the last
 * slot whose image is not a waffle-boot stream: the bootloader the unit
 * came with stays in one slot. The survey build (make installer-survey)
 * writes nothing to the SPI flash, and shows and saves what an install
 * would depend on (struct inst_survey).
 */
#ifndef WAFFLE_INSTALLER_H
#define WAFFLE_INSTALLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/board/testos_check.h"
#include "waffle/s15_console.h"
#include "waffle/s15_keytest.h"
#include "waffle/testos.h"

/* --- What the installer is doing (the protocol's phase) --- */

enum inst_phase {
	INST_STARTING,
	INST_CHECKING,		/* the automatic tests */
	INST_GUIDED,		/* a guided test on the screen (hold 8: return to the old OS) */
	INST_LOCKED,		/* a check did not pass: no write offered */
	INST_READY,		/* the trial offered (hold 1) */
	INST_WRITING_TRIAL,
	INST_TRIAL_WRITTEN,	/* switch off and on for the trial start */
	INST_COMMIT_READY,	/* the trial reached the boot menu (hold 1, or 2 again) */
	INST_COMMITTING,
	INST_DONE,
	INST_FAILED,		/* a write failed */
	INST_TRIAL_FAILED,	/* the trial start did not reach the boot menu */
	INST_LEFT,		/* left without installing */
	INST_SURVEYED		/* the survey build's end: what it found */
};

#define INST_F_CHECKS_DONE 0x01u
#define INST_F_UNLOCKED    0x02u
#define INST_F_IMAGE_OK    0x04u
#define INST_F_CRUMBS      0x08u
#define INST_F_SURVEY      0x10u	/* a survey build: no SPI-flash write at all */

enum inst_lock {
	INST_LOCK_NONE,
	INST_LOCK_FAILED,	/* a check failed */
	INST_LOCK_NOT_TESTED,	/* an automatic check could not run */
	INST_LOCK_IMAGE,	/* the carried image did not verify */
	INST_LOCK_SELECTED,	/* the selected slot is not verified or not good */
	INST_LOCK_RESERVED,	/* never set; the numbers after it stay */
	INST_LOCK_RESERVED_6,	/* never set: a key check ended with 8 offers no install */
	INST_LOCK_EMMC,		/* the eMMC could not be reached */
	INST_LOCK_DEVICE,	/* the first stage is in no row of S-18.003's table */
	INST_LOCK_OTHER_DEVICE,	/* this image is built for the other device */
	INST_LOCK_FACTORY,	/* OP-1F: the target holds the last bootloader not waffle-boot's */
	INST_LOCK_SURVEY	/* a survey build: never offered */
};

/* STATE byte 11: the other boot partition, and the OS made active there. */
enum inst_os {
	INST_OS_NOT_LOOKED,
	INST_OS_CHECKED,	/* found, its CRC passes */
	INST_OS_ENCRYPTED,	/* found, stored encrypted: not checked */
	INST_OS_NONE,
	INST_OS_BAD,
	INST_OS_UNREAD,
	INST_OS_ACTIVE,		/* made active again */
	INST_OS_WRITE_FAILED
};
enum inst_os inst_os_state(const struct tchk_switch *sw);

/* STATE byte 15: the trial as the flash shows it. */
enum inst_trial {
	INST_TRIAL_NONE,
	INST_TRIAL_WAITING,	/* written, no start of it yet */
	INST_TRIAL_REACHED,	/* its start reached the boot menu */
	INST_TRIAL_NOT_REACHED,	/* its front ran; the boot menu was not reached */
	INST_TRIAL_SPENT	/* used up without a start of it */
};

/*
 * The slot state bits of the engine (OPXY_INSTALL_SLOT_*, waffle/testos.h),
 * and what they say of the trial in the slot the selector does not name.
 */
enum inst_trial inst_trial_of(uint8_t other_slot_state);

/* --- The lock: whether the install may be offered at all --- */

/*
 * The device is known by its first stage (S-18.003, how 0) and is the one
 * this image is built for; every test of the table passes (guided ones
 * included, counted as not tested when they did not pass; a test not on
 * this device counts as passed), the carried image
 * verified, and the selected slot is verified and good; on the OP-1F the
 * trial keeps a slot whose image is not a waffle-boot stream; and the
 * build is not the survey. What the other boot partition holds plays no
 * part; an eMMC out of reach locks. Nothing overrides it.
 */
struct inst_lock_in {
	bool image_ok;		/* inst_image_check() passed */
	bool selected_ok;	/* the selected slot verified and good */
	uint8_t how;		/* enum wb_dev_how: only WB_DEV_BY_TABLE installs */
	bool device_mine;	/* the unit is the device this image is built for */
	bool keeps_factory;	/* inst_keeps_factory() */
	bool survey;		/* a survey build */
};
enum inst_lock inst_lock_of(const struct tchk_table *t, const struct tchk_switch *sw,
			    const struct inst_lock_in *in);
/* The lock in words, at most 56 columns. */
const char *inst_lock_words(enum inst_lock l);

/*
 * S-02.108, the rule that keeps the unit's own bootloader on the OP-1F: a
 * trial to slot target is refused when the other slot holds waffle-boot and
 * the target does not; with neither or both it is allowed. streams: bit k
 * set when slot k's image is a waffle-boot stream (OPXY_INSTALL_STREAMS).
 * Always true on a device other than the OP-1F, which keeps its rule.
 */
bool inst_keeps_factory(const struct wb_device *dev, unsigned target, uint8_t streams);

typedef uint32_t (*inst_crc_fn)(uint32_t crc, const uint8_t *p, size_t n);

/* --- The hold that confirms a write (wiki/Screen-Style-Guide.md) --- */

/*
 * A digit allowed by mask (bit d) starts a hold on its key down, with no
 * other key down (device.key.precomp aside), and no knob click. Released early, or any other key down, cancels it. Held
 * S15_HOLD_MS it fires once, and the key's release does nothing more.
 * A key that was down when the hold was offered acts only after an up.
 */
struct inst_hold {
	const struct wb_device *dev;	/* its keys (S-18.030); NULL the OP-XY's */
	uint16_t mask;		/* the digits that may be held */
	uint8_t down[(S15_NKEYS + 7u) / 8u];
	uint8_t stale[(S15_NKEYS + 7u) / 8u];
	unsigned key, digit;	/* the key held and its digit; 0 none */
	uint32_t t0;
	bool holding;
};
void inst_hold_offer(struct inst_hold *h, uint16_t mask);
void inst_hold_key(struct inst_hold *h, unsigned key, unsigned digit, bool down, uint32_t now_ms);
/* The digit whose hold completed at now_ms, else 0. */
unsigned inst_hold_tick(struct inst_hold *h, uint32_t now_ms);
/* The bar, 0 to 1000; 0 while no key is held. */
unsigned inst_hold_fill(const struct inst_hold *h, uint32_t now_ms);

/* --- What a completed hold starts --- */

/*
 * The digits a phase lets a hold start (bit d), and what each starts. In
 * INST_COMMIT_READY 1 keeps the new bootloader and 2 writes the same trial
 * again, so that one trial start can go to recovery (Power + the recovery
 * key) and another to the boot menu before it is kept. 8 returns to the
 * old OS, on the key check too, while an OS waits to be made active
 * (os_ok), since leaving otherwise changes nothing. Every write is the
 * engine's, under its own checks.
 */
#define INST_DIGIT_WRITE   1u	/* the trial, the commit, or the failed write again */
#define INST_DIGIT_RETRIAL 2u	/* INST_COMMIT_READY: the trial again */
#define INST_DIGIT_LEAVE   8u	/* return to the old OS */
enum inst_act {
	INST_ACT_NONE,
	INST_ACT_TRIAL,		/* the trial write */
	INST_ACT_COMMIT,	/* the commit */
	INST_ACT_RETRY,		/* the write that failed, again */
	INST_ACT_LEAVE		/* an OS found made active, nothing else */
};
uint16_t inst_holds_for(enum inst_phase p, bool os_ok);
enum inst_act inst_act_of(enum inst_phase p, unsigned digit);

/* --- The carried image's check, before any write --- */

struct inst_image {
	bool crc_ok;		/* CRC-32 over the bytes == the build's */
	bool stream_ok;		/* block headers sound to FINAL */
	bool desc_ok;		/* one descriptor at FR_DESC_ADDR, it parses */
	bool front_crc_ok;	/* the front's bytes against the descriptor */
	bool pin_ok;		/* the front's own pin == the descriptor's == the build's */
	uint16_t version[4];
	uint8_t pin[32];	/* the descriptor's */
};
struct inst_sha_ops {
	void *ctx;
	void (*init)(void *ctx);
	void (*update)(void *ctx, const uint8_t *p, size_t n);
	void (*final)(void *ctx, uint8_t out[32]);
};
/* True when every part of it holds. */
bool inst_image_check(const uint8_t *img, uint32_t len, uint32_t crc, const uint8_t pin[32],
		      inst_crc_fn crc32, const struct inst_sha_ops *sha, struct inst_image *out);

/* --- The protocol (wiki/Installer-Reference.md, "The installer's USB protocol") --- */

/* Recovery's framing (S-02.093): "WR", type, seq, length, payload, CRC-32. */
#define INST_FRAME_HEAD 6u
#define INST_FRAME_MAX_PAYLOAD 1032u
#define INST_FRAME_MAX (INST_FRAME_HEAD + INST_FRAME_MAX_PAYLOAD + 4u)
#define INST_T_HELLO  0x40u
#define INST_T_STATE  0x41u
#define INST_T_TEST   0x42u
#define INST_T_REPORT 0x43u
#define INST_T_SURVEY 0x44u	/* the survey build's facts; others answer INST_ST_UNKNOWN */
#define INST_PROTO_VERSION 1u
#define INST_HELLO_LEN 64u
/* HELLO's bytes 56 to 59 (zero before): the unit's device and how it was
 * decided (S-18.005's codes), the device this image is built for, and 1
 * for a survey build. */
#define INST_HELLO_O_DEVICE 56u
#define INST_SURVEY_LEN (76u + 2u * TCHK_FAULT_BYTES)
#define INST_STATE_HEAD 32u
#define INST_REPORT_CHUNK 1024u
#define INST_ST_OK 0u
#define INST_ST_NO_TEST 1u
#define INST_ST_UNKNOWN 0xFFu

struct inst_state {
	uint8_t phase, flags, verdict, under_way, lock;
	uint8_t sel_index, sel_verified;
	uint8_t slot_state[2];
	uint8_t target, last_rc, os;
	uint16_t hold_fill;
	uint8_t hold_digit, trial, next_digit;
	uint8_t streams;	/* STATE byte 17: OPXY_INSTALL_STREAMS */
};

struct inst_hello {
	uint8_t phase, flags;
	uint16_t version[4];
	uint32_t len, crc;
	uint8_t pin[32];
	uint8_t device, how, image_device, survey;
	uint32_t fs_n, fs_c;	/* S-18.003's, for check.txt (not on the wire) */
};

/*
 * The survey (make installer-survey): what decides whether an install is
 * safe, read and never written. INST_T_SURVEY's reply, INST_SURVEY_LEN bytes,
 * little-endian: [0] INST_ST_OK, [1] device, [2] how, [3] the boot mode,
 * [4..7] n, [8..11] c (S-18.003), [12] 1 when the selector was read, [13]
 * [14] its two bytes, [15] PA2 (0, 1, 0xFF not read), [16..19] SEC_COUNT,
 * [20..23] the boot partition's KiB, [24] 1 when EXT_CSD was read, [25]
 * EXT_CSD[179] (PART_CONFIG), [26] the OS state (enum inst_os), [27] 0;
 * then per slot 24 bytes: [0] bits INST_SV_*, [1..3] 0, the record's crc32,
 * length and tag, its start log's first INST_SURVEY_LOG bytes; then from
 * [76] emmc-boot's first failing select and read, TCHK_FAULT_BYTES each
 * (tchk_fault_bytes()), all 0 when none failed.
 */
#define INST_SURVEY_LOG 8u
#define INST_SV_RECORD 0x01u	/* the record read */
#define INST_SV_GOOD   0x02u	/* S-02.016's check passes */
#define INST_SV_WAFFLE 0x04u	/* a waffle-boot stream */
#define INST_SV_LOG    0x08u	/* the start log's bytes read */
struct inst_slot_survey {
	uint8_t bits;		/* INST_SV_* */
	uint32_t crc, length, tag;
	uint8_t log[INST_SURVEY_LOG];
};
struct inst_survey {
	uint8_t device, how, boot_mode;
	uint32_t fs_n, fs_c;
	bool sel_read;
	uint8_t sel_index, sel_verified;
	uint8_t pa2;
	bool ext_read;
	uint8_t part_config;
	uint32_t sec_count, boot_kib;
	uint8_t os;		/* enum inst_os */
	struct inst_slot_survey slot[2];
	uint8_t fault[2][TCHK_FAULT_BYTES];	/* tchk_fault_bytes(): select, read */
};
/* The reply's bytes into out (INST_SURVEY_LEN); its length. */
size_t inst_survey_bytes(const struct inst_survey *sv, uint8_t *out);
/* The survey part of check.txt, appended at out + len; the new length. */
size_t inst_survey_report(char *out, size_t len, size_t cap, const struct inst_survey *sv);

/* What a request is answered from. */
struct inst_view {
	const struct inst_hello *hello;
	const struct inst_state *state;
	const struct tchk_table *table;
	const char *report;
	size_t report_len;
	const struct inst_survey *survey;	/* NULL outside a survey build */
};

struct inst_proto {
	uint8_t rx[INST_FRAME_MAX];
	size_t rx_n;
	uint8_t tx[INST_FRAME_MAX];
	size_t tx_n, tx_off;
	inst_crc_fn crc32;
	uint32_t dropped;	/* bytes dropped while looking for a frame */
};
void inst_proto_init(struct inst_proto *p, inst_crc_fn crc32);
/*
 * Takes up to n bytes, stopping once a reply waits (no request is read
 * while one does): returns the bytes taken. A whole request with a good
 * CRC is answered from v into the reply buffer.
 */
size_t inst_proto_feed(struct inst_proto *p, const uint8_t *b, size_t n,
		       const struct inst_view *v);
/* The reply's bytes not yet sent, and their count. */
const uint8_t *inst_proto_out(const struct inst_proto *p, size_t *n);
void inst_proto_sent(struct inst_proto *p, size_t n);
/* A frame of type, seq and payload into out (INST_FRAME_MAX); its length. */
size_t inst_frame(uint8_t *out, uint8_t type, uint8_t seq, const uint8_t *pl, size_t n,
		  inst_crc_fn crc32);

/* --- The screens --- */

/*
 * The screen of a phase, as lines (at most S15_ROWS) in their styles. The
 * view gives what a screen names: the carried version ("1.0.3"), the target
 * slot index, the lock, the last write's code, the OS state, the hold, the
 * device (its key names), the first stage's n and c (an unknown device's
 * lock), and the survey's facts (INST_SURVEYED).
 */
struct inst_screen {
	uint8_t phase;
	const char *version;
	uint8_t target;		/* slot index, 0xFF none */
	uint8_t lock;
	uint8_t last_rc;
	uint8_t os;
	uint8_t trial;
	enum tchk_verdict verdict;
	unsigned hold_digit;	/* 0: no hold under way */
	unsigned hold_fill;	/* 0 to 1000 */
	unsigned hold_left_s;
	uint8_t keys_seen;	/* the key check: bit 0 com, bit 1 the recovery key */
	uint8_t device;		/* enum wb_dev_id */
	uint8_t last_key;	/* the key check's last key down that is neither, 0xFF none */
	uint32_t fs_n, fs_c;	/* S-18.003 */
	const struct inst_survey *survey;
};
unsigned inst_screen_lines(const struct inst_screen *s, struct tchk_line out[S15_ROWS]);
/* A write's code (STATE byte 10) in words, at most 56 columns. */
const char *inst_rc_words(uint8_t rc);

/* The install's part of check.txt, appended at out + len (cap the
 * buffer's size); returns the new length. */
size_t inst_report_append(char *out, size_t len, size_t cap, const struct inst_hello *h,
			  const struct inst_state *st, const struct inst_image *img,
			  const uint8_t *engine_reply);

/* --- The board's runner (src/testos/opxy_testos_installer.c) --- */

struct opxy_master;
struct s03_env;
struct opxy_installer_ctx {
	struct opxy_master *m;
	const struct tchk_table *t;
	struct tchk_switch *sw;		/* the look, then the write */
	const struct s03_env *env;	/* the eMMC through the suite's guard */
	struct s15_console *con;
	void (*pass)(struct opxy_master *m);
	bool (*next_key)(unsigned *key, bool *down);
	void (*show)(struct opxy_master *m);
	void (*report)(struct opxy_master *m);	/* check.txt rendered and written */
	const struct tchk_facts *f;		/* the suite's facts, for the survey */
	bool returned;		/* 8 held in the key check: the OS made active, nothing offered */
};
/* From the suite's start: the carried image checked, the USB port taken. */
void opxy_installer_start(struct opxy_master *m);
/* The suite's progress, for the protocol. */
void opxy_installer_note(enum inst_phase phase, unsigned under_way);
/* The key check's OS state and hold (digit 0 none, fill 0 to 1000), for
 * the protocol. */
void opxy_installer_note_hold(uint8_t os, unsigned digit, unsigned fill);
/* The verdict and the report as they are now, for the protocol. */
void opxy_installer_table(const struct tchk_table *t, const char *report, size_t len);
/* After the suite: the install's screens until the power is removed. */
/* The flash read through the engine, once: true when a trial of this
 * installer was written at an earlier start (the checks passed then). */
bool opxy_installer_trial_back(struct opxy_master *m);
void opxy_installer_run(const struct opxy_installer_ctx *c);
/* check.txt's install part, for the suite's render(). */
size_t opxy_installer_report(char *out, size_t len, size_t cap);
/* The flash's part of the survey, read alone (src/testos/installer_engine.c):
 * the selector, each slot's record, its check, whether it is a waffle-boot
 * stream, and the first bytes of its start log. */
void opxy_installer_survey_read(struct opxy_master *m, struct inst_survey *sv);

#endif /* WAFFLE_INSTALLER_H */

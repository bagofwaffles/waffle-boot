/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The installer (docs/install.md): the test OS's `installer` variant, built
 * with -DOPXY_TESTOS_CHECK=1 -DOPXY_TESTOS_INSTALLER=1. It runs the check
 * suite (waffle/board/testos_check.h), and only when every check passes it
 * offers to write the waffle-boot image it carries: a trial first, then,
 * after a trial start that reached the new boot menu, the commit, and the
 * unit's OS made active again. Every write is started on the unit by holding
 * its key for 5 s; over USB it only reports.
 *
 * The parts here are pure and are tested on the host: the hold, the lock,
 * the check of the carried image, the screens, and the USB protocol. The
 * board's runner is src/testos/opxy_testos_installer.c; the write engine is
 * src/testos/installer_engine.c.
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
	INST_GUIDED,		/* a guided test on the screen */
	INST_LOCKED,		/* a check did not pass: no write offered */
	INST_READY,		/* the trial offered (hold 1) */
	INST_WRITING_TRIAL,
	INST_TRIAL_WRITTEN,	/* switch off and on for the trial start */
	INST_COMMIT_READY,	/* the trial reached the boot menu (hold 1) */
	INST_COMMITTING,
	INST_DONE,
	INST_FAILED,		/* a write failed */
	INST_TRIAL_FAILED,	/* the trial start did not reach the boot menu */
	INST_LEFT		/* left without installing, the OS active again */
};

#define INST_F_CHECKS_DONE 0x01u
#define INST_F_UNLOCKED    0x02u
#define INST_F_IMAGE_OK    0x04u
#define INST_F_CRUMBS      0x08u

enum inst_lock {
	INST_LOCK_NONE,
	INST_LOCK_FAILED,	/* a check failed */
	INST_LOCK_NOT_TESTED,	/* an automatic check could not run */
	INST_LOCK_IMAGE,	/* the carried image did not verify */
	INST_LOCK_SELECTED,	/* the selected slot is not verified or not good */
	INST_LOCK_NO_OS,	/* the unit's OS was not found, or fails its check */
	INST_LOCK_GUIDED,	/* a guided test skipped or not passed */
	INST_LOCK_EMMC		/* the eMMC could not be reached */
};

/* STATE byte 11: the unit's OS partition. */
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
 * Every test of the table passes (guided ones included), the carried image
 * verified, the selected slot is verified and good, and the unit's OS is
 * there to make active again (a look of TCHK_SW_READY). Nothing overrides it.
 */
enum inst_lock inst_lock_of(const struct tchk_table *t, const struct tchk_switch *sw,
			    bool image_ok, bool selected_ok);
/* The lock in words, at most 56 columns. */
const char *inst_lock_words(enum inst_lock l);

/* --- The hold that confirms a write (docs/ui-style-guide.md) --- */

/*
 * A digit allowed by mask (bit d) starts a hold on its key down, with no
 * other key down. Released early, or any other key down, cancels it. Held
 * S15_CONFIRM_HOLD_MS it fires once, and the key's release does nothing more.
 * A key that was down when the hold was offered acts only after an up.
 */
struct inst_hold {
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
typedef uint32_t (*inst_crc_fn)(uint32_t crc, const uint8_t *p, size_t n);
/* True when every part of it holds. */
bool inst_image_check(const uint8_t *img, uint32_t len, uint32_t crc, const uint8_t pin[32],
		      inst_crc_fn crc32, const struct inst_sha_ops *sha, struct inst_image *out);

/* --- The protocol (docs/install.md, "The installer's USB protocol") --- */

/* Recovery's framing (S-02.093): "WR", type, seq, length, payload, CRC-32. */
#define INST_FRAME_HEAD 6u
#define INST_FRAME_MAX_PAYLOAD 1032u
#define INST_FRAME_MAX (INST_FRAME_HEAD + INST_FRAME_MAX_PAYLOAD + 4u)
#define INST_T_HELLO  0x40u
#define INST_T_STATE  0x41u
#define INST_T_TEST   0x42u
#define INST_T_REPORT 0x43u
#define INST_PROTO_VERSION 1u
#define INST_HELLO_LEN 64u
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
};

struct inst_hello {
	uint8_t phase, flags;
	uint16_t version[4];
	uint32_t len, crc;
	uint8_t pin[32];
};

/* What a request is answered from. */
struct inst_view {
	const struct inst_hello *hello;
	const struct inst_state *state;
	const struct tchk_table *table;
	const char *report;
	size_t report_len;
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
 * slot index, the lock, the last write's code, the OS state, the hold.
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
	uint8_t keys_seen;	/* the key check: bit 0 com, bit 1 M4 */
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
};
/* From the suite's start: the carried image checked, the USB port taken. */
void opxy_installer_start(struct opxy_master *m);
/* The suite's progress, for the protocol. */
void opxy_installer_note(enum inst_phase phase, unsigned under_way);
/* The verdict and the report as they are now, for the protocol. */
void opxy_installer_table(const struct tchk_table *t, const char *report, size_t len);
/* After the suite: the install's screens until the power is removed. */
/* The flash read through the engine, once: true when a trial of this
 * installer was written at an earlier start (the checks passed then). */
bool opxy_installer_trial_back(struct opxy_master *m);
void opxy_installer_run(const struct opxy_installer_ctx *c);
/* check.txt's install part, for the suite's render(). */
size_t opxy_installer_report(char *out, size_t len, size_t cap);

#endif /* WAFFLE_INSTALLER_H */

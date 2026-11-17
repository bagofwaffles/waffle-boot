/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Recovery's protocol over the serial port (spec S-02.093): frames of
 * "WR", a type, a sequence number, a little-endian u16 length, the payload
 * and a CRC-32 over type to payload's end. The host asks, the device
 * answers each with type | 0x80 and the same sequence number.
 *
 *   0x01 HELLO   {}                      -> proto, front version, the image's
 *                                           version and front pin
 *   0x02 INFO    {}                      -> everything the screen shows
 *   0x03 BEGIN   {len u32, sha256[32]}, or with a flags byte after them
 *                                        -> status, the way, the target slot
 *   0x04 DATA    {offset u32, 1..1024 bytes} -> status, next offset u32
 *   0x05 END     {}                      -> status; the write runs, INFO follows it
 *   0x06 RESTART {}, or {flags}          -> status, then the PSoC's reset;
 *                                           refused while a restart would
 *                                           start a slot this recovery
 *                                           damaged, unless forced
 *   0x07 ACT     {enum fr_act, flags}    -> status, the way, the slot, the
 *                                           tier; the action runs, INFO
 *                                           follows it
 *
 * An accepted END or ACT is followed by a countdown (FR_PH_COUNTDOWN) before
 * the first erase or program, which no request shortens; the host leaving
 * stops it with nothing written. Flags bit 7 (FR_BEGIN_F_FORCE) lifts the
 * refusals that protect the user.
 */
#ifndef FR_PROTO_H
#define FR_PROTO_H

#include "fr.h"
#include "fr_slots.h"

#define FR_PROTO_VERSION  2u
#define FR_FRONT_VERSION  16u	/* the front's own version, bumped with its pin */
#define FR_PROTO_MAX_DATA 1024u	/* image bytes in one DATA, after its offset */
#define FR_PROTO_MAX      (FR_PROTO_MAX_DATA + 8u)	/* a frame's payload */
#define FR_PROTO_HEAD     6u	/* "WR", type, seq, len */
#define FR_BATT_MAX       16u	/* bytes kept of a battery report */

#define FR_T_HELLO   0x01u
#define FR_T_INFO    0x02u
#define FR_T_BEGIN   0x03u
#define FR_T_DATA    0x04u
#define FR_T_END     0x05u
#define FR_T_RESTART 0x06u
#define FR_T_ACT     0x07u
#define FR_T_REPLY   0x80u

/*
 * Reply payloads, little-endian. Every reply but HELLO and INFO starts with
 * a status byte.
 *
 * HELLO (FR_HELLO_LEN bytes): */
#define FR_HELLO_O_PROTO     0u		/* u8 FR_PROTO_VERSION */
#define FR_HELLO_O_FRONT     1u		/* u16 FR_FRONT_VERSION */
#define FR_HELLO_O_MAX_DATA  3u		/* u16 FR_PROTO_MAX_DATA */
#define FR_HELLO_O_WHY       5u		/* u8 FR_WHY_* */
#define FR_HELLO_O_FLAGS     6u		/* u8 FR_INFO_F_* */
#define FR_HELLO_O_VERSION   7u		/* u16[4] the running image's version */
#define FR_HELLO_O_PIN       15u	/* u8[32] its front pin; zeros without own_desc */
#define FR_HELLO_LEN         47u

/* INFO (FR_INFO_LEN bytes): */
#define FR_INFO_O_WHY        0u		/* u8 FR_WHY_* */
#define FR_INFO_O_FLAGS      1u		/* u8 FR_INFO_F_* */
#define FR_INFO_O_RUNNING    2u		/* u8 the running slot (valid with ESTABLISHED) */
#define FR_INFO_O_SEL_INDEX  3u		/* u8 the selector's index (valid with SELECTOR) */
#define FR_INFO_O_SEL_VERIFIED 4u	/* u8 its verified byte */
#define FR_INFO_O_TARGET     5u		/* u8 the slot a write goes to, or FR_NO_SLOT */
#define FR_INFO_O_REFUSAL    6u		/* u8 enum fr_write_rc when FR_NO_SLOT */
#define FR_INFO_O_PHASE      7u		/* u8 enum fr_phase */
#define FR_INFO_O_LEN        8u		/* u32 BEGIN's length */
#define FR_INFO_O_GOT        12u	/* u32 bytes received */
#define FR_INFO_O_PROGRESS   16u	/* u16 the write's progress, 0..1000 */
#define FR_INFO_O_RESULT     18u	/* u8 the last write's enum fr_write_rc */
#define FR_INFO_O_IMAGE_RC   19u	/* u8 and its enum fr_image_rc */
#define FR_INFO_O_WRITTEN    20u	/* u8 the slot written, or FR_NO_SLOT */
#define FR_INFO_O_VERSION    21u	/* u16[4] the running image's version */
#define FR_INFO_O_PIN        29u	/* u8[32] its front pin; zeros without own_desc */
#define FR_INFO_O_READBACK   61u	/* u8[32] the read-back's SHA-256 (phase DONE), else zeros */
#define FR_INFO_O_SLOT(k)    (93u + FR_INFO_SLOT_LEN * (k))
#define FR_INFO_O_RUN_WAY    199u	/* u8 enum fr_way of a write with FR_BEGIN_F_RUNNING */
#define FR_INFO_O_RUN_REFUSAL 200u	/* u8 enum fr_write_rc when that is FR_WAY_NONE */
#define FR_INFO_O_WAY        201u	/* u8 enum fr_way of the write under way or ended */
#define FR_INFO_O_BATT_LEN   202u	/* u8 bytes of the battery report, 0: none came */
#define FR_INFO_O_BATT       203u	/* u8[16] the PSoC's last 0x37 payload (S-07.053) */
#define FR_INFO_O_COUNT      219u	/* u8 seconds left of the countdown, else 0 */
#define FR_INFO_O_ACT        220u	/* u8 enum fr_act of the action under way or ended,
					 * 0 for a write */
#define FR_INFO_O_TIER       221u	/* u8 enum fr_tier of a try of, or a switch to, the
					 * slot that is not running */
#define FR_INFO_O_ACTS       222u	/* u8[4] the status an ACT would get now, for
					 * each action in enum fr_act's order */
#define FR_INFO_O_FORCED     226u	/* u8[4] and with FR_BEGIN_F_FORCE */
#define FR_INFO_O_KEEP       230u	/* u8 the slot a keep would select, or FR_NO_SLOT */
#define FR_INFO_O_REFUSED    231u	/* u8 the status of the last request refused, or
					 * countdown stopped, since the write this recovery
					 * made last; 0 when none, or once a write or an
					 * action is accepted */
#define FR_INFO_LEN          232u
/* each slot, from fr_slot_info(), its offsets relative to FR_INFO_O_SLOT(k) */
#define FR_INFO_S_FLAGS      0u		/* u8 FR_SLOT_F_* */
#define FR_INFO_S_LENGTH     1u		/* u32 the record's length */
#define FR_INFO_S_CRC        5u		/* u32 the record's crc32 */
#define FR_INFO_S_VERSION    9u		/* u16[4] its image's version (with HAS_DESC) */
#define FR_INFO_S_FRONT_CRC  17u	/* u32 its descriptor's front CRC (with HAS_DESC) */
#define FR_INFO_S_PIN        21u	/* u8[32] its front pin (with HAS_DESC) */
#define FR_INFO_SLOT_LEN     53u

#define FR_INFO_F_ESTABLISHED 0x01u	/* the running slot is established (S-01.002) */
#define FR_INFO_F_SELECTOR    0x02u	/* the selector was read */
#define FR_INFO_F_OWN_DESC    0x04u	/* the running image's descriptor parsed */
#define FR_INFO_F_MOVED       0x08u	/* this recovery wrote the selector, which names the
					 * other slot, before a write of the running slot */
#define FR_INFO_F_SPOILED     0x10u	/* this recovery began a write of the running slot,
					 * and that slot is bad */
#define FR_INFO_F_HELD        0x20u	/* and no start would reach a good slot: the
					 * power-off is held and RESTART refused */
#define FR_INFO_F_READ_ERROR  0x40u	/* a read of the selector or a slot failed: what
					 * INFO reports of them may not be the flash's */
#define FR_INFO_F_UNSETTLED   0x80u	/* a write of this recovery read the running slot
					 * back good and did not settle the selector on it,
					 * and the other slot is bad */

#define FR_SLOT_F_GOOD       0x01u	/* S-02.016 */
#define FR_SLOT_F_HAS_DESC   0x02u
#define FR_SLOT_F_HAS_FRONT  0x04u
#define FR_SLOT_F_TRIED      0x08u	/* its record carries the trial mark (fr_tried()) */
#define FR_SLOT_F_STARTED    0x10u	/* tried, and its front marked a trial start */

/* RESTART: {status}. BEGIN: {status, enum fr_way, the target slot or
 * FR_NO_SLOT}: what END will do, or refuse with FR_WR_CHANGED.
 * DATA: {status, next offset u32}. END: {status, enum fr_image_rc}. */
#define FR_BEGIN_REPLY_LEN   3u
#define FR_ACT_REPLY_LEN     4u		/* {status, enum fr_way, the slot, enum fr_tier} */
#define FR_DATA_REPLY_LEN    5u
#define FR_END_REPLY_LEN     2u

/* Status bytes of the replies: 0 ok, else an enum fr_write_rc, or these. */
#define FR_ST_BAD_REQUEST 0x40u	/* unknown type or a malformed payload */
#define FR_ST_STATE       0x41u	/* not now (no BEGIN, or a write under way) */
#define FR_ST_OFFSET      0x42u	/* DATA not at the next offset */
#define FR_ST_LENGTH      0x43u	/* END before every byte came, DATA past the end */
#define FR_ST_DAMAGED     0x44u	/* RESTART: the selected slot was damaged by a write
				 * of this recovery, and no other slot is selected */

/* Why recovery runs. */
#define FR_WHY_KEYS  1u		/* Power + M4 (S-15.150) */
#define FR_WHY_CHECK 2u		/* the image check failed */
#define FR_WHY_COUNT 3u		/* three starts in a row did not finish (S-02.095) */

/* Recovery's phases, as INFO reports them. */
enum fr_phase {
	FR_PH_IDLE, FR_PH_RECEIVING, FR_PH_WRITING, FR_PH_DONE, FR_PH_FAILED,
	FR_PH_COUNTDOWN		/* accepted; the first erase follows the countdown */
};

/* S-02.084: the time between a host's write or action being accepted and
 * its first erase or program, in which a power-off stops it. */
#define FR_COUNTDOWN_US 15000000u

/* What recovery found at its start, filled by its caller. */
struct fr_rec_status {
	uint8_t why;			/* FR_WHY_* */
	struct fr_running running;
	bool selector_ok;
	struct fr_selector sel;
	struct fr_slot_info slot[2];
	bool read_error;		/* a read of the selector or a slot failed */
	bool own_desc;			/* the running image's descriptor parsed */
	struct fr_desc desc;		/* the running image's */
	uint8_t target;			/* the slot a write goes to, or FR_NO_SLOT */
	uint8_t refusal;		/* enum fr_write_rc when target is FR_NO_SLOT */
	uint8_t run_way;		/* enum fr_way with FR_BEGIN_F_RUNNING */
	uint8_t run_refusal;		/* enum fr_write_rc when that is FR_WAY_NONE */
	bool trial;			/* FR_BEGIN_F_TRIAL may write target for a trial */
	uint8_t select_target;		/* the slot FR_BEGIN_F_SELECT selects, or FR_NO_SLOT */
	uint8_t select_refusal;		/* enum fr_write_rc when FR_NO_SLOT */
	struct fr_home home;		/* what this recovery did to the running slot */
	uint8_t batt[FR_BATT_MAX];	/* the PSoC's last 0x37 payload */
	uint8_t batt_len;
};

struct fr_proto {
	const struct fr_bytes *io;
	struct fr_rec_status *st;
	const struct fr_flash *flash;
	uint8_t *staging;		/* FR_SLOT_CAP bytes */
	/* the frame being received */
	uint8_t rx[FR_PROTO_HEAD + FR_PROTO_MAX + 4u];
	size_t rx_n;
	/* a reply waiting for room on the port */
	uint8_t tx[FR_PROTO_HEAD + 256u + 4u];
	size_t tx_n, tx_at;
	/* the transfer */
	uint8_t phase;			/* enum fr_phase */
	uint32_t len, got;
	uint8_t flags;			/* BEGIN's flags */
	uint8_t plan_way, plan_target;	/* what BEGIN announced */
	uint8_t sha[FR_SHA256_LEN];
	struct fr_writer w;
	uint8_t result;			/* the last write's enum fr_write_rc */
	uint8_t image_rc;		/* and its enum fr_image_rc */
	/* the write this recovery made last, kept through a transfer that
	 * leads to no write (S-02.093) */
	bool made;
	uint8_t made_phase, made_result, made_flags, made_target, made_way, made_act;
	uint8_t made_sha[FR_SHA256_LEN];
	bool restart_due;		/* RESTART answered, its reply not yet out */
	bool stepping;			/* the write under way has made its first step */
	bool restart;			/* RESTART's reply is handed to the port:
					 * the caller waits for io->idle, then resets */
	uint64_t now;			/* fr_proto_clock()'s time */
	uint64_t count_end;		/* the countdown's end */
	uint8_t count_left;		/* its seconds left, as last shown */
	uint8_t refused;		/* enum fr_write_rc of a request refused since
					 * the write this recovery made last, else 0 */
	bool local;			/* the action under way or ended was asked for
					 * on the unit (fr_proto_local()) */
	uint8_t tier;			/* enum fr_tier of that action */
	uint16_t version[4];		/* the version of the image being written, or
					 * of the slot acted on */
	bool has_version;
	uint32_t changes;		/* bumped on every change the screen shows */
	/* Asks the PSoC for the battery once and fills st->batt, before a
	 * BEGIN for an in-place write is accepted; NULL: not asked. */
	void (*battery)(void *ctx);
	void *battery_ctx;
	/* Called between the passes of a survey made while answering a
	 * request (each pass reads up to a slot's capacity twice), so that the
	 * caller's links are served meanwhile; NULL: not called. */
	void (*idle)(void *ctx);
	void *idle_ctx;
};

void fr_proto_init(struct fr_proto *p, const struct fr_bytes *io,
		   struct fr_rec_status *st, const struct fr_flash *flash,
		   uint8_t *staging);
/* Reads what came and answers it; a write that was under way before the
 * call makes one step, its first only once END's reply is with the port.
 * During a write only HELLO and INFO are served: every other request is
 * answered busy. */
void fr_proto_poll(struct fr_proto *p);
/* The port's configured state changed: the host left, or a new one came.
 * A request half received is dropped, so that it cannot take in the next
 * host's first one, and a transfer or a countdown under way is stopped
 * (FR_WR_STOPPED): nothing is written. */
void fr_proto_drop(struct fr_proto *p);
/* The time, called after fr_proto_poll() on each pass: a countdown that has
 * run out makes the phase writing, and the write's first step comes in the
 * next fr_proto_poll(), so that the caller sees the phase first. */
void fr_proto_clock(struct fr_proto *p, uint64_t now_us);
/* An action asked for on the unit and confirmed there: decided with force,
 * and with no countdown. FR_WR_OK starts it (the phase is writing);
 * FR_WR_BUSY while a transfer, a countdown or a write is under way. */
uint8_t fr_proto_local(struct fr_proto *p, uint8_t act);
/* What an action would do now, from the state in st. */
void fr_rec_act(const struct fr_rec_status *st, uint8_t act, bool force, struct fr_act_plan *plan);
/* An in-place write is under way: the caller leaves the screen and the
 * current limit alone until it has ended (S-02.084). */
bool fr_proto_in_place(const struct fr_proto *p);
/* A power-off now would leave a unit that does not start: an in-place
 * write is under way, or a write of the running slot left it bad and the
 * first stage would reach no good slot (fr_rec_reach()). The caller holds
 * the power-off answer, and RESTART is refused, for that time. The state
 * is read from the flash again at each INFO, BEGIN and RESTART. */
bool fr_proto_hold(const struct fr_proto *p);

/* The target and refusal, the way and refusal of a write of the running
 * slot, whether a trial may go, and the select's target or refusal, from
 * the state in st (fr_writer_plan()'s rules, read from st and not from the
 * flash). */
void fr_rec_target(struct fr_rec_status *st);
/* The selector and both slots read from the flash into st (two CRC passes
 * and two stream scans), then fr_rec_target(). */
void fr_rec_survey(struct fr_rec_status *st, const struct fr_flash *f);
/* fr_reach() over what st holds; none after a read that failed. */
unsigned fr_rec_reach(const struct fr_rec_status *st);
/* fr_writer_plan()'s damaged state, over what st holds: this recovery
 * began a write of the running slot, and the slot is bad, or good with the
 * selector not settled after it while the other slot is bad. */
bool fr_rec_rescue(const struct fr_rec_status *st);
/* That state with no good slot reached: the power-off is held and RESTART
 * refused (fr_proto_hold() adds the in-place write itself). */
bool fr_rec_held(const struct fr_rec_status *st);
/* After a read that failed, and not during a write: the selector and both
 * slots read again. The caller calls it now and then while it holds the
 * power-off, so that a read error that has passed does not keep the hold
 * with no host there to ask. */
void fr_proto_tick(struct fr_proto *p);

#endif /* FR_PROTO_H */

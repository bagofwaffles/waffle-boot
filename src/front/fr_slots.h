/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SPI flash's slots as the front reads and writes them, its own copy of
 * S-02: the map (S-02.002), the selector and records (S-02.010-S-02.016),
 * the running slot (S-01.002), the bootable-stream check (S-02.091) with the
 * front's own condition, a waffle-boot front in the image, and the
 * recovery writer (S-02.040-S-02.052, S-02.084, S-02.090), stepped so that
 * the caller keeps USB and the PSoC served between steps.
 */
#ifndef FR_SLOTS_H
#define FR_SLOTS_H

#include "fr.h"
#include "fr_image.h"

#define FR_SECTOR       0x1000u
#define FR_PAGE         0x100u
#define FR_SELECTOR_OFF 0xD000u
#define FR_RECORD_OFF(k) (0xE000u + 0x1000u * (k))
#define FR_SLOT_BASE(k) (0x10000u + 0x78000u * (k))
#define FR_SLOT_CAP     0x78000u
/* S-02.084: recovery's images, which leave the slot's settings sector
 * (S-02.103) and its start log free. */
#define FR_IMAGE_MAX    0x76000u
#define FR_SETTINGS_OFF(k) (FR_SLOT_BASE(k) + FR_IMAGE_MAX)
#define FR_TAG          0x0000B004u
#define FR_NO_SLOT      0xFFu

/* Results of the flash calls. */
#define FR_FLASH_OK  0
#define FR_FLASH_IO  (-1)
/* A read whose busy wait ran its whole bound (S-13b.017) and gave up; any
 * caller may take it as FR_FLASH_IO. */
#define FR_FLASH_BUSY (-2)

struct fr_selector {
	uint8_t index, verified;
};

struct fr_record {
	uint32_t crc32, length, tag;
};

int fr_read_selector(const struct fr_flash *f, struct fr_selector *s);
/* S-02.011: 0x00 selects slot 0, anything else slot 1. */
unsigned fr_selected_slot(uint8_t index);
int fr_read_record(const struct fr_flash *f, unsigned k, struct fr_record *r);
/* S-02.016: tag, 1 <= length <= capacity, CRC over the slot's bytes. */
int fr_check_slot(const struct fr_flash *f, unsigned k, bool *good,
		  struct fr_record *r);

/*
 * S-01.002: the one slot whose stream the loaded memory matches in every
 * byte compared (where both do, the one whose stream reaches its FINAL
 * block), block by block (its non-FILL, non-IGNORE payloads where
 * mem maps them), at least one byte being compared, as s02_running_slot()
 * decides it. mem must map only memory the stream loaded, readable as
 * data, and not written since the load. Where both slots match whole and
 * both reach their FINAL block, they hold the same image: the slot is the
 * one later starts settle on, the slot the selector names when verified
 * is 0x01 or that slot is good (S-02.016), otherwise the other slot.
 * established is false, and *slot FR_NO_SLOT, when neither slot matches,
 * when both match and not both reach their FINAL block, or on a read
 * failure (then FR_FLASH_IO).
 */
int fr_running_slot(const struct fr_flash *f, const struct fr_mem *mem,
		    unsigned *slot, bool *established);
/* The same; *both says that both slots matched whole to their FINAL
 * block, so that the slot is the settled one and not told from memory. */
int fr_running_slots(const struct fr_flash *f, const struct fr_mem *mem,
		     unsigned *slot, bool *established, bool *both);
/* The same answer, only after fr_image_check() has passed: one slot whose
 * descriptor is the loaded one, the other's differing or a whole stream
 * that loads none of it, is that slot with no stream compared; anything else
 * is the whole compare above. */
int fr_running_slots_checked(const struct fr_flash *f, const struct fr_mem *mem,
			     unsigned *slot, bool *established, bool *both);
/* The flash holds the same image in both slots: each record has the tag
 * and a length inside the slot, the lengths are equal, and the slots'
 * bytes over that length are equal. It stops at the first that differs. */
int fr_slots_same(const struct fr_flash *f, bool *same);

/* What the screen and the host tool show of a slot. */
struct fr_slot_info {
	bool good;		/* S-02.016 */
	struct fr_record rec;
	bool has_desc;		/* a valid descriptor block in its stream, read
				 * within the record's length, or within the slot
				 * when the record is not plausible, but for the tag
				 * with a length of 2^31 or more: then none */
	bool bootable;		/* its stream passes S-02.091's five rules */
	bool has_front;		/* and its front's bytes match it */
	bool tried;		/* its record carries the trial mark (fr_tried()) */
	bool started;		/* tried, and entry 0 of its log reads finished:
				 * its own front marked a trial start */
	bool spent;		/* tried, and its log's byte 0 reads entry 0
				 * pending-coded but not 0xFD: a start of the other
				 * slot spent the trial (fr_log_trial_spend()) */
	bool has_log;		/* its record gives a log (S-02.095: the tag, a
				 * length of 1 to FR_LOG_IMAGE_MAX), and log0 is
				 * byte 0 of its last sector */
	uint8_t log0;
	struct fr_desc desc;
	uint8_t kind;		/* enum fr_kind_of, by fr_slot_kind() */
	uint16_t te_version[4];	/* with FR_KIND_TE_BUILD: its table row's */
};

/* S-02.106: which bootloader a good or tried slot holds. */
enum fr_kind_of {
	FR_KIND_NONE,		/* neither good nor tried: no kind */
	FR_KIND_WB,		/* a valid descriptor: desc.bl_version */
	FR_KIND_TE_BUILD,	/* a row of the TE table: te_version */
	FR_KIND_TE,		/* only the prefix CRC matches */
	FR_KIND_UNKNOWN		/* "??" */
};
int fr_slot_info(const struct fr_flash *f, unsigned k, struct fr_slot_info *out);
/* The kind of slot k from what fr_slot_info() read of it into s: rules 1
 * and 2 read nothing more; rule 3 reads the slot's first 4,468 bytes, and
 * only when they fail. FR_FLASH_IO when that read fails (kind unknown). */
int fr_slot_kind(const struct fr_flash *f, unsigned k, struct fr_slot_info *s);

/* The trial mark (S-02.016): the tag, a length inside the slot, and crc32
 * the complement of the CRC-32 crc of the slot's length bytes. A slot with
 * it is bad by S-02.016, and the first stage's check of it is sure to
 * fail (S-02.021). */
bool fr_tried(const struct fr_record *r, uint32_t crc);

/* Byte 0 of a slot's log sector (S-02.095): entry 0 in bits 0 and 1. A
 * trial write leaves it pending (0xFD: started set, finished clear); a
 * trial start of the slot's own front marks it finished (0xFC). */
#define FR_LOG0_PENDING 0xFDu
#define FR_LOG0_MARKED  0xFCu
#define FR_LOG0_E(b)    ((unsigned)(b) & 3u)	/* entry 0 of byte 0 */
#define FR_LOG_E0_PENDING  1u
#define FR_LOG_E0_FINISHED 0u

/*
 * An image as a slot image, before anything is written: its size, at most
 * FR_LOG_IMAGE_MAX bytes so that the slot's last sector stays the start
 * log's (S-02.084, S-02.095), S-02.091's five rules, and a waffle-boot front in it (a descriptor block
 * at FR_DESC_ADDR that parses, the FIRST and FINAL targets inside the
 * front's L2 window, the INIT target in its L1 code window, and the CRC of
 * every block payload inside the front's windows equal to the descriptor's
 * front_crc). desc gets the descriptor.
 */
enum fr_image_rc {
	FR_IMG_OK = 0,
	FR_IMG_SIZE,		/* 0 bytes, or more than FR_IMAGE_MAX (S-02.084) */
	FR_IMG_STREAM,		/* S-02.091 rule 1: headers, FINAL, lengths */
	FR_IMG_INIT,		/* rule 2 */
	FR_IMG_SPACES,		/* rule 3 */
	FR_IMG_STACK,		/* rule 4 */
	FR_IMG_BCODE,		/* rule 5 */
	FR_IMG_NO_FRONT		/* no waffle-boot front */
};
enum fr_image_rc fr_image_valid(const uint8_t *img, uint32_t n, struct fr_desc *desc);

/*
 * Which good slot the first stage reaches from this selector, by
 * its own rules (S-02.020-S-02.026), however its own selector write is cut
 * short. good[k] is slot k by S-02.016, and rec the record of the slot the
 * selector names.
 *
 * FR_REACH_NAMED: the slot the selector names. With verified 0x01 it is
 * booted with no check and no write. Else it is checked, passes, and the
 * first stage erases the selector and programs {index, 0x01}: every torn
 * result of that still names slot 1 when the index did, but from index
 * 0x00 it can read {0x01, 0x01}, so slot 0 counts only when slot 1 is good
 * as well.
 * FR_REACH_FALLBACK: the selector names slot 0, which is bad by a record
 * whose check is sure to fail (another tag, or the tag with a length from
 * 1 to the capacity), verified has a bit set besides bit 0, and slot 1 is
 * good: the first stage writes {0x01, 0x01} and boots slot 1 from the next
 * reset. No torn result of that erase reads {0x00, 0x01}, since verified
 * keeps its bits. A fallback to slot 0 never counts: its program
 * {0x00, 0x01}, cut short, can read {0x01, 0x01}.
 */
#define FR_REACH_NONE     0u
#define FR_REACH_NAMED    1u
#define FR_REACH_FALLBACK 2u
unsigned fr_reach(const struct fr_selector *sel, const bool good[2],
		  const struct fr_record *rec);
/* S-02.021: the record of a bad slot whose check the first stage
 * is sure to fail: another tag, or the tag with a length inside the slot.
 * It passes an empty record, and a longer length is left to chance
 * (S-02.026). */
bool fr_record_fails(const struct fr_record *r);

/* Why a write is refused, or how it ended (also the protocol's codes). */
enum fr_write_rc {
	FR_WR_OK = 0,
	FR_WR_BUSY,		/* a write is under way */
	FR_WR_NOT_RUNNING,	/* the running slot is not established (S-01.002) */
	FR_WR_NOT_SELECTED,	/* the running slot is not the selected one */
	FR_WR_UNVERIFIED,	/* the selector's verified byte is not 0x01 (S-02.090) */
	FR_WR_SELECTED_BAD,	/* the selected slot is bad, and is not the running
				 * slot with its front intact (S-02.084) */
	FR_WR_SIZE,		/* S-02.044 */
	FR_WR_IMAGE,		/* S-02.091 or no front: see image_rc */
	FR_WR_HASH,		/* the image is not the one announced */
	FR_WR_FLASH,		/* an erase, program or read failed */
	FR_WR_READBACK,		/* the read-back did not confirm the slot (S-02.047),
				 * or its SHA-256 is not the one announced */
	FR_WR_SELECTOR,		/* the selector write failed after a good read-back,
				 * or did not read back {target, 0x00} */
	FR_WR_FRONT,		/* the image's front is not the running one (S-02.084) */
	FR_WR_UNUSED_13,	/* never given: two slots may hold the same image */
	FR_WR_SELECT_FIRST,	/* the selector write before a write of the running
				 * slot failed; no slot or record was written */
	FR_WR_TRY_OTHER,	/* an in-place write, before a write of the other slot
				 * has failed in this recovery (S-02.084) */
	FR_WR_CHANGED,		/* END: the write would not go as BEGIN announced */
	FR_WR_READ,		/* the start condition could not be read from the
				 * flash; nothing was written */
	FR_WR_ONLY_GOOD,	/* the running slot is damaged and the other slot is
				 * the only good one: not replaced by another image */
	FR_WR_TRIAL_STATE,	/* a trial with the running slot bad, or a trial or
				 * select after this recovery began a write of the
				 * running slot or wrote the selector to the other */
	FR_WR_NOT_TRIED,	/* select: no slot holds the image as tried where
				 * the rule names */
	FR_WR_NOT_STARTED,	/* select or keep from the old front: the tried
				 * slot's front has left no mark of a trial start */
	FR_WR_ALREADY,		/* try or switch: the selector names the slot with
				 * verified 0x01 already; nothing was written */
	FR_WR_TARGET_BAD,	/* try or switch: the slot is bad, and not tried */
	FR_WR_STOPPED		/* the countdown before a write was stopped: the
				 * host left; nothing was written */
};

/* BEGIN's flags (S-02.093). */
#define FR_BEGIN_F_RUNNING 0x01u	/* write the running slot, not the other one */
#define FR_BEGIN_F_TRIAL   0x02u	/* another front is written for a trial */
#define FR_BEGIN_F_SELECT  0x04u	/* select the slot that holds the image as tried */
#define FR_BEGIN_F_ALL     0x07u	/* at most one of them is set */
#define FR_BEGIN_F_FORCE   0x80u	/* with any of them or none: the refusals that
					 * protect the user are lifted (S-02.093) */

/* How a write goes (S-02.084). */
enum fr_way {
	FR_WAY_NONE,		/* refused */
	FR_WAY_PLAIN,		/* the slot the selector does not name */
	FR_WAY_SELECT_FIRST,	/* the running slot, after the selector names the
				 * other slot, which is good */
	FR_WAY_IN_PLACE,	/* the running slot while the selector names it: the
				 * other slot is bad and a write of it has failed. A
				 * cut leaves no slot that boots */
	FR_WAY_TRIAL,		/* the other slot under the trial mark, the selector
				 * written to start it once */
	FR_WAY_SELECT,		/* a tried slot's record made right, then selected */
	/* the actions on a slot's image as it lies (enum fr_act) */
	FR_WAY_TRY,		/* the slot marked tried, the selector written to
				 * start it once */
	FR_WAY_SWITCH,		/* the selector written to the slot for good */
	FR_WAY_KEEP,		/* a tried slot's record made right, then selected,
				 * with no image from the host */
	FR_WAY_PUT_BACK		/* a tried slot's record made right, no selector */
};

/* An action on the image a slot holds (S-02.084): on the slot that is not
 * running, but a keep in a trial start, which keeps the running slot. */
enum fr_act { FR_ACT_NONE, FR_ACT_TRY, FR_ACT_SWITCH, FR_ACT_KEEP, FR_ACT_PUT_BACK };

/* How far a try or a switch is from safe, for the warning shown. */
enum fr_tier {
	FR_TIER_NONE,
	FR_TIER_SAME,		/* good, with the running slot's front (its pin and
				 * front CRC, its front's bytes checked) */
	FR_TIER_FRONT,		/* good, another waffle-boot front: recovery may
				 * not be reachable from it */
	FR_TIER_FOREIGN,	/* good, no waffle-boot front: no recovery there */
	FR_TIER_BAD		/* a bad record: nothing ever checks the slot */
};

enum fr_writer_state {
	FR_WS_IDLE, FR_WS_SELECT_ERASE, FR_WS_SELECT_PROGRAM, FR_WS_SLOT, FR_WS_LOG_ERASE,
	FR_WS_SETTINGS_ERASE, FR_WS_LOG_PENDING, FR_WS_SELECT_LOG, FR_WS_RECORD_ERASE, FR_WS_RECORD_PROGRAM,
	FR_WS_READBACK, FR_WS_SELECTOR_ERASE, FR_WS_SELECTOR_PROGRAM, FR_WS_TRIAL_VERIFIED,
	FR_WS_TRIAL_INDEX, FR_WS_ACT, FR_WS_DONE
};

/* The operations of an action, in the order made (struct fr_act_plan). */
enum fr_op {
	FR_OP_END,
	FR_OP_SETTLE_BACK,	/* the selector settled on the running slot */
	FR_OP_CRC,		/* the slot's bytes are the ones its mark is for */
	FR_OP_LOG_ERASE,	/* the slot's last sector erased */
	FR_OP_LOG_PENDING,	/* its byte 0 programmed 0xFD, read back */
	FR_OP_LOG_MARK,		/* its byte 0 0xFD programmed 0xFC, read back */
	FR_OP_REC_ERASE,	/* the record sector erased */
	FR_OP_REC_PUT,		/* the record programmed */
	FR_OP_REC_CHECK,	/* the record, and a pending log, read back */
	FR_OP_SEL_ERASE,	/* the selector sector erased */
	FR_OP_SEL_PUT,		/* {slot, verified} programmed, read back */
	FR_OP_SETTLE,		/* the selector settled on the slot */
	FR_OP_TRIAL_ERASE,	/* a trial's selector steps (S-02.048) */
	FR_OP_TRIAL_VERIFIED,
	FR_OP_TRIAL_INDEX
};
#define FR_ACT_OPS 10u

struct fr_writer {
	const struct fr_flash *f;
	const uint8_t *img;
	uint32_t n;
	uint32_t crc;		/* the image's CRC-32 (S-02.046) */
	uint32_t rec_crc;	/* the record's crc32: crc, or its complement for a
				 * trial */
	uint8_t target;
	uint8_t way;		/* enum fr_way */
	struct fr_home *home;
	bool own;		/* the target is the running slot */
	bool rescue;		/* the plan's: the selector is settled on the
				 * target, with verified 0x01, a byte at a time */
	uint8_t state;		/* enum fr_writer_state */
	uint32_t pos;		/* the next byte of the step in progress */
	uint32_t rb_crc;	/* the read-back's CRC-32 so far */
	int result;		/* enum fr_write_rc once done */
	uint8_t image_rc;	/* enum fr_image_rc of a refused image */
	struct fr_sha256 sha;	/* over the read-back */
	uint8_t want_sha[FR_SHA256_LEN];	/* the image's, as announced */
	uint8_t front_pin[FR_SHA256_LEN];	/* with home: the running slot's front */
	uint32_t front_crc;
	uint8_t readback_sha[FR_SHA256_LEN];
	uint16_t version[4];	/* the image's version, with has_version */
	bool has_version;
	/* an action (FR_WS_ACT) */
	uint8_t act;		/* enum fr_act, FR_ACT_NONE for a write */
	uint8_t ops[FR_ACT_OPS];	/* enum fr_op, to FR_OP_END */
	uint8_t ip;		/* the operation that is next */
	uint8_t n_ops;
	bool rec_put;		/* FR_OP_REC_PUT is among them */
	bool log_pending;	/* FR_OP_LOG_PENDING is among them */
	bool bytewise;		/* the selector programmed a byte at a time */
	uint8_t sel_verified;	/* FR_OP_SEL_PUT's verified byte */
};

/* What the front knows of the running slot (fr_running_slot()). */
struct fr_running {
	bool established;
	uint8_t slot;
};

/*
 * What this recovery has done to the running slot, kept for a retry
 * (S-02.084): once a write of the running slot has begun, the slot's own
 * front can no longer be read from the flash, so the front it had stands
 * for it; and once the selector was written to name the other slot, the
 * running slot is no longer the selected one.
 */
struct fr_home {
	bool moved;		/* the selector write to the other slot was begun:
				 * set before its erase */
	bool spoiled;		/* a write of the running slot was begun: set
				 * before the slot's first erase */
	bool other_failed;	/* a write of the other slot ended in a flash or
				 * read-back failure: an in-place write may follow */
	bool unsettled;		/* set with spoiled; cleared when a write ends well,
				 * the selector then being settled */
	bool trial;		/* a trial write was begun: set before its first
				 * erase; no select follows in this recovery */
	uint8_t pin[FR_SHA256_LEN];	/* with spoiled: the running slot's front */
	uint32_t front_crc;
};

/* What a write would do, from fr_writer_plan(). */
struct fr_plan {
	uint8_t way;		/* enum fr_way */
	uint8_t target;		/* the slot written, or FR_NO_SLOT */
	uint8_t keep;		/* the slot not written */
	uint8_t refusal;	/* enum fr_write_rc when way is FR_WAY_NONE */
	bool own_good;		/* the running slot is good (S-02.016) */
	bool only_same;		/* the target is the only good slot: the write is
				 * accepted only for the image it holds, and then
				 * makes the read-back and the selector write alone */
	bool rescue;		/* the running slot is bad by a write of this
				 * recovery: the write ends by settling the selector
				 * on its target (fr_writer_step()) */
	bool trial_ok;		/* a trial may go: the start condition, the running
				 * slot good, and neither moved nor spoiled */
};

/* What of the flash a plan was given (struct fr_facts' known). */
#define FR_KNOW_A   0x01u	/* the running slot, a */
#define FR_KNOW_SEL 0x02u	/* the selector, sel */
#define FR_KNOW_B   0x04u	/* the other slot's check, b_good and b_tried */
#define FR_KNOW_LOG 0x08u	/* the other slot's log byte 0, b_started */

/* What a plan is decided from: the flash as read, and this recovery's
 * state. With A the running slot and B the other one. */
struct fr_facts {
	const struct fr_running *run;
	const struct fr_home *home;
	uint8_t flags;			/* BEGIN's */
	uint8_t known;			/* FR_KNOW_* */
	struct fr_selector sel;
	const struct fr_slot_info *a;	/* A, or any slot without an established A */
	bool b_good, b_tried, b_started;	/* B's, as in struct fr_slot_info */
	bool force;			/* FR_BEGIN_F_FORCE */
};

/*
 * fr_writer_plan()'s rules over the facts given, the one place they are
 * decided, for the writer and for what recovery announces: 0 with plan
 * and its refusal filled, or the FR_KNOW_* bit it needs next (plan's
 * refusal then FR_WR_READ), asked only where the decision depends on it.
 * Asked for FR_KNOW_A, the writer reads the selector with A; given A
 * without FR_KNOW_SEL (a selector that could not be read), the plan is
 * FR_WR_READ. front, when not NULL, gets the front as fr_writer_plan()
 * gives it.
 */
unsigned fr_plan_decide(const struct fr_facts *x, struct fr_plan *plan, struct fr_desc *front);

/*
 * The start condition (S-02.084, S-02.090), from the flash, and what a
 * write with these BEGIN flags would do. With A the running slot and B the
 * other one: the running slot established; the selector naming A, or B
 * when this recovery moved it there; verified 0x01; A's front valid in the
 * flash, or A spoiled by this recovery; and where the selector names B, B
 * good. Then: without the flag the slot the selector does not name; with
 * it A, by FR_WAY_SELECT_FIRST when B is good and, when B is bad, by
 * FR_WAY_IN_PLACE, but only once a write of B has failed in this recovery
 * (home->other_failed; else FR_WR_TRY_OTHER). front gets the pin and front
 * CRC an image must carry. FR_WR_OK, or the refusal (also in
 * plan->refusal).
 *
 * Once this recovery has begun a write of A and A reads bad (the damaged
 * state, plan->rescue), nothing the selector's conditions protect is left, and they are waived: every call
 * decides from the flash as it is, by one invariant: while A is damaged,
 * no selector sector is erased whose value already leads the first stage
 * to a good slot.
 *
 * Where B is good and the selector leads to it, either request writes A,
 * the slot the selector does not lead to, with no selector write first. It
 * leads to B when it names B, whatever verified reads, and when it names A
 * with verified 0x00 and A's record is sure to fail its check, since an
 * erase of that value, cut short, could read verified 0x01. One exception:
 * where it names B as slot 0 with bit 0 of verified set, programs alone
 * settle it, and the request without the flag does that for B's own
 * image.
 *
 * The state lasts until a write has ended well: a write of A that read A
 * back good and then failed to settle the selector leaves A good and the
 * state standing while B is bad. Then the flag with A's own image settles
 * the selector on A, erasing nothing of A, and the request without the
 * flag writes B. Else, without the flag the write goes to B; when B is good it is
 * the only good slot, and only the image it holds is accepted, for the
 * read-back and the selector write (plan->only_same). With the flag the
 * write goes to A: after the selector {B, 0x01} when B is good; in place
 * when B is bad.
 *
 * Every write in this state ends by settling the selector on its target,
 * read back good, so that no first stage has a selector write left to
 * make: nothing when the selector names the target with verified 0x01, or
 * names slot 1 as the target; for slot 0 with bit 0 of verified set,
 * programs alone, to {0x00, 0x01}, and no erase; else an erase and
 * {target, 0x01}. The selector write before a write of A with the flag is
 * the same settling, on B. Every selector program in this state writes the index byte, reads
 * it back and only then writes the verified byte: a program cut short
 * then never leaves a selector that trusts a slot it was not written for.
 *
 * FR_BEGIN_F_TRIAL plans as no flag does, and sets plan->trial_ok where a
 * trial may go: outside the damaged state, A good, the selector naming A,
 * and home neither moved nor spoiled. FR_BEGIN_F_SELECT plans the select
 * (FR_WAY_SELECT) by its two places: A's front intact and named with
 * verified 0x01, B tried and started, B the target; or A tried, B good and
 * named with verified 0x01, A the target (a trial start). Else
 * FR_WR_NOT_RUNNING, FR_WR_TRIAL_STATE when home is moved, spoiled or has
 * begun a trial, FR_WR_NOT_TRIED, or FR_WR_NOT_STARTED where the first
 * place holds but B has not started.
 *
 * FR_BEGIN_F_FORCE lifts the refusals that protect the user, and none that
 * states a fact (S-02.093): a selector that does not name A, or names it
 * with verified not 0x01; A's front gone from the flash, when no front is
 * then asked of the image; an in-place write before a write of B has
 * failed; a trial with A bad; a select of a slot that has not started, or
 * after a trial of this recovery. With the selector naming B, the write
 * goes to A, the slot it does not name, but only with B good.
 */
int fr_writer_plan(const struct fr_flash *f, const struct fr_running *run,
		   const struct fr_home *home, uint8_t flags, struct fr_plan *plan,
		   struct fr_desc *front);

/*
 * The image and the start condition, before any erase: the image valid
 * with a front (fr_image_valid()) and its SHA-256 equal to sha, checked
 * before the start condition is read (S-02.091); then fr_writer_plan();
 * then the image's pin and front CRC those of the running slot's front
 * (S-02.084). The image a slot already holds is written as any other: two
 * slots may hold the same image. FR_WR_OK
 * starts the write: then fr_writer_step() until done. A refusal ends the
 * writer (FR_WS_DONE, result the refusal) with nothing written, and home
 * unchanged. home changes only as the write's steps reach its erases, and
 * when a write of the other slot ends in a flash or read-back failure.
 *
 * Outside the damaged state, FR_WAY_PLAIN and FR_WAY_SELECT_FIRST end with
 * the selector {target, 0x00}, erased only after a read-back whose SHA-256
 * is sha, and are FR_WR_OK only when it then reads so; FR_WAY_IN_PLACE
 * writes no selector and ends with the read-back. FR_WAY_SELECT_FIRST
 * begins with the selector {other, 0x01}, read back before the first slot
 * erase.
 *
 * An image whose front is not the running one is FR_WR_FRONT without
 * FR_BEGIN_F_TRIAL; with it, FR_WAY_TRIAL where plan->trial_ok, else
 * FR_WR_TRIAL_STATE. With FR_BEGIN_F_FORCE and no trial flag it is written
 * and selected as the plan says, and so is a bootable image with no
 * waffle-boot front at all. The trial sets home->trial before its first erase and
 * writes B as a plain write does, but for byte 0 of B's log, programmed
 * pending after the log's erase, and the record's crc32, the complement of
 * the image's CRC-32; both read back so. Then the selector, each step read
 * back: erased, which ends it for slot 1; for slot 0 then verified 0xFE,
 * then the index 0x00. A selector step that fails settles the selector back
 * on A, {A, 0x01}, a byte at a time, and ends FR_WR_SELECTOR.
 *
 * A select writes nothing in the slot or its log sector, but from the
 * tried slot's own front clears bit 0 of its log's byte 0 where that entry
 * reads pending: then the record erased and programmed right, the
 * read-back, the selector {target, 0x00}; after the image was found byte
 * for byte in the target under the trial mark (else FR_WR_NOT_TRIED, which
 * comes before FR_WR_NOT_STARTED).
 */
int fr_writer_begin(struct fr_writer *w, const struct fr_flash *f,
		    const struct fr_running *run, struct fr_home *home,
		    const uint8_t *img, uint32_t n,
		    const uint8_t sha[FR_SHA256_LEN], uint8_t flags);
/* Ends a write that has made no step, with FR_WR_CHANGED: nothing was
 * written and home is unchanged. */
void fr_writer_cancel(struct fr_writer *w);
/* One step of S-02.049's order: one selector operation, one sector's erase
 * and its programs, the erase of the slot's last sector (S-02.045), one
 * record operation, or 4 KiB of the read-back. True once done (w->result
 * says how). */
bool fr_writer_step(struct fr_writer *w);
/* Bytes done of the whole, for a progress bar (0..1000). */
unsigned fr_writer_progress(const struct fr_writer *w);

/* What an action is decided from: the flash as read, and this recovery's
 * state. a is the running slot's, b the other one's. */
struct fr_act_facts {
	const struct fr_running *run;
	const struct fr_home *home;
	bool sel_ok;			/* the selector was read */
	struct fr_selector sel;
	const struct fr_slot_info *a, *b;
	uint8_t act;			/* enum fr_act */
	bool force;
};

struct fr_act_plan {
	uint8_t status;		/* enum fr_write_rc: FR_WR_OK, or the refusal */
	uint8_t target;		/* the slot acted on, or FR_NO_SLOT */
	uint8_t tier;		/* enum fr_tier of a try or a switch */
	uint8_t way;		/* enum fr_way */
	uint8_t n_ops;
	uint8_t ops[FR_ACT_OPS];	/* enum fr_op, in order */
	uint8_t sel_verified;	/* FR_OP_SEL_PUT's verified byte */
	bool mark;		/* FR_OP_REC_PUT writes the trial mark, else the
				 * right record */
};

/*
 * An action's rules (S-02.084), the one place they are decided, for the
 * writer and for what recovery shows. With A the running slot, established,
 * and B the other one. A slot is tried by its mark (fr_tried()), which says
 * that its bytes are the ones the mark was made for.
 *
 * A try marks B tried where it is good, sets its log pending where its
 * record gives a log, and writes the selector as a trial write does, so
 * that the first stage starts B once and names A again before B runs. A
 * bad B that is not tried gets no slot or log write, and no record write
 * but an erase where the first stage's check of the record is not sure to
 * fail (fr_record_fails()).
 *
 * A switch writes the selector {B, 0x00} for a good B, after a tried B's
 * record is made right; for a bad B, {B, 0x01}, the index first, which no
 * first stage ever checks. A slot whose stream fails S-02.091's rules is
 * never switched to (FR_WR_IMAGE).
 *
 * A keep makes a tried slot's record right and writes {slot, 0x00}: B from
 * A's front, or A itself in a trial start. A put back makes a tried B's
 * record right and writes no selector, but where the selector names B with
 * verified not 0x01, a try not yet started: there it is first settled back
 * on A.
 *
 * A try and a switch are refused with FR_WR_ALREADY where the selector
 * names B with verified 0x01. A try is refused once this recovery has
 * written A or moved the selector (FR_WR_TRIAL_STATE): the trial start
 * would have no slot to come back to. Without force: the default write's
 * selector condition; for a try, A good with its front and B good or tried
 * (FR_WR_TARGET_BAD); for a switch, B good with A's front (FR_WR_FRONT), or
 * tried, started and so; for a keep, the select's rule. While A is bad by
 * a write of this recovery, a switch settles the selector on B instead
 * (fr_slots.c's settle()), erasing no selector that leads to a good slot.
 *
 * A slot made good again, or switched to, has its log erased where entry 0
 * reads pending-coded, so that no trial's entry outlives the trial; a keep
 * of A in a trial start marks it finished instead (0xFD to 0xFC).
 */
void fr_act_decide(const struct fr_act_facts *x, struct fr_act_plan *plan);

/* The selector and both slots read from the flash, then fr_act_decide().
 * FR_WR_OK starts the action: then fr_writer_step() until done. A refusal
 * ends the writer with nothing written. plan, when not NULL, gets the
 * plan. */
int fr_act_begin(struct fr_writer *w, const struct fr_flash *f, const struct fr_running *run,
		 struct fr_home *home, uint8_t act, bool force, struct fr_act_plan *plan);

#endif /* FR_SLOTS_H */

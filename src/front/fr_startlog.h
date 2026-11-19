/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The start log (S-02.095): a count of starts that did not finish, kept in
 * the selected slot's last sector, two bits per start. The front clears an
 * entry's started bit just before it jumps to the main image, and the main
 * image clears its finished bit once it is alive (fr_handoff.h). Three
 * unfinished starts in a row open recovery, with no key.
 *
 * The front never erases for the log. Each start programs one bit, in a
 * byte of that one sector, and anything about the log that is not as
 * expected means: write nothing, go on to the main image.
 * Portable: the flash comes in through struct fr_flash.
 */
#ifndef FR_STARTLOG_H
#define FR_STARTLOG_H

#include "fr.h"
#include "fr_handoff.h"

/* Unfinished starts in a row that open recovery. */
#define FR_LOG_LIMIT 3u

/* An entry's two bits, the started bit low. */
#define FR_LOG_E_FREE       3u
#define FR_LOG_E_UNFINISHED 2u	/* started, not finished */
#define FR_LOG_E_FINISHED   0u	/* finished, or closed by the front */
#define FR_LOG_E_INVALID    1u	/* in a log; as entry 0 of the slot the
					 * selector does not name, a trial pending */

enum fr_log_state {
	FR_LOG_OK,	/* used entries, then free ones to the sector's end */
	FR_LOG_NONE,	/* the selected slot's record does not leave the sector free */
	FR_LOG_ODD,	/* anything else in the sector */
	FR_LOG_FULL,	/* no free entry */
	FR_LOG_IO	/* a read failed */
};

struct fr_log {
	uint8_t state;		/* enum fr_log_state */
	uint8_t slot;		/* the selected slot (S-02.011) */
	bool named;		/* the selector was read: slot is the one it names */
	uint8_t trial;		/* FR_LOG_TRIAL_*, from fr_log_step() */
	uint16_t next;		/* the first free entry */
	uint16_t unfinished;	/* the unfinished entries just before it */
	/* fr_log_step()'s reading of the running slot, where it made one */
	bool run_read;		/* fr_running_slots() ran and did not fail */
	bool run_est;		/* and established run_slot */
	uint8_t run_slot;
};

/* Reads the selector, the selected slot's record and its last sector. The
 * log is used only when the record has the tag and a length of 1 to
 * FR_LOG_IMAGE_MAX. */
void fr_log_scan(const struct fr_flash *f, struct fr_log *l);
/* Clears entry l->next's started bit and reads the byte back: true when it
 * reads as programmed. Nothing is written unless the entry reads free. */
bool fr_log_mark(const struct fr_flash *f, const struct fr_log *l);
/* Clears the finished bit of the newest entry, l->next - 1, and reads the
 * byte back. Nothing is written unless that entry reads unfinished. */
bool fr_log_close(const struct fr_flash *f, const struct fr_log *l);

/* The front's step before the jump to the main image. */
enum fr_log_step {
	FR_LOG_GO,		/* nothing written: go on, no entry to finish */
	FR_LOG_GO_MARKED,	/* entry l->next of slot l->slot marked: go on */
	FR_LOG_RECOVERY		/* FR_LOG_LIMIT unfinished starts, the newest closed */
};
/* fr_log_scan(), then fr_log_go(). */
enum fr_log_step fr_log_start(const struct fr_flash *f, struct fr_log *l);
/* The step on a log fr_log_scan() read: the newest entry closed after
 * FR_LOG_LIMIT unfinished starts, else entry l->next marked. */
enum fr_log_step fr_log_go(const struct fr_flash *f, const struct fr_log *l);

/*
 * S-02.095, S-02.093: a trial pending in the slot the selector does not
 * name, after fr_log_scan() read the selector into l: byte 0 of that
 * slot's log reads 0xFD (read alone first: one 1-byte read when no trial
 * is pending), its record has the tag and a length of 1 to
 * FR_LOG_IMAGE_MAX, and every other byte of the sector reads 0xFF. *slot
 * gets the slot. A read that fails is false.
 */
bool fr_log_trial_pending(const struct fr_flash *f, const struct fr_log *l, unsigned *slot);
/* A trial start of slot's own front: entry 0 marked finished (0xFD to
 * 0xFC, one bit) where it reads pending, and read back. */
bool fr_log_trial_mark(const struct fr_flash *f, unsigned slot);
/* A start of the slot the selector names while the other slot's trial is
 * pending: no trial start of it can come without a new trial write, which
 * erases the sector. Byte 0 0xFD to 0xF9 (entry 1's started bit, one bit),
 * read back: the gate fails at its first read from then on, and entry 0
 * neither reads pending to the gate nor finished. */
bool fr_log_trial_spend(const struct fr_flash *f, unsigned slot);

/* What fr_log_step() did of a pending trial. */
#define FR_LOG_TRIAL_NONE   0u
#define FR_LOG_TRIAL_MARKED 1u	/* this start runs from the pending slot, or
				 * both slots hold this image */
#define FR_LOG_TRIAL_SPENT  2u	/* this start runs from the slot named */

/*
 * The front's step before the main image (S-02.095, S-02.093):
 * fr_log_scan(), then, where the other slot holds a pending trial
 * (fr_log_trial_pending()) and the running slot is established from mem
 * (fr_running_slots()): a start from the pending slot marks it and logs
 * nothing (FR_LOG_GO), and so does a start with both slots holding this
 * image byte for byte (fr_slots_same()), which the pending slot's front
 * then is; a start from the slot named spends it. Else, and then,
 * fr_log_go(). l->trial says which.
 */
enum fr_log_step fr_log_step(const struct fr_flash *f, const struct fr_mem *mem, struct fr_log *l);

#endif /* FR_STARTLOG_H */

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's hand-off record as the full bootloader takes it (S-01.023,
 * front/fr_handoff.h), and the INIT stage's values it carries.
 */
#include "waffle/board/opxy_boot.h"

#include <stddef.h>
#include <string.h>

#include "waffle/bytes.h"
#include "waffle/wire_crc.h"

/* S-01.007, S-01.021: as the front found them at the INIT call (its
 * hand-off), logged by the master; 0 when there was none. */
uint32_t opxy_init_sp;
uint32_t opxy_init_dctl;
uint32_t opxy_init_dflt;
/* The INIT stage's stamps, for BOOT_TIMING=1 (waffle/board/opxy_boot.h). */
uint64_t opxy_init_cycles[3];

/* The record's layout on the host as on the target, up to its CRC; the host
 * adds only tail padding. */
_Static_assert(offsetof(struct fr_handoff, stamps) == 80u &&
	       offsetof(struct fr_handoff, run_slot) == 144u &&
	       offsetof(struct fr_handoff, link_gap_max_us) == 148u &&
	       offsetof(struct fr_handoff, crc32) == FR_HO_SIZE - 4u,
	       "S-01.023: the hand-off record's offsets");

bool opxy_front_accept(const uint8_t *rec, struct fr_handoff *out)
{
	const uint16_t version = wb_get_le16(rec + 4u), size = wb_get_le16(rec + 6u);
	uint32_t n;

	/* S-01.023: version 1 of 148 bytes or version 2 of 156, each with its
	 * CRC in its last four bytes; version 1 reads 0 in version 2's fields */
	if (wb_get_le32(rec) != FR_HO_MAGIC)
		return false;
	if (version == 1u && size == FR_HO_SIZE_V1)
		n = FR_HO_SIZE_V1 - 4u;
	else if (version == 2u && size == FR_HO_SIZE)
		n = FR_HO_SIZE - 4u;
	else
		return false;
	if (wire_crc32(0, rec, n) != wb_get_le32(rec + n))
		return false;
	memset(out, 0, sizeof(*out));
	memcpy(out, rec, n);
	out->crc32 = wb_get_le32(rec + n);
	return true;
}

bool opxy_front_running(const struct fr_handoff *ho, unsigned *slot)
{
	/* S-01.023: version 2's flag, with a slot that exists */
	if (ho->version < 2u || (ho->flags & FR_HO_RUNNING) == 0u || ho->run_slot > 1u)
		return false;
	*slot = ho->run_slot;
	return true;
}

bool opxy_front_no_trial(const struct fr_handoff *ho, unsigned *slot)
{
	return opxy_front_running(ho, slot) && (ho->flags & FR_HO_TRIAL_MARKED) == 0u;
}

bool opxy_front_take(struct fr_handoff *out)
{
#if defined(__bfin__) && !defined(OPXY_TEST_OS)
	uint8_t rec[FR_HO_SIZE];

	/* version 2's length; a version 1 record does not use the last 8 */
	memcpy(rec, (const void *)(uintptr_t)FR_HANDOFF_ADDR, sizeof rec);
	/* S-01.023: the record taken, its magic cleared, so a later stream
	 * finds none: L2 survives a hard reset (NOMEMINIT, opxy_reset.c). The
	 * DCPLB for L2 is write-through (cplb.c's bf703_dcplb), so the store
	 * reaches L2 without a flush. */
	*(volatile uint32_t *)(uintptr_t)FR_HANDOFF_ADDR = 0u;
	return opxy_front_accept(rec, out);
#else
	(void)out;
	return false;
#endif
}

bool opxy_log_finish_at(const struct fr_handoff *ho, uint32_t *off, uint8_t *value)
{
	if ((ho->flags & FR_HO_START_LOGGED) == 0u || ho->log_slot > 1u ||
	    ho->log_entry >= FR_LOG_ENTRIES)
		return false;
	*off = FR_LOG_OFF(ho->log_slot) + FR_LOG_BYTE(ho->log_entry);
	*value = (uint8_t)~FR_LOG_FINISHED(ho->log_entry);
	return true;
}

void opxy_start_finished(struct opxy_master *m)
{
	uint32_t off;
	uint8_t v;

	if (m->log_finished || !m->front)
		return;
	m->log_finished = true;
	/* A failed program leaves the entry unfinished; the boot goes on. */
	if (opxy_log_finish_at(&m->ho, &off, &v)) {
		opxy_flash_writer(&m->hw, true);	/* S-13b.017 */
		(void)m->flash.flash->program(m->flash.flash->ctx, off, &v, 1u);
		opxy_flash_writer(&m->hw, false);
	}
}

bool opxy_trial_reached_at(const struct s02_env *env, unsigned running, uint32_t *off)
{
	struct s02_selector sel;
	const uint32_t at = FR_LOG_OFF(running);
	uint32_t i;

	if (running > 1u || s02_read_selector(env, &sel) != S02_OK ||
	    s02_selected_slot(sel.index) == running)
		return false;
	for (i = 0; i < S02_SECTOR_SIZE; i += env->scratch_len) {
		const uint32_t k = S02_SECTOR_SIZE - i < env->scratch_len ?
					   S02_SECTOR_SIZE - i : env->scratch_len;
		uint32_t j;

		if (env->flash->read(env->flash->ctx, at + i, env->scratch, k) != 0)
			return false;
		for (j = 0; j < k; j++)
			if (env->scratch[j] != (i + j == 0u ? OPXY_TRIAL_STARTED_BYTE : 0xFFu))
				return false;
	}
	*off = at;
	return true;
}

void opxy_trial_reached(struct opxy_master *m)
{
	static const uint8_t reached = OPXY_TRIAL_REACHED_BYTE;
	uint32_t off;

	if (m->trial_marked || !m->front || !m->running_trial)
		return;
	m->trial_marked = true;
	if (opxy_trial_reached_at(&m->flash, m->running_slot, &off)) {
		opxy_flash_writer(&m->hw, true);	/* S-13b.017 */
		(void)m->flash.flash->program(m->flash.flash->ctx, off, &reached, 1u);
		opxy_flash_writer(&m->hw, false);
	}
}

/* --- S-02.101: the main image's erase of the start log (S-02.095's) --- */

_Static_assert(FR_LOG_OFF(0) == S02_SLOT0_OFF + S02_SLOT_CAPACITY - S02_SECTOR_SIZE &&
	       FR_LOG_OFF(1) == S02_SLOT1_OFF + S02_SLOT_CAPACITY - S02_SECTOR_SIZE,
	       "S-02.095: a slot's log is its last sector");
_Static_assert(OPXY_LOG_ERASE_USED >= 1u && OPXY_LOG_ERASE_USED <= FR_LOG_ENTRIES,
	       "the erase threshold lies inside the log");

/* Byte 0 of a trial's entry (S-02.095): pending, started, reached, spent. */
static bool trial_byte(uint8_t b)
{
	return b == 0xFDu || b == OPXY_TRIAL_STARTED_BYTE || b == OPXY_TRIAL_REACHED_BYTE ||
	       b == 0xF9u;
}

int opxy_log_scan(const struct s02_env *env, unsigned slot, struct opxy_log_scan *out)
{
	struct s02_record rec;
	uint32_t e = 0, i;
	bool used = true, rest_erased = true;

	memset(out, 0, sizeof(*out));
	out->state = OPXY_LOG_IO;
	if (slot > 1u || env->scratch_len == 0u || s02_read_record(env, slot, &rec) != S02_OK)
		return -1;
	/* the front's rule for "this slot has a log" */
	if (!s02_tag_ok(rec.tag) || rec.length < 1u || rec.length > FR_LOG_IMAGE_MAX) {
		out->state = OPXY_LOG_NONE;
		return 0;
	}
	out->state = OPXY_LOG_OK;
	for (i = 0; i < S02_SECTOR_SIZE; i += env->scratch_len) {
		const uint32_t k = S02_SECTOR_SIZE - i < env->scratch_len ?
					   S02_SECTOR_SIZE - i : env->scratch_len;
		uint32_t j;

		if (env->flash->read(env->flash->ctx, FR_LOG_OFF(slot) + i, env->scratch, k) != 0) {
			memset(out, 0, sizeof(*out));
			out->state = OPXY_LOG_IO;
			return -1;
		}
		for (j = 0; j < k; j++) {
			const uint8_t b = env->scratch[j];
			unsigned q;

			if (i + j == 0u)
				out->byte0 = b;
			else if (b != 0xFFu)
				rest_erased = false;
			for (q = 0; q < 4u && out->state == OPXY_LOG_OK; q++, e++) {
				const unsigned v = ((unsigned)b >> (2u * q)) & 3u;

				if (!used) {
					if (v != OPXY_LOG_E_FREE)
						out->state = OPXY_LOG_ODD;
				} else if (v == OPXY_LOG_E_FREE) {
					used = false;
					out->used = (uint16_t)e;
				} else if (v == OPXY_LOG_E_INVALID) {
					out->state = OPXY_LOG_ODD;
				} else if (v == OPXY_LOG_E_UNFINISHED) {
					out->unfinished++;
				} else {
					out->unfinished = 0;
				}
			}
		}
	}
	out->trial_alone = rest_erased && trial_byte(out->byte0);
	if (out->state == OPXY_LOG_OK && used) {
		out->state = OPXY_LOG_FULL;
		out->used = 0;
	}
	if (out->state != OPXY_LOG_OK) {
		out->used = 0;
		out->unfinished = 0;
	}
	return 0;
}

enum opxy_log_erase opxy_log_erase_at(const struct s02_env *env, const struct opxy_log_start *st,
				      uint32_t *off)
{
	struct s02_selector sel;
	struct opxy_log_scan mine, other;
	unsigned s;

	/* a normal start of an established slot, never a trial's (S-01.002) */
	if (!st->front || !st->established || st->trial || st->running > 1u)
		return OPXY_ERASE_NO;
	/* read again now, as the front reads them: the selector, then the
	 * selected slot's record and log */
	if (s02_read_selector(env, &sel) != S02_OK)
		return OPXY_ERASE_NO;
	s = s02_selected_slot(sel.index);
	if (s != st->running || opxy_log_scan(env, s, &mine) != 0)
		return OPXY_ERASE_NO;
	if (mine.state == OPXY_LOG_OK) {
		/* this start's entry, named by the front in this log and
		 * finished: the newest, so the next start counts none */
		if (!st->logged || st->log_slot != s || mine.used < OPXY_LOG_ERASE_USED ||
		    mine.unfinished != 0u || (uint32_t)st->log_entry + 1u != mine.used)
			return OPXY_ERASE_NO;
	} else if (mine.state == OPXY_LOG_FULL || mine.state == OPXY_LOG_ODD) {
		/*
		 * A trial's entry is one only in a tried slot's log, and this
		 * slot's stream was not read under the trial mark (st->trial),
		 * so an entry 0 of 1, 0 here is a torn erase's or an old
		 * trial's, and is erased with the rest. With no entry named,
		 * the front's step may have been the trial start's mark of an
		 * image both slots hold: no erase while the other slot's log
		 * reads a trial pending or just started (0xFD or 0xFC, the rest
		 * 0xFF), or cannot be read. Unless the front's version 2 record
		 * names this slot as the one it established and marked no
		 * trial: the other slot's mark is then a trial not taken in
		 * this start.
		 */
		if (!st->logged && !(st->no_trial && st->no_trial_slot == s)) {
			if (opxy_log_scan(env, 1u - s, &other) != 0 ||
			    (other.trial_alone && (other.byte0 == 0xFDu ||
						   other.byte0 == OPXY_TRIAL_STARTED_BYTE)))
				return OPXY_ERASE_NO;
		}
	} else {
		return OPXY_ERASE_NO;
	}
	*off = FR_LOG_OFF(s);
	return mine.state == OPXY_LOG_OK ? OPXY_ERASE_USED :
	       mine.state == OPXY_LOG_FULL ? OPXY_ERASE_FULL : OPXY_ERASE_ODD;
}

/* The sector at off reads 0xFF throughout. */
static bool reads_erased(const struct s02_env *env, uint32_t off)
{
	uint32_t i, j;

	for (i = 0; i < S02_SECTOR_SIZE; i += env->scratch_len) {
		const uint32_t k = S02_SECTOR_SIZE - i < env->scratch_len ?
					   S02_SECTOR_SIZE - i : env->scratch_len;

		if (env->flash->read(env->flash->ctx, off + i, env->scratch, k) != 0)
			return false;
		for (j = 0; j < k; j++)
			if (env->scratch[j] != 0xFFu)
				return false;
	}
	return true;
}

void opxy_start_log_tidy(struct opxy_master *m)
{
	static const char *const why[] = {
		[OPXY_ERASE_USED] = "used entries",
		[OPXY_ERASE_FULL] = "full",
		[OPXY_ERASE_ODD] = "not valid",
	};
	unsigned named = 2u;
	const bool no_trial = m->front && opxy_front_no_trial(&m->ho, &named);
	const struct opxy_log_start st = {
		m->front,
		(m->ho.flags & FR_HO_START_LOGGED) != 0u && m->ho.log_slot <= 1u &&
			m->ho.log_entry < FR_LOG_ENTRIES,
		m->ho.log_slot, m->ho.log_entry,
		m->running_established, m->running_slot, m->running_trial,
		no_trial, (uint8_t)named,
	};
	enum opxy_log_erase e;
	struct s14_text t;
	uint32_t off = 0;
	bool ok;

	if (m->log_tidied || !m->log_finished)
		return;
	m->log_tidied = true;
	/* a log the front named an entry in, below the threshold: nothing
	 * to read */
	if (st.logged && (uint32_t)st.log_entry + 1u < OPXY_LOG_ERASE_USED)
		return;
	e = opxy_log_erase_at(&m->flash, &st, &off);
	if (e == OPXY_ERASE_NO)
		return;
	opxy_flash_writer(&m->hw, true);	/* S-13b.017 */
	ok = m->flash.flash->erase(m->flash.flash->ctx, off) == 0;
	opxy_flash_writer(&m->hw, false);
	/* a failure is logged and stops nothing: the log is then odd, and the
	 * next good start erases it again */
	ok = ok && reads_erased(&m->flash, off);
	s14_text_init(&t);
	s14_text_str(&t, ok ? "start log erased, " : "start log erase failed, ");
	s14_text_str(&t, why[e]);
	s14_log_text(&m->sv.log, ok ? S14_INFO : S14_WARNING, S14_TAG("boot"), &t);
}

void opxy_start_log_view(struct opxy_master *m)
{
	struct s02_selector sel;
	struct opxy_log_scan v;
	struct s15_boot *b = &m->sv.boot;

	if (m->log_viewed)
		return;
	m->log_viewed = true;
	b->start_log = S15_LOG_UNREAD;
	b->start_log_slot = 0;
	b->start_log_used = 0;
	b->start_log_unfinished = 0;
	b->start_log_erase = OPXY_LOG_ERASE_USED;
	if (s02_read_selector(&m->flash, &sel) != S02_OK)
		return;
	b->start_log_slot = (uint8_t)s02_selected_slot(sel.index);
	if (opxy_log_scan(&m->flash, b->start_log_slot, &v) != 0)
		return;
	b->start_log = v.state == OPXY_LOG_OK ? S15_LOG_OK : v.state == OPXY_LOG_FULL ? S15_LOG_FULL :
		       v.state == OPXY_LOG_ODD ? S15_LOG_ODD : S15_LOG_NONE;
	b->start_log_used = v.used;
	b->start_log_unfinished = v.unfinished;
}

void opxy_start_log_reread(struct opxy_master *m)
{
	m->log_viewed = false;
	opxy_start_log_view(m);
}

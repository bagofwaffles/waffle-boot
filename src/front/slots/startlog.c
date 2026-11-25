/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The start log (see slots/startlog.h). */
#include "front/slots/startlog.h"

#include "front/slots/slots.h"

_Static_assert(FR_LOG_OFF(0) == FR_SLOT_BASE(0) + FR_SLOT_CAP - FR_SECTOR &&
	       FR_LOG_OFF(1) == FR_SLOT_BASE(1) + FR_SLOT_CAP - FR_SECTOR,
	       "S-02.095: the log is the slot's last sector");
_Static_assert(FR_LOG_ENTRIES == 4u * FR_SECTOR, "four entries to a byte");
_Static_assert(FR_LOG_IMAGE_MAX == FR_SLOT_CAP - FR_SECTOR, "the image ends before the log");

/* The sector is read through this buffer, a page at a time. */
static uint8_t page[FR_PAGE];

void fr_log_scan(const struct fr_flash *f, struct fr_log *l)
{
	struct fr_selector sel;
	struct fr_record rec;
	uint32_t e = 0;
	bool used = true;

	l->state = FR_LOG_IO;
	l->slot = 0;
	l->named = false;
	l->next = 0;
	l->unfinished = 0;
	if (fr_read_selector(f, &sel) != FR_FLASH_OK)
		return;
	l->slot = (uint8_t)fr_selected_slot(sel.index);
	l->named = true;
	if (fr_read_record(f, l->slot, &rec) != FR_FLASH_OK)
		return;
	if (rec.tag != FR_TAG || rec.length < 1u || rec.length > FR_LOG_IMAGE_MAX) {
		l->state = FR_LOG_NONE;
		return;
	}
	for (uint32_t off = 0; off < FR_SECTOR; off += FR_PAGE) {
		if (f->read(f->ctx, FR_LOG_OFF(l->slot) + off, page, FR_PAGE) != 0)
			return;
		for (uint32_t i = 0; i < FR_PAGE; i++) {
			const uint8_t b = page[i];

			if (!used) {
				if (b != 0xFFu) {
					l->state = FR_LOG_ODD;
					return;
				}
				continue;
			}
			for (unsigned q = 0; q < 4u; q++, e++) {
				const unsigned v = ((unsigned)b >> (2u * q)) & 3u;

				if (!used) {
					if (v != FR_LOG_E_FREE) {
						l->state = FR_LOG_ODD;
						return;
					}
				} else if (v == FR_LOG_E_FREE) {
					used = false;
					l->next = (uint16_t)e;
				} else if (v == FR_LOG_E_INVALID) {
					l->state = FR_LOG_ODD;
					return;
				} else if (v == FR_LOG_E_UNFINISHED) {
					l->unfinished++;
				} else {
					l->unfinished = 0;
				}
			}
		}
	}
	l->state = used ? FR_LOG_FULL : FR_LOG_OK;
}

/*
 * Clears mask in entry e's byte, when the entry reads want there, and
 * reads the byte back. The address is the slot's last sector for every
 * slot and entry this takes; anything else is refused before any access.
 */
static bool clear_bits(const struct fr_flash *f, unsigned slot, uint32_t e, unsigned want,
		       unsigned mask)
{
	uint32_t at;
	uint8_t old, v, back;

	if (slot > 1u || e >= FR_LOG_ENTRIES)
		return false;
	at = FR_LOG_OFF(slot) + FR_LOG_BYTE(e);
	if (f->read(f->ctx, at, &old, 1u) != 0)
		return false;
	if (((unsigned)old >> (2u * (e & 3u)) & 3u) != want)
		return false;
	v = (uint8_t)~mask;
	if (f->program(f->ctx, at, &v, 1u) != 0)
		return false;
	return f->read(f->ctx, at, &back, 1u) == 0 && back == (uint8_t)(old & v);
}

bool fr_log_mark(const struct fr_flash *f, const struct fr_log *l)
{
	if (l->state != FR_LOG_OK)
		return false;
	return clear_bits(f, l->slot, l->next, FR_LOG_E_FREE, FR_LOG_STARTED(l->next));
}

bool fr_log_close(const struct fr_flash *f, const struct fr_log *l)
{
	if (l->state != FR_LOG_OK || l->next == 0u)
		return false;
	return clear_bits(f, l->slot, l->next - 1u, FR_LOG_E_UNFINISHED,
			  FR_LOG_FINISHED(l->next - 1u));
}

enum fr_log_step fr_log_go(const struct fr_flash *f, const struct fr_log *l)
{
	if (l->state != FR_LOG_OK)
		return FR_LOG_GO;
	if (l->unfinished >= FR_LOG_LIMIT)
		return fr_log_close(f, l) ? FR_LOG_RECOVERY : FR_LOG_GO;
	return fr_log_mark(f, l) ? FR_LOG_GO_MARKED : FR_LOG_GO;
}

enum fr_log_step fr_log_start(const struct fr_flash *f, struct fr_log *l)
{
	fr_log_scan(f, l);
	return fr_log_go(f, l);
}

bool fr_log_trial_pending(const struct fr_flash *f, const struct fr_log *l, unsigned *slot)
{
	struct fr_record rec;
	uint8_t b;
	unsigned o;

	if (!l->named || l->slot > 1u)
		return false;
	o = 1u - l->slot;
	if (f->read(f->ctx, FR_LOG_OFF(o), &b, 1u) != 0 || b != FR_LOG0_PENDING)
		return false;
	if (fr_read_record(f, o, &rec) != FR_FLASH_OK || rec.tag != FR_TAG || rec.length < 1u ||
	    rec.length > FR_LOG_IMAGE_MAX)
		return false;
	for (uint32_t off = 0; off < FR_SECTOR; off += FR_PAGE) {
		if (f->read(f->ctx, FR_LOG_OFF(o) + off, page, FR_PAGE) != 0)
			return false;
		for (uint32_t i = 0; i < FR_PAGE; i++)
			if (page[i] != (off == 0u && i == 0u ? FR_LOG0_PENDING : 0xFFu))
				return false;
	}
	*slot = o;
	return true;
}

/* Byte 0 of slot's log reads exactly pending (0xFD). */
static bool pending(const struct fr_flash *f, unsigned slot)
{
	uint8_t b;

	return slot <= 1u && f->read(f->ctx, FR_LOG_OFF(slot), &b, 1u) == 0 && b == FR_LOG0_PENDING;
}

bool fr_log_trial_mark(const struct fr_flash *f, unsigned slot)
{
	return pending(f, slot) && clear_bits(f, slot, 0u, FR_LOG_E_INVALID, FR_LOG_STARTED(0u));
}

bool fr_log_trial_spend(const struct fr_flash *f, unsigned slot)
{
	return pending(f, slot) && clear_bits(f, slot, 1u, FR_LOG_E_FREE, FR_LOG_STARTED(1u));
}

enum fr_log_step fr_log_step(const struct fr_flash *f, const struct fr_mem *mem, struct fr_log *l)
{
	unsigned slot = 0, running = 0;
	bool established = false, both = false, same = false;

	fr_log_scan(f, l);
	l->trial = FR_LOG_TRIAL_NONE;
	l->run_read = l->run_est = false;
	l->run_slot = 0;
	/* one 1-byte read when no trial is pending; the running slot only
	 * where one is, and only an established one acts */
	if (fr_log_trial_pending(f, l, &slot) &&
	    fr_running_slots_checked(f, mem, &running, &established, &both) == FR_FLASH_OK) {
		l->run_read = true;
		l->run_est = established;
		l->run_slot = (uint8_t)running;
	}
	if (l->run_est) {
		/* both slots match this image's memory, and the flash holds
		 * the same bytes in both: the pending slot's image, its front
		 * included, is the one that has come this far, whichever slot
		 * the ROM read */
		if (both && running != slot && fr_slots_same(f, &same) != FR_FLASH_OK)
			same = false;
		if (running == slot || same) {
			/* a trial start of this front: no start logged */
			(void)fr_log_trial_mark(f, slot);
			l->trial = FR_LOG_TRIAL_MARKED;
			return FR_LOG_GO;
		}
		if (running == l->slot) {
			(void)fr_log_trial_spend(f, slot);
			l->trial = FR_LOG_TRIAL_SPENT;
		}
	}
	return fr_log_go(f, l);
}

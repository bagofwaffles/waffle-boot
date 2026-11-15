/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's hand-off record as the full bootloader takes it (S-01.023,
 * front/fr_handoff.h), and the INIT stage's values it carries.
 */
#include "waffle/board/opxy_boot.h"

#include <stddef.h>
#include <string.h>

#include "waffle/wire_crc.h"

/* S-01.007, S-01.021: as the front found them at the INIT call (its
 * hand-off), logged by the master; 0 when there was none. */
uint32_t opxy_init_sp;
uint32_t opxy_init_dctl;
uint32_t opxy_init_dflt;
/* The INIT stage's stamps, for BOOT_TIMING=1 (waffle/board/opxy_boot.h). */
uint64_t opxy_init_cycles[3];

bool opxy_front_take(struct fr_handoff *out)
{
#if defined(__bfin__) && !defined(OPXY_TEST_OS)
	const uint8_t *rec = (const uint8_t *)(uintptr_t)FR_HANDOFF_ADDR;
	struct fr_handoff ho;

	memcpy(&ho, rec, sizeof ho);
	/* S-01.023: the record taken, its magic cleared, so a later stream
	 * finds none: L2 survives a hard reset (NOMEMINIT, opxy_reset.c). The
	 * DCPLB for L2 is write-through (cplb.c's bf703_dcplb), so the store
	 * reaches L2 without a flush. */
	*(volatile uint32_t *)(uintptr_t)FR_HANDOFF_ADDR = 0u;
	if (ho.magic != FR_HO_MAGIC || ho.version != FR_HO_VERSION ||
	    ho.size != sizeof ho ||
	    wire_crc32(0, (const uint8_t *)&ho, offsetof(struct fr_handoff, crc32)) != ho.crc32)
		return false;
	*out = ho;
	return true;
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

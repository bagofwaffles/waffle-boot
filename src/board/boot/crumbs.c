/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Breadcrumbs for a trial boot (wiki/Test-OS.md, "The installer"): built only
 * with BOOT_CRUMBS=1, never in a release. A trial boot has no USB and maybe no
 * screen, so the master programs one byte per milestone into the last sector
 * of the slot the selector does not name, past the image and outside its
 * record's length. A trial boot runs from that slot (S-02.023), the installer
 * erased that sector for it and the test OS reads it back after the power
 * cycle. The selected slot's last sector is its start log (S-02.095), never
 * written here, and the installer never commits a breadcrumb build, so the
 * unselected slot is never a start log in use.
 *
 * The first crumb is the earliest point with DDR and C both running, so a unit
 * with no crumbs stopped before opxy_master_main: the stream, INIT or the start
 * code. Nothing here runs in INIT, where the ROM is still reading the flash.
 */
#include "waffle/board/boot.h"

#if OPXY_BOOT_CRUMBS
#include "waffle/drivers/bf70x/spiflash.h"
#include "waffle/drivers/bf70x/start.h"

static struct wb_spi crumb_spi;
/* The marks' sector; 0 on any doubt (crumb_sector()): no marks. */
static uint32_t crumb_off;

/*
 * The last sector of the slot the selector does not name (S-02.011), read
 * once: only when that slot's record has the tag and a length of 1 to
 * FR_LOG_IMAGE_MAX, so the sector lies outside its image (S-02.095's rule).
 * 0 otherwise, and on a read failure.
 */
static uint32_t crumb_sector(struct wb_spiflash *flash)
{
	uint8_t sel[2], raw[S02_RECORD_SIZE];
	struct s02_record rec;
	unsigned t;

	if (wb_spiflash_read(flash, S02_SELECTOR_OFF, sel, sizeof(sel)) != 0)
		return 0u;
	t = s02_other_slot(sel[0]);
	if (wb_spiflash_read(flash, s02_record_off(t), raw, sizeof(raw)) != 0)
		return 0u;
	s02_record_decode(raw, &rec);
	if (!s02_tag_ok(rec.tag) || rec.length < 1u || rec.length > FR_LOG_IMAGE_MAX)
		return 0u;
	return s02_slot_base(t) + S02_SLOT_CAPACITY - S02_SECTOR_SIZE;
}

void opxy_crumb(unsigned n)
{
	static struct wb_spiflash flash;
	const uint8_t mark = (uint8_t)n;

	if (flash.spi == NULL) {
		const uint32_t entry[3] = { opxy_init_sp, opxy_init_dctl, opxy_init_dflt };

		(void)wb_spi_init(&crumb_spi, OPXY_SPI_FLASH, &opxy_spi_flash, &opxy_master.hw.time,
				  OPXY_SPI_WORD_US);
		flash.spi = &crumb_spi;
		flash.size = OPXY_FLASH_SIZE;
		flash.busy_timeout_us = OPXY_FLASH_BUSY_US;
		flash.busy = true;	/* S-13b.015: the first frame an RDSR */
		crumb_off = crumb_sector(&flash);
		/* What the INIT stage kept (S-01.007, S-01.021). */
		if (crumb_off != 0u)
			(void)wb_spiflash_program(&flash, crumb_off + 0x20u,
						  (const uint8_t *)entry, sizeof(entry));
	}
	if (crumb_off != 0u)
		(void)wb_spiflash_program(&flash, crumb_off + n, &mark, 1u);
}

static void crumb_bytes(uint32_t at, const void *p, uint32_t len)
{
	struct wb_spiflash f = { 0 };

	if (crumb_off == 0u)
		return;
	f.spi = &crumb_spi;
	f.size = OPXY_FLASH_SIZE;
	f.busy_timeout_us = OPXY_FLASH_BUSY_US;
	f.busy = false;
	(void)wb_spiflash_program(&f, crumb_off + at, (const uint8_t *)p, len);
}

/* A coarse name for a space, no addresses: the other slot's are not ours. */
static uint8_t space_of(uint32_t addr)
{
	switch (addr >> 20) {
	case 0x118u:
		return 1u;	/* L1 data A */
	case 0x119u:
		return 2u;	/* L1 data B */
	case 0x11Bu:
		return 3u;	/* L1 scratch */
	case 0x080u:
		return 4u;	/* L2 */
	default:
		return addr >= 0x80000000u ? 6u : 5u;	/* DDR; anything else */
	}
}

static uint8_t last_space;

/* Each change of space, before the read that might not come back (0x34 on). */
void opxy_crumb_read(uint32_t addr)
{
	static uint32_t n;
	const uint8_t space = space_of(addr);

	if (space != last_space && n < 12u) {
		crumb_bytes(0x34u + n, &space, 1u);
		n++;
	}
	last_space = space;
}

/* An exception: crumb 15, then SEQSTAT and RETX (0x2C). */
void wb_core_fault(uint32_t seqstat, uint32_t ret)
{
	const uint32_t words[2] = { seqstat, ret };

	opxy_crumb(15u);
	crumb_bytes(0x2Cu, words, sizeof(words));
	for (;;)
		;
}
#else
typedef int opxy_crumbs_absent;	/* a translation unit is not empty */
#endif

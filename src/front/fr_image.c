/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The image descriptor and the loaded image's check (see fr_image.h). */
#include "fr_image.h"

/*
 * _fr_entry clears L1DM_DCTL.ENX, so a data load from L1 code faults, and
 * past each L1 data bank's 32 KB lie the extended-access aliases
 * (S-13a.040). L1 data is SRAM here, the data cache being off. Left out as
 * well: the first stage's stack while the ROM loads, 0x11807000-0x11808000
 * (S-02.091 rule 4), and what the front writes, its windows and the
 * record's half of the shared block. L2 stops at the fatal record
 * (S-16.104), DDR at the main image's end (bf703.ld), below the staging
 * buffer. tools/mkldr.py keeps the same list (FR_READABLE) and lists no
 * region outside it.
 */
static const struct {
	uint32_t lo, hi;
} readable[] = {
	{ 0x11800000u, FR_L1_DATA_START },	/* L1 data A, main's */
	{ FR_L1_DATA_END, 0x11807000u },
	{ 0x11900000u, 0x11908000u },		/* L1 data B */
	{ FR_DESC_ADDR, FR_HANDOFF_ADDR },	/* the descriptor's half */
	{ FR_SHARED_END, 0x0803DFD4u },		/* main's L2 */
	{ 0x8803D000u, 0x8833D000u },		/* main's DDR */
};

bool fr_mem_readable(uint32_t addr, uint32_t len)
{
	const uint64_t end = (uint64_t)addr + len;
	size_t i;

	for (i = 0; i < sizeof readable / sizeof readable[0]; i++)
		if (addr >= readable[i].lo && end <= readable[i].hi)
			return true;
	return false;
}

bool fr_desc_parse(const uint8_t *raw, size_t n, struct fr_desc *d)
{
	uint32_t size, i;

	if (n < FR_DESC_HEAD + 4u || fr_le32(raw + FR_DESC_O_MAGIC) != FR_DESC_MAGIC ||
	    fr_le16(raw + FR_DESC_O_VERSION) != FR_DESC_VERSION)
		return false;
	size = fr_le16(raw + FR_DESC_O_SIZE);
	d->n_regions = fr_le32(raw + FR_DESC_O_NREGIONS);
	if (d->n_regions > FR_DESC_REGIONS || size > n ||
	    size != FR_DESC_HEAD + 8u * d->n_regions + 4u ||
	    fr_crc32(0, raw, size - 4u) != fr_le32(raw + size - 4u))
		return false;
	d->main_entry = fr_le32(raw + FR_DESC_O_MAIN_ENTRY);
	d->main_crc = fr_le32(raw + FR_DESC_O_MAIN_CRC);
	d->front_crc = fr_le32(raw + FR_DESC_O_FRONT_CRC);
	for (i = 0; i < 4u; i++)
		d->bl_version[i] = fr_le16(raw + FR_DESC_O_BL_VERSION + 2u * i);
	fr_copy(d->pin, raw + FR_DESC_O_FRONT_PIN, FR_SHA256_LEN);
	for (i = 0; i < d->n_regions; i++) {
		d->addr[i] = fr_le32(raw + FR_DESC_O_REGIONS + 8u * i);
		d->len[i] = fr_le32(raw + FR_DESC_O_REGIONS + 8u * i + 4u);
	}
	return true;
}

bool fr_image_check(const struct fr_mem *mem, struct fr_desc *d)
{
	const uint8_t *raw = mem->map(mem->ctx, FR_DESC_ADDR, FR_DESC_MAX);
	uint32_t crc = 0, i;

	if (raw == NULL || !fr_desc_parse(raw, FR_DESC_MAX, d))
		return false;
	for (i = 0; i < d->n_regions; i++) {
		const uint8_t *p = mem->map(mem->ctx, d->addr[i], d->len[i]);

		if (p == NULL)
			return false;
		crc = fr_crc32(crc, p, d->len[i]);
	}
	return crc == d->main_crc;
}

void fr_handoff_seal(struct fr_handoff *ho)
{
	ho->magic = FR_HO_MAGIC;
	ho->version = FR_HO_VERSION;
	ho->size = (uint16_t)sizeof *ho;
	ho->crc32 = fr_crc32(0, (const uint8_t *)ho,
			     offsetof(struct fr_handoff, crc32));
}

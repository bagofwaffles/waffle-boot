/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-02 writers: the bootloader-slot upgrade, the metadata write and the
 * boot-time restore.
 */
#include "waffle/flash/layout.h"

#include "waffle/util/bytes.h"
#include "waffle/boot/memmap.h"

#include "front/handoff.h"

#define META_RETRIES 3u	/* attempts at steps 4-6 of S-02.064 */

static bool same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return false;
	return true;
}

static bool all_erased(const uint8_t *p, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		if (p[i] != S02_ERASED)
			return false;
	return true;
}

/*
 * Program [off, off + n) from buf, one call per page (S-02.019).  With
 * skip_erased, pages whose bytes are all 0xFF are not programmed (S-02.064
 * step 5).
 */
static int program_range(const struct s02_env *env, uint32_t off,
			 const uint8_t *buf, uint32_t n, bool skip_erased)
{
	while (n > 0u) {
		uint32_t room = S02_PAGE_SIZE - (off % S02_PAGE_SIZE);
		uint32_t len = n < room ? n : room;

		if (!(skip_erased && all_erased(buf, len)) &&
		    env->flash->program(env->flash->ctx, off, buf, len) != 0)
			return S02_ERR_IO;
		off += len;
		buf += len;
		n -= len;
	}
	return S02_OK;
}

/* Compare n flash bytes at off with buf, read in scratch-sized pieces. */
static int verify_range(const struct s02_env *env, uint32_t off,
			const uint8_t *buf, uint32_t n)
{
	while (n > 0u) {
		uint32_t chunk = n < env->scratch_len ? n : env->scratch_len;

		if (env->flash->read(env->flash->ctx, off, env->scratch, chunk) != 0)
			return S02_ERR_IO;
		if (!same(env->scratch, buf, chunk))
			return S02_ERR_VERIFY;
		off += chunk;
		buf += chunk;
		n -= chunk;
	}
	return S02_OK;
}

/*
 * Erase every sector that [off, off + n) overlaps, each just before the
 * programs of that sector, and program the bytes (S-02.045).  Sectors
 * wholly beyond the image are left as they are.
 */
static int write_extent(const struct s02_env *env, uint32_t off,
			const uint8_t *image, uint32_t n)
{
	uint32_t done = 0;

	while (done < n) {
		uint32_t sector = (off + done) - ((off + done) % S02_SECTOR_SIZE);
		uint32_t end = sector + S02_SECTOR_SIZE;
		uint32_t len = end - (off + done);
		int rc;

		if (len > n - done)
			len = n - done;
		if (env->flash->erase(env->flash->ctx, sector) != 0)
			return S02_ERR_IO;
		rc = program_range(env, off + done, image + done, len, false);
		if (rc != S02_OK)
			return rc;
		done += len;
	}
	return S02_OK;
}

/*
 * S-02.040 to S-02.053, S-02.056, S-02.090, S-02.091.  Under the option every
 * rule applies unchanged, a return to the factory bootloader included (S-02.058).
 */
int s02_slot_upgrade(const struct s02_env *env, bool option_on,
		     const uint8_t *image, uint32_t n, unsigned *target)
{
	return s02_slot_upgrade_start(env, option_on, image, n, target, NULL);
}

int s02_slot_upgrade_start(const struct s02_env *env, bool option_on,
			   const uint8_t *image, uint32_t n, unsigned *target,
			   struct s02_start *start)
{
	struct s02_selector sel;
	struct s02_record rec, back;
	uint8_t raw[S02_RECORD_SIZE];
	uint32_t base, crc;
	unsigned t;
	bool good;
	int rc;

	*target = 2u;
	if (start != NULL)
		start->read = false;
	/* S-02.056, S-02.057: no option, no write of a slot, record or selector. */
	if (!option_on)
		return S02_SKIPPED;
	/* S-02.044: refused before anything is erased or programmed. */
	if (n == 0u || n > S02_SLOT_CAPACITY)
		return S02_ERR_SIZE;
	/*
	 * S-02.091: a stream the ROM can boot from a slot, before the start
	 * condition is read.  The size check comes first, so 0 bytes and more
	 * than 0x78000 stay S02_ERR_SIZE; a 1-byte image passes S-02.044 and is
	 * refused here as Bad file (S-02.044, SC-UPG-SIZE).
	 */
	if (!s01_bootable(S01_BOOTLOADER, image, n))
		return S02_ERR_STREAM;

	/*
	 * S-02.043, S-02.090: one read of the selector decides both the start
	 * and the target.  It is never cached, so a second upgrade in the same
	 * boot reads the {target, 0x00} the first one wrote and is refused.
	 */
	rc = s02_read_selector(env, &sel);
	if (rc != S02_OK)
		return rc;
	if (start != NULL) {
		start->read = true;
		start->sel = sel;
		start->selected_good = false;
	}
	if (!s02_verified(sel.verified)) {
		/* reads only: a refused start erases and programs nothing */
		if (start != NULL &&
		    s02_check_slot(env, s02_selected_slot(sel.index), &good) == S02_OK)
			start->selected_good = good;
		return S02_ERR_START;
	}
	/* S-02.090: the selected slot reads good, after that read, before class 1. */
	rc = s02_check_slot(env, s02_selected_slot(sel.index), &good);
	if (rc != S02_OK)
		return rc;
	if (!good)
		return S02_ERR_START;
	t = s02_other_slot(sel.index);
	*target = t;
	base = s02_slot_base(t);

	/* S-02.049 class 1: the slot (S-02.045). */
	rc = write_extent(env, base, image, n);
	if (rc != S02_OK)
		return rc;
	/* S-02.045: an image that leaves the last sector free gets it erased. */
	if (n <= S02_SLOT_CAPACITY - S02_SECTOR_SIZE &&
	    env->flash->erase(env->flash->ctx,
			      base + S02_SLOT_CAPACITY - S02_SECTOR_SIZE) != 0)
		return S02_ERR_IO;
	/* S-02.113: an image that leaves the logo's header sector free gets it
	 * erased, so the new image starts on the front's own logo
	 * (front/handoff.h). */
	if (n <= FR_LOGO_SLOT_OFF &&
	    env->flash->erase(env->flash->ctx, base + FR_LOGO_SLOT_OFF) != 0)
		return S02_ERR_IO;
	/* S-02.103: an image that leaves the settings sector free gets it
	 * erased with the start log, so the new image starts on defaults. */
	if (n <= S02_SLOT_CAPACITY - 2u * S02_SECTOR_SIZE &&
	    env->flash->erase(env->flash->ctx,
			      base + S02_SLOT_CAPACITY - 2u * S02_SECTOR_SIZE) != 0)
		return S02_ERR_IO;

	/* Classes 2 and 3: the record (S-02.046). */
	rec.crc32 = env->crc32(0u, image, n);	/* software: the value written */
	rec.length = n;
	rec.tag = S02_RECORD_TAG;
	s02_record_encode(&rec, raw);
	if (env->flash->erase(env->flash->ctx, s02_record_off(t)) != 0)
		return S02_ERR_IO;
	if (env->flash->program(env->flash->ctx, s02_record_off(t), raw,
				S02_RECORD_SIZE) != 0)
		return S02_ERR_IO;

	/*
	 * Class 4: the read-back (S-02.047).  The record and the n slot bytes
	 * are read from the flash and must form a good slot whose crc32 is
	 * the image's.  The bytes are also compared with the image itself.
	 */
	rc = s02_read_record(env, t, &back);
	if (rc != S02_OK)
		return rc;
	if (back.crc32 != rec.crc32 || back.length != n || !s02_tag_ok(back.tag))
		return S02_ERR_VERIFY;
	rc = s02_flash_crc(env, base, n, &crc);
	if (rc != S02_OK)
		return rc;
	if (crc != rec.crc32)
		return S02_ERR_VERIFY;
	rc = verify_range(env, base, image, n);
	if (rc != S02_OK)
		return rc;

	/* Classes 5 and 6: the selector, {target, 0x00} (S-02.048). */
	if (env->flash->erase(env->flash->ctx, S02_SELECTOR_OFF) != 0)
		return S02_ERR_IO;
	raw[0] = (uint8_t)t;
	raw[1] = 0x00u;
	if (env->flash->program(env->flash->ctx, S02_SELECTOR_OFF, raw, 2u) != 0)
		return S02_ERR_IO;
	/* S-02.048: a part that ignored the program would leave the old
	 * slot starting while the step reports success. */
	rc = s02_read_selector(env, &sel);
	if (rc != S02_OK)
		return rc;
	if (sel.index != raw[0] || sel.verified != raw[1])
		return S02_ERR_VERIFY;
	return S02_OK;
}

uint32_t s02_meta_size(const struct s02_env *env)
{
	return env->meta_size == 0u || env->meta_size > S02_META_SIZE ? S02_META_SIZE
								       : env->meta_size;
}

/* S-02.063: s bytes of the record, then their complements at 0xB000 + s. */
void s02_mirror_build_n(const uint8_t *rec, uint32_t size, uint8_t *mirror)
{
	uint32_t i;

	for (i = 0; i < size; i++) {
		mirror[i] = rec[i];
		mirror[size + i] = (uint8_t)(rec[i] ^ 0xFFu);
	}
}

/* S-02.063: the magic, and each of the s bytes paired with its complement. */
bool s02_mirror_valid_n(const uint8_t *mirror, uint32_t size)
{
	uint8_t magic[4];
	uint32_t i;

	if (size < 4u)
		return false;
	wb_put_le32(magic, S02_META_MAGIC);
	if (!same(mirror, magic, 4u))
		return false;
	for (i = 0; i < size; i++)
		if ((uint8_t)(mirror[i] ^ mirror[size + i]) != 0xFFu)
			return false;
	return true;
}

void s02_mirror_build(const uint8_t rec[S02_META_SIZE], uint8_t mirror[S02_MIRROR_SIZE])
{
	s02_mirror_build_n(rec, S02_META_SIZE, mirror);
}

bool s02_mirror_valid(const uint8_t mirror[S02_MIRROR_SIZE])
{
	return s02_mirror_valid_n(mirror, S02_META_SIZE);
}

/*
 * Rewrite the metadata sector from the scratch copy (S-02.064 steps 4-6,
 * S-02.065).  The scratch must hold the whole sector as it is to be.
 */
static int meta_sector_store(const struct s02_env *env)
{
	uint32_t attempt;
	int rc = S02_ERR_IO;

	for (attempt = 0; attempt < META_RETRIES; attempt++) {
		uint32_t off;

		if (env->flash->erase(env->flash->ctx, S02_META_OFF) != 0) {
			rc = S02_ERR_IO;
			continue;
		}
		rc = program_range(env, S02_META_OFF, env->scratch,
				   S02_SECTOR_SIZE, true);
		if (rc != S02_OK)
			continue;
		/*
		 * The read-back: the scratch holds the expected sector, so read
		 * page by page into a local buffer.
		 */
		for (off = 0; off < S02_SECTOR_SIZE; off += S02_PAGE_SIZE) {
			uint8_t page[S02_PAGE_SIZE];

			if (env->flash->read(env->flash->ctx, S02_META_OFF + off,
					     page, S02_PAGE_SIZE) != 0) {
				rc = S02_ERR_IO;
				break;
			}
			if (!same(page, env->scratch + off, S02_PAGE_SIZE)) {
				rc = S02_ERR_VERIFY;
				break;
			}
		}
		if (off == S02_SECTOR_SIZE)
			return S02_OK;
	}
	return rc;
}

/*
 * S-02.064 step 0: the sector read twice into the scratch and compared, so
 * that a read that returned wrong bytes with no error is not written into
 * both copies. Nothing is erased when the two reads differ.
 */
static int meta_sector_read(const struct s02_env *env)
{
	uint32_t off;

	if (env->flash->read(env->flash->ctx, S02_META_OFF, env->scratch,
			     S02_SECTOR_SIZE) != 0)
		return S02_ERR_IO;
	for (off = 0; off < S02_SECTOR_SIZE; off += S02_PAGE_SIZE) {
		uint8_t page[S02_PAGE_SIZE];

		if (env->flash->read(env->flash->ctx, S02_META_OFF + off, page,
				     S02_PAGE_SIZE) != 0)
			return S02_ERR_IO;
		if (!same(page, env->scratch + off, S02_PAGE_SIZE))
			return S02_ERR_VERIFY;
	}
	return S02_OK;
}

/* S-02.060, S-02.061, S-02.064 */
int s02_meta_write(const struct s02_env *env, const uint8_t *rec)
{
	const uint32_t size = s02_meta_size(env);
	uint8_t mirror[S02_MIRROR_SIZE], back[S02_MIRROR_SIZE];
	int rc;

	if (env->scratch_len < S02_SECTOR_SIZE)
		return S02_ERR_IO;
	/* The sector as it is now: every byte not meant to change survives,
	 * those past a record of size bytes included (S-02.061). */
	rc = meta_sector_read(env);
	if (rc != S02_OK)
		return rc;
	wb_copy(env->scratch, rec, size);

	/* Steps 1-3: the mirror, 2 x size bytes and the sector erased past
	 * them, read back before 0xC000 is touched. */
	s02_mirror_build_n(rec, size, mirror);
	if (env->flash->erase(env->flash->ctx, S02_MIRROR_OFF) != 0)
		return S02_ERR_IO;
	if (program_range(env, S02_MIRROR_OFF, mirror, 2u * size, false) != S02_OK)
		return S02_ERR_IO;
	if (env->flash->read(env->flash->ctx, S02_MIRROR_OFF, back, 2u * size) != 0)
		return S02_ERR_IO;
	if (!same(back, mirror, 2u * size))
		return S02_ERR_VERIFY;

	/* Steps 4-6. */
	rc = meta_sector_store(env);
	if (rc != S02_OK)
		return rc;
	/*
	 * Step 7: the record is written and read back, so the mirror is
	 * erased: a mirror left behind would roll back a record another
	 * writer changes later (S-02.065). A cut or a failure here leaves the
	 * mirror whole and equal to the record, or not valid; either way the
	 * next start presents the record as written.
	 */
	(void)env->flash->erase(env->flash->ctx, S02_MIRROR_OFF);
	return S02_OK;
}

/* S-02.065 */
int s02_meta_restore(const struct s02_env *env, bool *restored)
{
	const uint32_t size = s02_meta_size(env);
	uint8_t mirror[S02_MIRROR_SIZE];

	*restored = false;
	if (env->scratch_len < S02_SECTOR_SIZE)
		return S02_ERR_IO;
	if (env->flash->read(env->flash->ctx, S02_MIRROR_OFF, mirror, 2u * size) != 0)
		return S02_ERR_IO;
	if (!s02_mirror_valid_n(mirror, size))
		return S02_OK;		/* use 0xC000 as it is */
	if (env->flash->read(env->flash->ctx, S02_META_OFF, env->scratch,
			     S02_SECTOR_SIZE) != 0)
		return S02_ERR_IO;
	if (same(env->scratch, mirror, size))
		return S02_OK;
	/* The mirror wins; the rest of the sector is kept as read. */
	wb_copy(env->scratch, mirror, size);
	*restored = true;
	return meta_sector_store(env);
}

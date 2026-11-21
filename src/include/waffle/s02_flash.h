/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-02: the master's SPI NOR flash: layout, selector, slot records, the
 * conforming first stage's decision and the writer's protocols (bootloader
 * slot, metadata mirror).
 *
 * Storage comes in through struct s02_flash_ops; the CRC-32 through an
 * s02_crc32_fn with zlib's calling convention.  Nothing here touches
 * hardware.
 */
#ifndef WAFFLE_S02_FLASH_H
#define WAFFLE_S02_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* S-02.001 geometry, S-02.002 area map, S-02.003 slot capacity. */
#define S02_FLASH_SIZE      0x100000u
#define S02_SECTOR_SIZE     0x1000u
#define S02_PAGE_SIZE       0x100u
#define S02_ERASED          0xFFu

#define S02_FIRST_STAGE_OFF 0x00000u
#define S02_FIRST_STAGE_MAX 0x0B000u	/* S-02.002 */
#define S02_MIRROR_OFF      0x0B000u	/* S-02.063 */
#define S02_META_OFF        0x0C000u	/* S-02.018 */
#define S02_SELECTOR_OFF    0x0D000u	/* S-02.010 */
#define S02_RECORD0_OFF     0x0E000u	/* S-02.013 */
#define S02_RECORD1_OFF     0x0F000u
#define S02_SLOT0_OFF       0x10000u
#define S02_SLOT1_OFF       0x88000u
#define S02_SLOT_CAPACITY   0x78000u
#define S02_PROTECTED_END   0x0D000u	/* S-02.005 */

#define S02_RECORD_SIZE     12u
#define S02_RECORD_TAG      0x0000B004u
/* The largest metadata record, the OP-XY's; a device's is
 * device.metadata_size (S-18.030), s02_env.meta_size. */
#define S02_META_SIZE       0x34u
#define S02_META_MAGIC      0xDEADBABEu
#define S02_MIRROR_SIZE     (2u * S02_META_SIZE)

/* Results.  0 is success; every other value is a refusal or a failure. */
enum s02_status {
	S02_OK = 0,
	S02_SKIPPED,		/* protected step skipped, option off (S-02.057) */
	S02_ERR_IO,		/* a read, erase or program failed */
	S02_ERR_SIZE,		/* image size refused (S-02.044) */
	S02_ERR_VERIFY,		/* a read-back did not match (S-02.047, .064, .071) */
	S02_ERR_START,		/* selector not verified, or the selected slot
				 * bad, at the start (S-02.090) */
	S02_ERR_NOT_GOOD,	/* selector write towards a bad slot (S-02.050) */
	S02_ERR_STREAM		/* the image is not a bootable stream (S-02.091);
				 * the step reports Bad file */
};

/*
 * The flash.  Every call returns 0 on success.  erase() takes the offset of
 * an aligned 4 KiB sector; program() writes within one 256-byte page and
 * ANDs into the stored bytes (S-02.019).
 */
struct s02_flash_ops {
	void *ctx;
	int (*read)(void *ctx, uint32_t off, uint8_t *buf, uint32_t len);
	int (*erase)(void *ctx, uint32_t sector_off);
	int (*program)(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len);
};

/*
 * CRC-32 with zlib's convention: crc32(0, buf, len) gives the CRC of buf,
 * and crc32(crc32(0, a, n), b, m) the CRC of a followed by b.  The CRC is
 * the one of S-02.015 (zlib, check value 0xCBF43926).
 */
typedef uint32_t (*s02_crc32_fn)(uint32_t crc, const uint8_t *buf, uint32_t len);

/*
 * What every function below works with.  scratch is caller memory of at
 * least S02_SECTOR_SIZE bytes; no function keeps a pointer to it after it
 * returns.
 */
struct s02_env {
	const struct s02_flash_ops *flash;
	s02_crc32_fn crc32;
	uint8_t *scratch;
	uint32_t scratch_len;
	/* S-02.016's check of a read image (s02_image_check()) tries this
	 * first when it is not NULL: a result equal to the record's CRC or its
	 * complement is the verdict, any other is computed again by crc32. */
	s02_crc32_fn crc32_fast;
	/* device.metadata_size (S-02.018, S-02.063): the record's bytes, at
	 * most S02_META_SIZE; 0 reads as S02_META_SIZE. */
	uint8_t meta_size;
};

/* The metadata record's size env works with (S-02.018). */
uint32_t s02_meta_size(const struct s02_env *env);

/* --- Formats (S-02.010 to S-02.016) --- */

struct s02_selector {
	uint8_t index;
	uint8_t verified;
};

struct s02_record {
	uint32_t crc32;
	uint32_t length;
	uint32_t tag;
};

/* S-02.011: index 0 selects slot 0, every other value slot 1. */
unsigned s02_selected_slot(uint8_t index);
/* S-02.043: the slot the boot rule does not select. */
unsigned s02_other_slot(uint8_t index);
/* S-02.012: only 0x01 means trusted. */
bool s02_verified(uint8_t verified);

uint32_t s02_slot_base(unsigned slot);
uint32_t s02_record_off(unsigned slot);

void s02_record_decode(const uint8_t raw[S02_RECORD_SIZE], struct s02_record *rec);
void s02_record_encode(const struct s02_record *rec, uint8_t raw[S02_RECORD_SIZE]);
/* S-02.014: all 32 bits must equal 0x0000B004. */
bool s02_tag_ok(uint32_t tag);
/* S-02.016 items 1 and 2, the parts that need no read of the slot. */
bool s02_record_plausible(const struct s02_record *rec);

int s02_read_selector(const struct s02_env *env, struct s02_selector *sel);
int s02_read_record(const struct s02_env *env, unsigned slot, struct s02_record *rec);

/*
 * CRC-32 of len flash bytes from off, read in scratch-sized pieces.
 * Returns S02_OK or S02_ERR_IO.
 */
int s02_flash_crc(const struct s02_env *env, uint32_t off, uint32_t len, uint32_t *crc);

/*
 * S-02.016: judge a slot by its record and contents.  A record that fails
 * the tag or the length rule is bad without a read of the slot (S-02.031).
 * *good is set on S02_OK; S02_ERR_IO means the judgment could not be made.
 */
int s02_check_slot(const struct s02_env *env, unsigned slot, bool *good);

/* --- The conforming first stage (S-02.030 to S-02.036) --- */

struct s02_fs_outcome {
	/*
	 * The slot handed to the ROM: the running slot, the slave's boot
	 * source (S-02.054). After a read fault it may differ from the slot
	 * the unwritten selector names (S-02.036).
	 */
	unsigned boot_slot;
	bool wrote;			/* one selector erase and program made */
	struct s02_selector written;	/* valid when wrote */
	unsigned checked;		/* bit k set: slot k was checked */
	bool selected_good;
	int status;			/* S02_OK, or the first I/O failure */
};

/*
 * One reset of a first stage (S-02.020-S-02.026).  With no_writes set (a
 * chord was read in this boot, S-02.082) the check-path write is left out.
 */
void s02_first_stage(const struct s02_env *env, bool no_writes, struct s02_fs_outcome *out);

/* --- The running slot (S-01.002, S-02.054; S-01 question 10) --- */

/*
 * The memory the running bootloader was loaded into: copy len bytes at addr
 * into buf, or return false where addr may not be read with data loads (L1
 * instruction memory, say); a block there is not compared.
 */
struct s02_mem_ops {
	void *ctx;
	bool (*read)(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len);
};

/*
 * The slot whose stream the boot ROM loaded in this boot, inferred as S-01
 * question 10 allows ("its own image against the slot records"), for a first
 * stage that passes no slot (S-01.006). Each slot's stream is walked as
 * S-05's LDR blocks, from the slot's base to its record's length (the slot's
 * capacity when the record is implausible), up to the FINAL block or the
 * first header that fails; the payload of each block that loads into memory
 * mem reads is compared with what that memory holds now. The running slot
 * is the one slot whose stream matches every byte compared, at least one
 * byte being compared, and where both do, the one whose stream reaches its
 * FINAL block (a stream cut short can match a part of the image). Where
 * both match and both reach their FINAL block the slots hold the same
 * image, and the answer is the slot later starts settle on: the slot the
 * selector names when it is verified or that slot is good (S-02.016),
 * otherwise the other slot. When neither matches, or both match and not
 * both reach their FINAL block, it is `fallback`, the selected slot. mem
 * must read only memory the stream loaded and nothing has written since.
 * Neither slot's CRC is consulted but for that one answer: a slot whose
 * check failed is booted once, one boot late (S-02.023). env->scratch
 * holds at least 2 x 16 bytes; it is split in halves. Returns S02_OK, or
 * S02_ERR_IO with *slot = fallback.
 */
int s02_running_slot(const struct s02_env *env, const struct s02_mem_ops *mem,
		     unsigned fallback, unsigned *slot);

/*
 * The same inference for the boot that streams the slot to the slave
 * (S-09.002), with that slot's stream read once: into memory, where
 * S-02.016's CRC is computed and from where it is compared and then sent.
 */

/* A read of the flash, as struct s02_flash_ops's. */
typedef int (*s02_read_fn)(void *ctx, uint32_t off, uint8_t *buf, uint32_t len);

struct s02_image {
	uint8_t *buf;		/* room for a slot's stream */
	uint32_t cap;
	s02_read_fn fast;	/* a faster read, or NULL: used for the stream read
				 * into buf alone, whose CRC stands behind it */
	void *fast_ctx;
	unsigned slot;		/* the slot buf holds; 2 for none */
	struct s02_record rec;	/* its record */
	uint32_t len;		/* its stream's bytes in buf: the record's length,
				 * or 0 when the record is implausible */
	bool good;		/* S-02.016 over buf, once checked */
	bool trial;		/* once checked: the record's CRC is the complement
				 * of buf's, the trial mark's (S-02.016): not good */
	bool by_fast;		/* buf was read by `fast` */
	unsigned known;		/* bit k: recs[k] is slot k's record, read once */
	unsigned probed;	/* bit k: slot k's probe chunks were compared */
	unsigned ruled_out;	/* bit k: and they differ */
	struct s02_record recs[2];
};

/* Bytes of the loaded image that tell one build from another, compared
 * before the rest; len 0 for none. */
struct s02_probe {
	uint32_t addr, len;
};

void s02_image_init(struct s02_image *im, uint8_t *buf, uint32_t cap, s02_read_fn fast,
		    void *fast_ctx);

/*
 * Read slot's record and, when it is plausible, its stream into the image.
 * A read that fails is made once more, a failed fast read at the usual
 * rate. Returns S02_OK with im->slot = slot, or S02_ERR_IO with the image
 * holding no slot.
 */
int s02_image_read(const struct s02_env *env, unsigned slot, struct s02_image *im);

/*
 * S-02.016 over the image: im->good. A stream read by `fast` whose CRC
 * fails is read once more at the usual rate and judged by that read, so a
 * slot is called bad only as s02_check_slot() would call it. Not when the
 * CRC is the complement of the record's, as under a trial mark: that match
 * confirms the bytes as an equal one does, the slot is bad as S-02.016 has
 * it, and nothing is read again.
 */
int s02_image_check(const struct s02_env *env, struct s02_image *im);

/*
 * The slot to read first: the one slot the probe's bytes do not rule out,
 * else `selected`. It reads each slot's record, the stream's block headers
 * up to the probe's bytes, and the chunks that hold them.
 */
unsigned s02_running_first(const struct s02_env *env, const struct s02_mem_ops *mem,
			   const struct s02_probe *probe, unsigned selected,
			   struct s02_image *im);

/*
 * s02_running_slot()'s answer, *slot = 2 when it gives its fallback (the
 * caller then takes the selected slot), with less reading: a slot the
 * image holds is compared there, once its CRC is good or the trial mark's,
 * or when the usual read filled it (s02_image_check() first); every other
 * byte compared comes by the flash's usual read; flash is read only where
 * mem reads the memory; a slot is left at its first chunk that differs;
 * and the probe's chunks are compared first. `first` is the slot compared
 * first. Where both slots hold the same image the selector is read for the
 * settled slot, and with a selector that is not verified the selected
 * slot's check is the image's when it holds that slot, with no read; else
 * that slot is read whole. A failed read is made once more; then
 * S02_ERR_IO and *slot = 2. The image is not changed: the caller reads the
 * slot it needs when the image holds another.
 */
int s02_running_image(const struct s02_env *env, const struct s02_mem_ops *mem,
		      const struct s02_probe *probe, struct s02_image *im, unsigned first,
		      unsigned *slot);

/* --- Writers (S-02.040 to S-02.058, S-02.090, S-02.091) --- */

/*
 * Bootloader-slot upgrade.  option_on is the session upgrade option
 * (S-02.055); with it off the step is skipped (S02_SKIPPED) and nothing is
 * read or written.  On every return *target is the target slot, or 2 when
 * none was chosen.
 */
int s02_slot_upgrade(const struct s02_env *env, bool option_on,
		     const uint8_t *image, uint32_t n, unsigned *target);

/* What an upgrade's S-02.090 start read: the selector, and for a selector
 * not verified whether the selected slot reads good. */
struct s02_start {
	bool read;			/* sel holds the selector read */
	struct s02_selector sel;
	bool selected_good;		/* checked only when not verified */
};

/*
 * s02_slot_upgrade(), with what its start read in *start. A start refused
 * for `verified` alone then also checks the selected slot, by reads only,
 * so that the caller can tell that case (S-11.068 item 5).
 */
int s02_slot_upgrade_start(const struct s02_env *env, bool option_on,
			   const uint8_t *image, uint32_t n, unsigned *target,
			   struct s02_start *start);

/*
 * S-02.050: a selector write by any mechanism other than the upgrade
 * (the recovery loader, S-02.084).  The slot is checked by reading
 * it immediately before the write; a bad slot gives S02_ERR_NOT_GOOD and the
 * selector is not touched.  The selector written is {slot, verified}.
 */
int s02_write_selector(const struct s02_env *env, unsigned slot, uint8_t verified);

/* --- The metadata mirror (S-02.060 to S-02.065) --- */

/*
 * S-02.063 with s = size: the mirror's 2s bytes for a record of s bytes
 * (the copy, then its complement), and whether 2s bytes read at 0xB000 are
 * a valid mirror of that size. size is at most S02_META_SIZE.
 */
void s02_mirror_build_n(const uint8_t *rec, uint32_t size, uint8_t *mirror);
bool s02_mirror_valid_n(const uint8_t *mirror, uint32_t size);
/* The same at the OP-XY's size, 0x34 (the mirror's 0x68 bytes). */
void s02_mirror_build(const uint8_t rec[S02_META_SIZE], uint8_t mirror[S02_MIRROR_SIZE]);
bool s02_mirror_valid(const uint8_t mirror[S02_MIRROR_SIZE]);

/* S-02.064: write a new record R of s02_meta_size(env) bytes.  R is S-04.043's.  The sector is read
 * twice and the reads must agree before the first erase (S02_ERR_VERIFY
 * otherwise, nothing erased); once the record is read back, the mirror
 * sector is erased (step 7). */
int s02_meta_write(const struct s02_env *env, const uint8_t *rec);

/* S-02.065: the boot-time restore.  *restored tells whether it wrote. */
int s02_meta_restore(const struct s02_env *env, bool *restored);

/*
 * Whether the bytes read through read from base on hold waffle-boot: LDR
 * block headers (S-05) up to FINAL within max bytes, one of them the image
 * descriptor at FR_DESC_ADDR whose magic, version, size and CRC hold
 * (S-01.023, front/fr_handoff.h). Reads headers and the descriptor alone.
 * Recovery's WB kind (S-02.106 rule 1), the installer's slot test (S-02.108)
 * and S-11.091's test of a package's bootloader image.
 */
bool s02_stream_is_waffle(int (*read)(void *ctx, uint32_t off, uint8_t *buf, uint32_t len),
			  void *ctx, uint32_t base, uint32_t max,
			  uint32_t (*crc32)(uint32_t crc, const uint8_t *p, size_t n));

#endif /* WAFFLE_S02_FLASH_H */

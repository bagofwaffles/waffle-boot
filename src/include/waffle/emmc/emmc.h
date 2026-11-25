/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-03: the eMMC as an on-disk interface.  The eMMC and its partitions, the
 * 512-byte OS header, the OS load rule, the OS writer with its write order
 * and failure paths, the eMMC features, and the user-area map.
 *
 * eMMC operations come in through struct s03_emmc_ops (on the OP-XY's
 * master they are bridge requests, S-09; on its slave and on the OP-1F's
 * main CPU, the eMMC driver).  Nothing here touches hardware.
 */
#ifndef WAFFLE_EMMC_EMMC_H
#define WAFFLE_EMMC_EMMC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/boot/device.h"

#define S03_SECTOR          512u
#define S03_GROUP_SECTORS   0x400u		/* S-03.001 erase group */
#define S03_USER_SECTORS    0x1CF8000u		/* S-03.001 */
#define S03_OS_MAX          0x800000u		/* S-03.042, S-01.014 */
#define S03_MIN_IMAGE       (10000u - 512u)	/* S-03.051: length + 512 >= 10000 */
#define S03_FORMAT_WRITTEN  3u			/* S-03.033 */
#define S03_ACTIVE_OFF      0x0Cu
#define S03_ENCRYPTED_OFF   0x0Du

/* Partition numbers (EXT_CSD[179] PARTITION_ACCESS). */
#define S03_PART_USER 0u
#define S03_PART_BP1  1u
#define S03_PART_BP2  2u

/* S-03.010: attempts per eMMC operation before it counts as failed. */
#define S03_RETRIES 3u

/*
 * The eMMC.  Every call returns 0 on success.  Sector numbers are relative
 * to the selected partition.  erase() names one erase group by its first
 * sector, which the driver sends as both CMD35 and CMD36 (S-03.004).
 * switch_byte() is a CMD6 SWITCH in write-byte mode of one EXT_CSD byte;
 * select(n) is the same with index 179 and value n on the OP-XY (S-03.005)
 * and s03_select_keep() on the OP-1F (S-03.012).
 */
struct s03_emmc_ops {
	void *ctx;
	int (*select)(void *ctx, unsigned part);
	int (*read)(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf);
	int (*write)(void *ctx, uint32_t sector, uint32_t count, const uint8_t *buf);
	int (*erase)(void *ctx, uint32_t first_sector);
	int (*switch_byte)(void *ctx, uint8_t index, uint8_t value);
};

/*
 * S-03.013's switch of the OP-1F: a CMD6 in write-byte mode whose busy wait
 * is CMD13 polled until the eMMC reports ready for data in the transfer
 * state, for at most limit_ms. ctx is the ops' ctx. Returns 0 done, 1 the
 * wait ran out, -1 the command failed.
 */
typedef int (*s03_switch_poll_fn)(void *ctx, uint8_t index, uint8_t value, uint32_t limit_ms);

/* CRC-32 with zlib's convention (S-03.025), as in S-02. */
typedef uint32_t (*s03_crc32_fn)(uint32_t crc, const uint8_t *buf, uint32_t len);

/*
 * What the load and the write work with.  boot_size is B, read from the eMMC
 * (EXT_CSD[226] x 128 KiB, S-03.008).  scratch is caller memory of at least
 * one sector; a multiple of 512 bytes is used.  max_xfer is the most sectors
 * one read or write may move (0: no limit).
 */
struct s03_env {
	const struct s03_emmc_ops *ops;
	s03_crc32_fn crc32;
	uint32_t boot_size;
	uint8_t *scratch;
	uint32_t scratch_len;
	uint32_t max_xfer;
	/* S-03.044's check of a loaded image tries this first when it is not
	 * NULL: a result equal to the header's CRC is the verdict, any other is
	 * computed again by crc32. */
	s03_crc32_fn crc32_fast;
};

/* --- The eMMC (S-03.001, S-03.002, S-03.009, S-03.011) --- */

/* S-03.001, S-03.002: SEC_COUNT, the erase group in sectors, and
 * OPTIMAL_WRITE_SIZE as the number EXT_CSD[265] holds, which passes when it
 * divides 8. The count is device.emmc_sectors: exactly that many, or with
 * emmc_sectors_at_least that many or more. */
bool s03_emmc_supported(const struct wb_device *dev, uint32_t sec_count,
			uint32_t erase_group_sectors, uint32_t opt_write);

/* S-03.009: the three feature switches before the first user-area read. */
int s03_emmc_features_on(const struct s03_env *env);

/*
 * S-03.011: the shutdown before power-off with the yaffs2 volume mounted.
 * unmount() flushes and unmounts the volume (S-12); it returns 0 on success.
 * Nothing reaches the eMMC after the power-off notification.
 */
int s03_emmc_shutdown(const struct s03_env *env, int (*unmount)(void *ctx), void *unmount_ctx);

/*
 * S-03.013, the OP-1F's: FLUSH_CACHE <- 1, CACHE_CTRL <- 0, the volume
 * unmounted, POWER_OFF_NOTIFICATION <- 3 by poll (NULL: the step fails) with a limit of
 * EXT_CSD[247] (power_off_long_time) x 10 ms. BKOPS_EN is not written. A
 * failed step, after S-03.010's retries, does not end the sequence; the
 * notification is retried only on a command error, never after its wait
 * ran out, and nothing follows it. Returns 0, or -1 when a step failed.
 */
int s03_emmc_shutdown_1f(const struct s03_env *env, int (*unmount)(void *ctx), void *unmount_ctx,
			 s03_switch_poll_fn poll, uint8_t power_off_long_time);

/* The wait limit of S-03.013 step 4, in ms, for EXT_CSD[247]; a byte of 0
 * (not defined by the eMMC) takes the longest, 255 x 10 ms. */
uint32_t s03_power_off_limit_ms(uint8_t power_off_long_time);

/*
 * S-03.012: the select of the OP-1F, which changes only PARTITION_ACCESS,
 * bits 2-0 of EXT_CSD[179]. cur is the byte as the eMMC holds it. Returns
 * false when bits 2-0 already equal part (no CMD6 is issued), else true with
 * *value = (cur AND 0xF8) OR part.
 */
bool s03_select_value(uint8_t cur, unsigned part, uint8_t *value);

/*
 * S-03.012's select as an s03_emmc_ops select: the current byte from an
 * EXT_CSD read (read_part_config, a CMD8) when it is not known, or from
 * the last switch that succeeded. A failed switch leaves it not known, so
 * the next select reads it first.
 */
struct s03_select_keep {
	void *ctx;
	int (*switch_byte)(void *ctx, uint8_t index, uint8_t value);
	int (*read_part_config)(void *ctx, uint8_t *value);
	uint8_t cur;
	bool known;
};

int s03_select_keep(struct s03_select_keep *k, unsigned part);
/* --- The OS header (S-03.020 to S-03.027) --- */

struct s03_header {
	uint32_t format;
	uint32_t length;
	uint32_t crc;
	uint8_t active;
	uint8_t encrypted;
};

enum s03_mode {
	S03_MODE_PLAIN = 0,	/* the stored region is read as plaintext */
	S03_MODE_ENCRYPTED,	/* format 2 or 3 marked encrypted: not loaded (S-03.048) */
	S03_MODE_REFUSED	/* format above 3: fails the load (S-03.042) */
};

void s03_header_decode(const uint8_t raw[S03_SECTOR], struct s03_header *h);
/* S-03.053: format 3, encrypted 0, reserved zero; active as given (0 or 1). */
void s03_header_encode(uint32_t length, uint32_t crc, uint8_t active, uint8_t raw[S03_SECTOR]);
/* S-03.022: any nonzero active byte. */
bool s03_header_active(const struct s03_header *h);
/* S-03.023, S-03.024 */
enum s03_mode s03_header_mode(const struct s03_header *h);
/* S-03.027: N = ceil(length / 512). */
uint32_t s03_sectors(uint32_t length);

/* --- Loading the OS (S-03.040 to S-03.048) --- */

enum s03_load_result {
	S03_LOAD_OK = 0,
	S03_LOAD_NO_ACTIVE,
	S03_LOAD_ZERO_LENGTH,
	S03_LOAD_TOO_LONG,
	S03_LOAD_PAST_PARTITION,
	S03_LOAD_FORMAT,
	S03_LOAD_ENCRYPTED,
	S03_LOAD_CRC,
	S03_LOAD_IO		/* a select or read failed (S-03.046) */
};

/* The request of a failed load's S03_LOAD_IO, the first that failed. */
enum s03_load_step {
	S03_STEP_NONE = 0,
	S03_STEP_SELECT,	/* a boot partition's select (S-03.040, S-03.041) */
	S03_STEP_HEADER,	/* its header's read */
	S03_STEP_IMAGE,		/* a read of the image's sectors (S-03.043) */
	S03_STEP_USER		/* the final select of the user area (S-03.007) */
};

struct s03_load {
	enum s03_load_result result;
	unsigned part;		/* the partition chosen, 0 when none */
	uint32_t length;	/* the image length, valid on S03_LOAD_OK */
	bool select0_failed;	/* the final select 0 failed: report it (S-14) */
	/* What a failed load knew, for the boot's account (S-05.006): */
	enum s03_load_step step;	/* S03_LOAD_IO: the request */
	unsigned step_part;	/* its partition (S03_PART_*) */
	uint32_t sector;	/* S03_STEP_IMAGE: the failed read's first sector */
	uint32_t count;		/* and its sectors */
	struct s03_header header;	/* the chosen partition's, once read */
	uint32_t crc;		/* S03_LOAD_CRC: the image's own */
};

/*
 * The load.  staging receives the image's sectors 1 to N, N x 512
 * bytes; staging_cap must be at least S03_OS_MAX.  A load whose final select
 * 0 fails is a failed load (S03_LOAD_IO), whatever it read before.
 */
void s03_load_os(const struct s03_env *env, uint8_t *staging, uint32_t staging_cap,
		 struct s03_load *out);

/* --- Writing the OS (S-03.050 to S-03.067) --- */

enum s03_write_result {
	S03_WRITE_OK = 0,
	S03_WRITE_REFUSED,	/* S-03.051: no eMMC operation made */
	S03_WRITE_FAILED
};

/* The writer's stages, in the order of S-03.055. */
enum s03_write_stage {
	S03_WS_ACCEPT = 0,	/* S-03.051 */
	S03_WS_TARGET,		/* S-03.052: BP1's header read, the target selected */
	S03_WS_ERASE,		/* step 1 */
	S03_WS_WRITE,		/* step 2 */
	S03_WS_VERIFY,		/* step 3 */
	S03_WS_HEADER,		/* steps 4 to 7 */
	S03_WS_DEACTIVATE,	/* step 8, S-03.058, with the second flush before it */
	S03_WS_FLUSH,		/* S-03.068's third flush */
	S03_WS_DONE		/* the write succeeded */
};

/* What stopped a write. */
enum s03_write_fail {
	S03_FAIL_NONE = 0,
	S03_FAIL_LENGTH,	/* S-03.051 refused the length */
	S03_FAIL_SELECT,
	S03_FAIL_READ,
	S03_FAIL_WRITE,
	S03_FAIL_ERASE,
	S03_FAIL_MISMATCH,	/* a read-back differed (S-03.056, S-03.057) */
	S03_FAIL_FLUSH		/* S-03.068 */
};

struct s03_write {
	enum s03_write_result result;
	unsigned target;	/* 0 when none was chosen */
	bool select0_failed;	/* S-03.063: reported, the result unchanged */
	/* Where a refused or failed write stopped; the final select 0 is not
	 * counted (select0_failed). */
	enum s03_write_stage stage;	/* the last stage begun */
	enum s03_write_fail fail;	/* the first failure */
	unsigned part;		/* the partition it was in, or the one selected */
	uint32_t at;		/* the sector of a read, write or mismatch; the group of an erase */
	bool recovery_failed;	/* S-03.057: the rewrite of the inactive copy failed */
};

/*
 * What a watched write reports as it goes; either function may be NULL.
 * stage() is called as each stage begins, with *w as far as it is known
 * (w->target from S03_WS_ERASE on), and with S03_WS_DONE once a write
 * has succeeded, after its final select 0.
 * progress() is called after each erase of S03_WS_ERASE (groups) and each
 * transfer of S03_WS_WRITE and S03_WS_VERIFY (sectors).
 */
struct s03_write_watch {
	void *ctx;
	void (*stage)(void *ctx, enum s03_write_stage stage, const struct s03_write *w);
	void (*progress)(void *ctx, uint32_t done, uint32_t total);
};

/* S-03.051 */
bool s03_image_acceptable(uint32_t length, uint32_t boot_size);

/* The writer (S-03.052 to S-03.063).  image is the plaintext OS image. */
void s03_write_os(const struct s03_env *env, const uint8_t *image, uint32_t length,
		  struct s03_write *out);
/* The same write, eMMC operation for eMMC operation, reported to watch
 * (NULL: as s03_write_os()). */
void s03_write_os_watched(const struct s03_env *env, const uint8_t *image, uint32_t length,
			  struct s03_write *out, const struct s03_write_watch *watch);

/* --- The user area (S-03.070 to S-03.073) --- */

enum s03_region { S03_REGION_YAFFS2 = 0, S03_REGION_UNUSED, S03_REGION_FAT, S03_REGION_NONE };

#define S03_YAFFS2_FIRST   0x000000u
#define S03_YAFFS2_LAST    0x47FFFFu
#define S03_UNUSED_FIRST   0x480000u
#define S03_UNUSED_LAST    0xCF7FFFu
#define S03_FAT_FIRST      0xCF8000u
#define S03_FAT_LAST       0x1CF7FFFu
#define S03_FAT_SECTORS    0x1000000u
/* S-03.071: one yaffs2 block per erase group, chunks of 8 sectors. */
#define S03_YAFFS2_CHUNK_SECTORS 8u
/* S-03.073: the factory reset's one erase in the FAT region. */
#define S03_FACTORY_RESET_GROUP 0x4000u

enum s03_region s03_user_region(uint32_t sector);
/*
 * S-03.070, S-03.075: the region of a user-area sector on dev. Without
 * fat-region only the yaffs2 row is defined: every sector from 0x480000
 * on is S03_REGION_NONE.
 */
enum s03_region s03_user_region_dev(const struct wb_device *dev, uint32_t sector);
/*
 * S-03.073, S-03.075: whether the bootloader may read, write or erase
 * user-area sectors [first, first + count) on dev, outside the volume's
 * own accesses: the yaffs2 region; with fat-region also the factory
 * reset's group (S-03.073). False for an empty or wrapping range.
 */
bool s03_user_access_ok(const struct wb_device *dev, uint32_t first, uint32_t count);
/* S-03.072: the user-area sector that holds FAT volume sector s. */
uint32_t s03_fat_volume_to_user(uint32_t s);

#endif /* WAFFLE_EMMC_EMMC_H */

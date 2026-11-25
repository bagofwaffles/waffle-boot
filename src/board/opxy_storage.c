/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The eMMC and the yaffs2 volume on the CPU that drives them (see
 * struct opxy_storage in waffle/board/opxy_boot.h): the OP-XY's slave
 * serves them over the bridge (S-09), the OP-1F's main CPU to its own
 * bridge client (S-13b.060, s09_master_local()).
 */
#include "waffle/board/opxy_boot.h"

#include <string.h>

#include "waffle/board/opxy_bridge.h"
#include "waffle/drivers/bf70x_core.h"
#include "waffle/bridge/records.h"
#include "waffle/util/crc.h"
#include "waffle/bridge/slave_files.h"
#include "waffle/fs/yaffs.h"
#include "wb_yaffs.h"

/* Each descriptor is four words the eMMC controller fetches as one burst:
 * aligned to its own size, so its place does not move with the link. */
static _Alignas(16) struct wb_msi_desc msi_desc[OPXY_MSI_DESC];
static _Alignas(32) uint8_t emmc_scratch[S03_SECTOR];	/* to the eMMC driver (S-13b.054) */
/* S-13b.054: a request's sectors pass through here when its buffer is not
 * on a cache line. Over the bridge the slave's own buffers always are; a
 * local client (S-13b.060) hands over its own, which may not be. */
static WB_NOINIT _Alignas(32) uint8_t bounce[S09_MAX_SECTORS * S03_SECTOR];
static struct wb_blockdev user_bd;
/* The eMMC commands of S-09.040 that s03_emmc_ops does not carry; the
 * yaffs2 volume's chunks take the reliable write too (S-12.060). */
static const struct opxy_emmc_more emmc_more = {
	wb_msi_emmc_erase_arg, wb_msi_emmc_write_reliable,
};
static struct wire_yaffs yaffs;
static struct wire_s09 files;
/* The failure notes (struct opxy_emmc_note) are the test OS's report's and
 * the host's tests'; the bootloader itself keeps none. */
#if defined(OPXY_TEST_OS) || !defined(__bfin__)
#define NOTES 1
#else
#define NOTES 0
#endif
/* The storage whose select keeps the byte (S-03.012); one per CPU. */
static struct opxy_storage *keep_of;

#if defined(OPXY_TEST_OS)
static int note(const struct opxy_storage *st, unsigned step, int rc)
{
	return st->note != NULL ? st->note(st->note_ctx, step, rc) : rc;
}
#define STEP_NOTE(st, step, x) note((st), (step), (x))
#else
#define STEP_NOTE(st, step, x) (x)
#endif

/* --- The eMMC's geometry (S-03.001, S-03.002) --- */

/* EXT_CSD bytes, JESD84-B51. */
#define EXT_CSD_ERASED_MEM_CONT    181u	/* bit 0: an erased byte reads 0xFF */
#define EXT_CSD_SEC_COUNT          212u	/* 4 bytes, little-endian */
#define EXT_CSD_BOOT_SIZE_MULT     226u
#define EXT_CSD_POWER_OFF_LONG     247u
#define EXT_CSD_CACHE_SIZE         249u	/* 4 bytes, little-endian; 0: no cache */
#define EXT_CSD_OPTIMAL_WRITE_SIZE 265u

/*
 * S-03.001, from the bring-up's EXT_CSD and CSD: SEC_COUNT, against
 * device.emmc_sectors; the CSD's erase group, since ERASE_GROUP_DEF is
 * never written and EXT_CSD[224] is not in force; and EXT_CSD[265] as the
 * number the byte holds, which must divide 8.
 */
static bool geometry_ok(const struct opxy_storage *st)
{
	const uint8_t *c = st->ext_csd + EXT_CSD_SEC_COUNT;
	const uint32_t sectors = (uint32_t)c[0] | (uint32_t)c[1] << 8 |
				 (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24;

	return s03_emmc_supported(st->dev, sectors, wb_msi_csd_erase_group(&st->hw->msi),
				  st->ext_csd[EXT_CSD_OPTIMAL_WRITE_SIZE]);
}

/* S-03.007, S-03.002: the user area's sectors only while it is selected,
 * and never another eMMC's. */
static bool user_area(const struct opxy_storage *st)
{
	return st->part == S03_PART_USER && !st->emmc_other;
}

/* --- The user area as a block device, for yaffs2 (S-12) --- */

/* file() refuses every request first, so these failures are a last guard. */
/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static int bd_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	const struct opxy_storage *st = ctx;
	int rc;

	if (!user_area(st))
		return -1;
	OPXY_TIMING_CHUNK(true, true, count);
	rc = st->emmc_ops.read(st->emmc_ops.ctx, lba, count, buf);
	OPXY_TIMING_CHUNK(false, rc == 0, count);
	return rc;
}

/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static int bd_write(void *ctx, uint32_t lba, const void *buf, uint32_t count)
{
	const struct opxy_storage *st = ctx;

	if (!user_area(st))
		return -1;
	return st->emmc_ops.write(st->emmc_ops.ctx, lba, count, buf);
}

/* S-12.060: whole chunks at a chunk's first sector, as one reliable write
 * (CMD23 with the count and bit 31, then CMD25; S-13b.054). */
/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static int bd_write_reliable(void *ctx, uint32_t lba, const void *buf,
			     uint32_t count)
{
	const struct opxy_storage *st = ctx;

	if (!user_area(st) || st->emmc_more == NULL || count == 0u ||
	    lba % WB_YAFFS_SECTORS_PER_CHUNK != 0u ||
	    count % WB_YAFFS_SECTORS_PER_CHUNK != 0u)
		return -1;
	return st->emmc_more->write_reliable(st->emmc_ops.ctx, lba, count, buf);
}

/* S-03.003: what an erased group reads as, from the bring-up's EXT_CSD,
 * before anything relies on it: zeros unless ERASED_MEM_CONT says 0xFF. */
/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static bool erase_zeroes(void *ctx)
{
	const struct opxy_storage *st = ctx;

	return (st->ext_csd[EXT_CSD_ERASED_MEM_CONT] & 1u) == 0u;
}

/* S-03.068: the eMMC has a volatile cache to flush (CACHE_SIZE not 0). */
static bool cache_present(const struct opxy_storage *st)
{
	const uint8_t *c = st->ext_csd + EXT_CSD_CACHE_SIZE;

	return (c[0] | c[1] | c[2] | c[3]) != 0u;
}

#if !defined(OPXY_YAFFS_ZERO_ERASE)
/* S-12.042: a block is one erase group, emptied by one eMMC erase of it
 * (CMD35 and CMD36 at its first sector, CMD38; S-03.004), tried at most
 * S03_RETRIES times (S-03.010), each try within the driver's bounds
 * (S-13b.057). */
/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static int bd_erase(void *ctx, uint32_t lba, uint32_t count)
{
	const struct opxy_storage *st = ctx;
	unsigned i;

	if (!user_area(st) || count != S03_GROUP_SECTORS ||
	    lba % S03_GROUP_SECTORS != 0u)
		return -1;
	for (i = 0; i < S03_RETRIES; i++)
		if (st->emmc_ops.erase(st->emmc_ops.ctx, lba) == 0)
			return 0;
	return -1;
}
#define BD_ERASE bd_erase
#else
/* make YAFFS_ZERO_ERASE=1: no erase operation, so the glue writes zeros
 * over the block, should an erased group not read 0x00 (S-03.003). */
#define BD_ERASE NULL
#endif

void opxy_storage_user_bd(struct opxy_storage *st, struct wb_blockdev *bd)
{
	bd->ctx = st;
	bd->n_sectors = S03_USER_SECTORS;
	bd->read = bd_read;
	bd->write = bd_write;
	bd->erase = BD_ERASE;
	bd->erase_zeroes = erase_zeroes;
	bd->write_reliable = bd_write_reliable;
}

/* --- S-03.012: the select of the OP-1F --- */

/* S-03.012: the byte by a CMD8 into the bring-up's copy of EXT_CSD, which
 * holds the same fields and the byte as the eMMC has it now. */
static int keep_part_config(void *ctx, uint8_t *value)
{
	struct opxy_storage *st = keep_of;

	if (st == NULL || wb_msi_read_part_config(ctx, st->ext_csd, value) != WB_MSI_OK)
		return -1;
	/* for the report: the value this select starts from, and the one a
	 * failed select left */
	if (!NOTES)
		return 0;
	st->pc_read = true;
	st->pc_value = *value;
	if (st->pc_after_pending) {
		st->pc_after_pending = false;
		st->note_select.pc_after = *value;
		st->note_select.pc_flags |= OPXY_NOTE_PC_AFTER;
	}
	return 0;
}

static int keep_switch(void *ctx, uint8_t index, uint8_t value)
{
	return wb_msi_emmc_switch(ctx, index, value) == WB_MSI_OK ? 0 : -1;
}

/* An s03_emmc_ops select: bits 2-0 of EXT_CSD[179] alone, no CMD6 when
 * they already name the partition. */
static int keep_select(void *ctx, unsigned part)
{
	(void)ctx;
	return keep_of != NULL ? s03_select_keep(&keep_of->sel, part) : -1;
}

/* The OP-1F's select (S-03.012) in place of S-03.005's. */
static bool select_keeps(const struct opxy_storage *st)
{
	return st->dev->id == WB_DEV_OP1F;
}

/* --- s09_store_ops --- */

static void emmc_wake(struct opxy_storage *st);

/*
 * S-03.009: the three feature switches, before the first request that
 * uses the eMMC: an eMMC request, the 0x08 mount, the release's load. Not
 * at start-up (S-09.018). They run on the bring-up's waits, so the worst,
 * every switch tried S03_RETRIES times with each wait running out, is
 * 3 * 3 * (CMD6, DAT0, CMD13, one more CMD13) = 9 * (2 * 0.25 + 2 * 0.1)
 * = 6.3 s, inside the request's T_EXEC (S-09.060, at least 10 s). If a
 * switch still fails, that request fails (S-03.010) and the next one
 * tries again. Once they are on, only the shutdown of S-03.011 (S-03.013
 * on the OP-1F) writes these bytes again, and after it a request makes
 * them again first (emmc_wake()); the hand-off finds them on (S-13b.058).
 */
bool opxy_storage_features(struct opxy_storage *st)
{
	emmc_wake(st);
	if (!st->emmc_up)
		return false;
	if (!st->features) {
		st->hw->msi.c = st->msi_bringup;
		st->features = STEP_NOTE(st, OPXY_TESTOS_STEP_FEATURES,
					s03_emmc_features_on(&st->emmc)) == 0;
		st->hw->msi.c = st->msi;
	}
	return st->features;
}

/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static uint32_t erase_group(void *ctx)
{
	const struct opxy_storage *st = ctx;

	/* S-09.030 0x03: S-03.001's erase group, the CSD's, in sectors; none
	 * from an eMMC that failed its bring-up (S-13b.059). */
	if (!st->emmc_up)
		return 0u;
	return wb_msi_csd_erase_group(&st->hw->msi);
}

/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static uint32_t boot_size(void *ctx)
{
	const struct opxy_storage *st = ctx;

	return st->emmc.boot_size;
}

/* The requests S-03.002 refuses on another eMMC: every write and erase, and
 * a read of the user area. Selects, the flush and boot-partition reads go
 * on: whether such an eMMC's OS is loaded is not specified. */
static bool other_refuses(uint8_t cmd, unsigned part)
{
	switch (cmd) {
	case S09_WRITE_SECTOR:
	case S09_WRITE_SECTORS:
	case S09_WRITE_RELIABLE:
	case S09_ERASE:
		return true;
	case S09_READ_SECTOR:
	case S09_READ_SECTORS:
		return part == S03_PART_USER;
	default:
		return false;
	}
}

/*
 * S-03.075: on a device without fat-region, no user-area sector from
 * 0x480000 on is read, written or erased, whoever asks. The sectors a
 * command names: one, a count, or an erase group.
 */
static bool footprint_ok(const struct opxy_storage *st, uint8_t cmd, uint32_t arg0,
			 uint32_t arg1)
{
	uint32_t count;

	if (wb_has(st->dev, WB_F_FAT_REGION) || st->part != S03_PART_USER)
		return true;
	switch (cmd) {
	case S09_READ_SECTOR:
	case S09_WRITE_SECTOR:
		count = 1u;
		break;
	case S09_READ_SECTORS:
	case S09_WRITE_SECTORS:
	case S09_WRITE_RELIABLE:
		count = arg1;
		break;
	case S09_ERASE:
		count = S03_GROUP_SECTORS;
		break;
	default:
		return true;
	}
	return s03_user_access_ok(st->dev, arg0, count);
}

/*
 * S-13b.054 on a buffer off a cache line: the same commands, with the
 * sectors copied through the bounce buffer. Anything else, a count out of
 * bounds included, goes on as it came, to opxy_slave_emmc()'s own checks.
 */
static int32_t emmc_sectors(struct opxy_storage *st, uint8_t cmd, uint32_t arg0,
			    uint32_t arg1, uint8_t *data, uint32_t n)
{
	/* the sector requests are 0x11 to 0x16, the erase (0x14) apart */
	const bool many = cmd == S09_READ_SECTORS || cmd == S09_WRITE_SECTORS ||
			  cmd == S09_WRITE_RELIABLE;
	const bool rd = cmd <= S09_READ_SECTORS;
	const uint32_t sectors = many ? arg1 : 1u;
	uint32_t bytes = 0;
	uint8_t *p = data;
	int32_t rc;

	_Static_assert(S09_READ_SECTOR + 1u == S09_READ_SECTORS &&
		       S09_READ_SECTORS < S09_WRITE_RELIABLE && S09_WRITE_RELIABLE < S09_ERASE &&
		       S09_ERASE < S09_WRITE_SECTOR && S09_WRITE_SECTOR < S09_WRITE_SECTORS,
		       "the sector requests' codes");
	/* the count against the bounce buffer first: no product that wraps */
	if (cmd >= S09_READ_SECTOR && cmd <= S09_WRITE_SECTORS && cmd != S09_ERASE &&
	    ((uintptr_t)data & (WB_MSI_BUF_ALIGN - 1u)) != 0u &&
	    sectors <= sizeof(bounce) / S03_SECTOR && sectors * S03_SECTOR <= n) {
		bytes = sectors * S03_SECTOR;
		p = bounce;
		if (!rd)
			memcpy(bounce, data, bytes);
	}
	rc = opxy_slave_emmc(&st->emmc_ops, st->emmc_more, cmd, arg0, arg1, p, n);
	if (rc == 0 && rd && p != data)
		memcpy(data, bounce, bytes);
	return rc;
}

#if !defined(__bfin__)
uint8_t *opxy_storage_bounce(void)
{
	return bounce;
}
#endif

/* The first failure of its kind, for the report. */
#if !NOTES
#define noted(st, cmd, arg0, arg1, why, known, cur, rc) ((void)(why), (void)(known), (void)(cur), (rc))
#else
static bool is_read(uint8_t cmd)
{
	return cmd == S09_READ_SECTOR || cmd == S09_READ_SECTORS;
}

static int32_t noted(struct opxy_storage *st, uint8_t cmd, uint32_t arg0, uint32_t arg1,
		     enum opxy_note_why why, bool known, uint8_t cur, int32_t rc)
{
	struct opxy_emmc_note *n;

	if (rc == 0)
		return rc;
	if (cmd == S09_SELECT && arg0 != S03_PART_USER && arg0 <= S03_PART_BP2)
		n = &st->note_select;
	else if (is_read(cmd))
		n = &st->note_read;
	else
		return rc;
	if (n->seen)
		return rc;
	memset(n, 0, sizeof(*n));
	n->seen = 1u;
	n->why = (uint8_t)why;
	n->part = (uint8_t)(cmd == S09_SELECT ? arg0 : st->part);
	n->sector = cmd == S09_SELECT ? 0u : arg0;
	n->count = cmd == S09_READ_SECTORS ? arg1 : cmd == S09_READ_SECTOR ? 1u : 0u;
	if (why == OPXY_NOTE_DRIVER || why == OPXY_NOTE_DOWN)
		n->f = st->hw->msi.fault;
	if (cmd == S09_SELECT) {
		if (known || st->pc_read) {
			n->pc_before = known ? cur : st->pc_value;
			n->pc_flags = OPXY_NOTE_PC_BEFORE;
		}
		st->pc_after_pending = true;
	}
	return rc;
}
#endif

/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static int32_t emmc_command(void *ctx, uint8_t cmd, uint32_t arg0,
			    uint32_t arg1, uint8_t *data, uint32_t n)
{
	struct opxy_storage *st = ctx;
	const bool known = st->sel.known;
	const uint8_t cur = st->sel.cur;
	int32_t rc;

	if (NOTES) {
		wb_msi_fault_clear(&st->hw->msi);
		st->pc_read = false;
	}
	emmc_wake(st);	/* S-03.011, S-03.013 */
	/* S-03.002: another eMMC's user area is not read or written, and
	 * neither boot partition written; no command reaches the eMMC. */
	if (st->emmc_other && other_refuses(cmd, st->part))
		return noted(st, cmd, arg0, arg1, OPXY_NOTE_REFUSED, known, cur, -1);
	if (!footprint_ok(st, cmd, arg0, arg1))
		return noted(st, cmd, arg0, arg1, OPXY_NOTE_REFUSED, known, cur, -1);
	/* S-13b.059: an error at once; emmc_more would reach the driver. */
	if (!opxy_storage_features(st) || st->emmc_more == NULL)
		return noted(st, cmd, arg0, arg1, OPXY_NOTE_DOWN, known, cur, -1);
	/* S-03.063: after a select that failed, no read, write or erase
	 * until a select succeeds. */
	if (st->part == OPXY_PART_UNKNOWN && cmd != S09_SELECT && cmd != S09_FLUSH)
		return noted(st, cmd, arg0, arg1, OPXY_NOTE_UNKNOWN, known, cur, -1);
	/* S-03.068: a flush where EXT_CSD shows a cache; with none there is
	 * nothing to commit, and no command is sent */
	if (cmd == S09_FLUSH && !cache_present(st))
		return 0;
	if (NOTES)
		wb_msi_fault_clear(&st->hw->msi);
	rc = emmc_sectors(st, cmd, arg0, arg1, data, n);
	if (cmd == S09_SELECT && arg0 <= S03_PART_BP2)
		st->part = rc == 0 ? arg0 : OPXY_PART_UNKNOWN;
	return noted(st, cmd, arg0, arg1,
		     st->hw->msi.fault.step != WB_MSI_STEP_NONE ? OPXY_NOTE_DRIVER :
								 OPXY_NOTE_REFUSED,
		     known, cur, rc);
}

/* S-03.007: the volume is the user area's, and is used only while the user
 * area is selected; until then every file request fails with ENODEV
 * (S-09.041), its unmount included, which could write. */
bool opxy_storage_volume_usable(const struct opxy_storage *st, uint32_t *err)
{
	if (st->part == S03_PART_USER)
		return true;
	*err = S09_ENODEV;
	return false;
}

struct wire_s09 *opxy_storage_files(void)
{
	return &files;
}

/* --- S-03.011, S-03.013: the eMMC shut down at the unmount --- */

/* The sequence's third step. yaffs2 writes the chunks it holds to the
 * eMMC as it unmounts. */
static int unmount_volume(void *ctx)
{
	(void)ctx;
	wire_s09_release(&files);
	wire_yaffs_unmount(&yaffs);
	return 0;
}

/*
 * At every 0x09 that finds the volume mounted, the shutdown's (S-04.063,
 * S-04.066) and a format's (S-12.057) alike, each step made whatever the
 * one before gave. On the OP-XY S-03.011: the flush, the cache off, the
 * unmount, background operations off, the power-off notification; the
 * driver sends nothing after Switch(34, 3) and waits for its busy end on
 * DAT0 alone, up to the eMMC's POWER_OFF_LONG_TIME (S-13b.056). On the
 * OP-1F S-03.013: the flush, the cache off, the unmount, the notification
 * with CMD13 polled up to EXT_CSD[247] x 10 ms; BKOPS_EN stays on. The
 * reply follows.
 */
static void emmc_shutdown(struct opxy_storage *st)
{
	if (st->dev->id == WB_DEV_OP1F)
		(void)s03_emmc_shutdown_1f(&st->emmc, unmount_volume, st, wb_msi_switch_poll,
					   st->ext_csd[EXT_CSD_POWER_OFF_LONG]);
	else
		(void)s03_emmc_shutdown(&st->emmc, unmount_volume, st);
	st->emmc_notified = true;
	st->features = false;
}

/* S-09.041, S-09.035 on S-12's volume. The store's ctx is the storage;
 * wire_s09's is its own state. Only the mount and the unmount reach the
 * eMMC first: every other command needs the volume mounted. */
static int32_t file(void *ctx, const struct s09_req *r, const char *path,
		    const char *path2, const uint8_t *data, uint32_t n,
		    uint8_t *out, uint32_t *err)
{
	struct opxy_storage *st = ctx;

	/* S-03.011: a mount after the notification brings the eMMC up
	 * first. S-03.002: no mount of another eMMC's user area; 0x08
	 * answers as any failed mount does (S-09.041). */
	if (r->cmd == S09_MOUNT)
		emmc_wake(st);
	if (r->cmd == S09_MOUNT && (st->emmc_other || !opxy_storage_features(st))) {
		*err = 0;
		return -1;
	}
	if (!opxy_storage_volume_usable(st, err))	/* S-09.041's failure result */
		return r->cmd == S09_OPENDIR || r->cmd == S09_READDIR ||
		       r->cmd == S09_FREE_SPACE || r->cmd == S09_TOTAL_SPACE ? 0 : -1;
	if (r->cmd == S09_UNMOUNT && yaffs.mounted) {
		emmc_shutdown(st);
		*err = 0;
		return 1;		/* S-09.041 */
	}
#if defined(OPXY_TEST_OS)
	if (r->cmd == S09_MOUNT) {
		const int32_t v = wire_s09_file(&files, r, path, path2, data, n,
						out, err);

		(void)note(st, OPXY_TESTOS_STEP_MOUNT,
			   v >= 0 ? 0 : *err != 0u ? (int)*err : 1);
		return v;
	}
#endif
	return wire_s09_file(&files, r, path, path2, data, n, out, err);
}

#if OPXY_BOOT_TIMING
/* A timing build: the mount's handling timed, with its chunk reads. */
static int32_t file_timed(void *ctx, const struct s09_req *r, const char *path,
			  const char *path2, const uint8_t *data, uint32_t n,
			  uint8_t *out, uint32_t *err)
{
	/* the boot's mount; the scan before a write (S-12.062) apart */
	const bool mount = r->cmd == S09_MOUNT && r->arg0 != S09_MOUNT_FOR_WRITE;
	int32_t v;

	if (mount)
		OPXY_TIMING_MOUNT_SLAVE(true);
	v = file(ctx, r, path, path2, data, n, out, err);
	if (mount)
		opxy_timing_mount_files(&yaffs.rep);
	else if (r->cmd == S09_MOUNT)
		opxy_timing_write_scan_slave(&yaffs.rep);
	if (mount)
		OPXY_TIMING_MOUNT_SLAVE(false);
	return v;
}
#define FILE_OP file_timed
#else
#define FILE_OP file
#endif

/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static int32_t read_file(void *ctx, uint32_t fd, uint8_t *buf, uint32_t n,
			 uint32_t *err)
{
	if (!opxy_storage_volume_usable(ctx, err))
		return -1;
	return wire_s09_read(&files, fd, buf, n, err);
}

/*
 * S-13b.051, S-13b.057: the whole bring-up, at most OPXY_EMMC_TRIES times.
 * Every step's failure ends a try, CMD8's included (its transfer did not
 * end with DTO and no data or IDMAC error). After the last failed try
 * EXT_CSD is cleared: no field of a partly received one is used
 * (S-13b.059). S-09.018: the bring-up's waits are opxy_msi_bringup's and
 * a repeat starts only within OPXY_EMMC_REPEAT_US, so the whole ends
 * within 4.5 s; the eMMC's later operations wait as opxy_msi says. The
 * bus is device.emmc_width lines wide (S-13b.061).
 */
bool opxy_storage_up(struct opxy_storage *st)
{
	struct opxy_hw *hw = st->hw;
	uint64_t start = wb_time_us(&hw->time);
	unsigned i;

	for (i = 0; i < OPXY_EMMC_TRIES; i++) {
		if (i > 0u &&
		    wb_time_us(&hw->time) - start >= OPXY_EMMC_REPEAT_US)
			break;
		if (STEP_NOTE(st, OPXY_TESTOS_STEP_MSI,
			      wb_msi_init(&hw->msi, st->msi_bringup,
					  &hw->time, msi_desc,
					  OPXY_MSI_DESC)) == WB_MSI_OK &&
		    STEP_NOTE(st, OPXY_TESTOS_STEP_EMMC,
			      wb_msi_emmc_init(&hw->msi,
					       st->ext_csd)) == WB_MSI_OK) {
			hw->msi.c = st->msi;
			return true;
		}
	}
	memset(st->ext_csd, 0, sizeof(st->ext_csd));
	return false;
}

void opxy_storage_start(struct opxy_storage *st, bool up)
{
	st->emmc_up = up;
	/*
	 * S-13b.059: after a failed bring-up no S-03 operation reaches the
	 * eMMC: each fails at once, so the load fails (S-03.046), the
	 * writer erases and writes nothing and the eMMC requests get an
	 * error. No feature switch, and no volume: nothing of the user area
	 * is read at an unknown bus width and clock.
	 */
	st->emmc_ops = up ? opxy_emmc_ops(st->hw) : opxy_emmc_down_ops;
	st->emmc_more = up ? &emmc_more : NULL;
	/* S-03.012: the select byte is read afresh after each bring-up */
	st->sel.ctx = &st->hw->msi;
	st->sel.switch_byte = keep_switch;
	st->sel.read_part_config = keep_part_config;
	st->sel.known = false;
	if (NOTES)
		st->pc_after_pending = false;
	if (up && select_keeps(st)) {
		keep_of = st;
		st->emmc_ops.select = keep_select;
	}
	/*
	 * S-03.001, S-03.002: before any user-area access. Another eMMC gets
	 * no volume and none of its user area is read or written, but the
	 * storage serves on. S-03.002's log line is not observable from the
	 * slave (S-14.026), which logs nothing; the master sees the 0x08 fail.
	 */
	st->emmc_other = up && !geometry_ok(st);
	st->emmc.boot_size = up ? (uint32_t)st->ext_csd[EXT_CSD_BOOT_SIZE_MULT] * 0x20000u : 0u;
	st->features = false;
}

/*
 * S-03.011, S-13b.050, S-13b.060: after the power-off notification, the
 * next request that needs the eMMC (a mount, an eMMC request 0x04 or
 * 0x11-0x18, the release's load) first wakes it: the bring-up of S-13b.051
 * again from CMD0, the geometry check (S-03.001) and, in
 * opxy_storage_features(), the switches of S-03.009 (S-03.009: a format's
 * unmount then a start of the OS hands the eMMC over with them on). Until
 * then no command reaches the eMMC (S-13b.056). A wake that fails is a
 * failed bring-up (S-13b.059). The selected partition is the user area,
 * as the 0x09 found it. A repeat of S-13b.057's bounds and the switches'
 * can pass T_EXEC's floor of 10 s (S-09.060).
 */
static void emmc_wake(struct opxy_storage *st)
{
	if (!st->emmc_notified)
		return;
	st->emmc_notified = false;
	opxy_storage_start(st, opxy_storage_up(st));
}

#if defined(OPXY_TEST_OS)
void opxy_storage_testos_volume(struct opxy_storage *st)
{
	st->part = S03_PART_USER;	/* S-09.040: until the first select */
	(void)note(st, OPXY_TESTOS_STEP_VOLUME,
		   wire_yaffs_add(&yaffs, "/yaffs2", &user_bd, 0u,
				  WB_YAFFS_LAST_BLOCK) ? 0 : 1);
	wire_s09_init(&files, &yaffs);
}
#endif

#if !defined(OPXY_TEST_OS)
static const struct opxy_storage *clock_of;

static uint64_t volume_us(void)
{
	return wb_time_us(&clock_of->hw->time);
}
#endif

/* S-13b.050-.059 (S-13b.060 on the OP-1F), S-03.008, then S-12's volume;
 * S-03.009 comes later. Runs on DDR, so after the DDR check. S-09.018: the
 * volume is added here and mounted only by the 0x08 (wire_s09_file());
 * until then every file command fails with ENODEV. */
void opxy_storage_init(struct opxy_storage *st, struct opxy_hw *hw,
		       const struct wb_device *dev, bool bring_up)
{
	const bool four = dev->mem.emmc_width == 4u;

	st->hw = hw;
	st->dev = dev;
	st->msi = four ? &opxy_msi_1f : &opxy_msi;
	st->msi_bringup = four ? &opxy_msi_bringup_1f : &opxy_msi_bringup;
	opxy_storage_start(st, bring_up && opxy_storage_up(st));
	st->emmc_notified = false;
	st->emmc.ops = &st->emmc_ops;
	st->emmc.crc32 = wire_crc32;	/* software: the reference behind crc32_fast */
	st->emmc.crc32_fast = opxy_crc32_check_quiet;	/* S-03.044, at the release */
	st->emmc.scratch = emmc_scratch;
	st->emmc.scratch_len = sizeof(emmc_scratch);
	st->emmc.max_xfer = 128u;
	opxy_storage_user_bd(st, &user_bd);
	st->part = S03_PART_USER;	/* S-09.040: until the first select */
	if (st->emmc_up && !st->emmc_other)
		(void)wire_yaffs_add(&yaffs, "/yaffs2", &user_bd, 0u,
				     WB_YAFFS_LAST_BLOCK);
#if !defined(OPXY_TEST_OS)
	/* S-12.061: the boot menu's mount restores the OS's checkpoint; the
	 * test OS scans, as before */
	clock_of = st;
	yaffs.may_restore = true;
	yaffs.now_us = volume_us;
#endif
	wire_s09_init(&files, &yaffs);
	st->store.ctx = st;
	st->store.erase_group = erase_group;
	st->store.boot_size = boot_size;
	st->store.emmc = emmc_command;
	st->store.file = FILE_OP;
	st->store.read = read_file;
}

/* S-05.037, S-13b.062: after the last eMMC access, before the kernel call. */
int opxy_storage_close(struct opxy_storage *st)
{
	if (st->hw == NULL || st->hw->msi.c == NULL)
		return WB_MSI_ERR_ARG;	/* never opened: as the reset left it */
	return wb_msi_close(&st->hw->msi);
}

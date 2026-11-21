/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The slave's yaffs2 volume on the board's user-area device
 * (opxy_slave_user_bd()) over an eMMC model in RAM that logs every
 * operation, scenario SC-12-YAFFS-EMMC (S-12.042, S-12.060): files
 * written and deleted until the volume's space has been used over twice
 * and yaffs2 has reclaimed blocks. Every write to the volume is a reliable
 * write of whole chunks at a chunk's first sector, none a plain write;
 * every block that ends the run empty, having held data, was emptied by
 * one eMMC erase at its first sector and by no write; no write carries a
 * chunk of zeros. The erase itself: one group, only while the user area is
 * selected, tried at most S03_RETRIES times (S-03.010); the reliable
 * write, whole chunks only, on the user area only. test_board_storage
 * checks the commands the driver sends for these operations (CMD23 with
 * bit 31, then CMD25; CMD35, CMD36, CMD38).
 */
#include <stdlib.h>
#include <string.h>

#include "support/check.h"
#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_bridge.h"
#include "waffle/s03_emmc.h"
#include "waffle/wire_yaffs.h"

#define BLOCKS 24u
#define SECTORS (BLOCKS * S03_GROUP_SECTORS)
#define CHUNK_SECTORS 8u

/* ---- the eMMC model ---- */

enum op_kind { OP_WRITE, OP_RELIABLE, OP_ERASE };

struct op {
	enum op_kind kind;
	uint32_t sector, count;
};

static uint8_t *vol;
static struct op *ops;
static size_t n_ops, cap_ops;
static unsigned zero_chunk_writes;	/* chunks of 0x00 written */
static unsigned erase_calls;
static unsigned erase_fail_left;	/* erases that fail before one works */

static void log_op(enum op_kind kind, uint32_t sector, uint32_t count)
{
	if (n_ops == cap_ops) {
		cap_ops = cap_ops ? 2u * cap_ops : 4096u;
		ops = realloc(ops, cap_ops * sizeof(*ops));
		if (ops == NULL)
			abort();
	}
	ops[n_ops].kind = kind;
	ops[n_ops].sector = sector;
	ops[n_ops].count = count;
	n_ops++;
}

static bool in_volume(uint32_t sector, uint32_t count)
{
	return sector <= SECTORS && count <= SECTORS - sector;
}

static bool all_zero(const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (p[i] != 0u)
			return false;
	return true;
}

static int em_select(void *ctx, unsigned part)
{
	(void)ctx;
	(void)part;
	return 0;
}

static int em_read(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf)
{
	(void)ctx;
	if (!in_volume(sector, count))
		return -1;
	memcpy(buf, vol + (size_t)sector * S03_SECTOR, (size_t)count * S03_SECTOR);
	return 0;
}

static int em_store(enum op_kind kind, uint32_t sector, uint32_t count,
		    const uint8_t *buf)
{
	uint32_t c;

	log_op(kind, sector, count);
	if (!in_volume(sector, count))
		return -1;
	for (c = 0; c + CHUNK_SECTORS <= count; c += CHUNK_SECTORS)
		if (all_zero(buf + (size_t)c * S03_SECTOR,
			     (size_t)CHUNK_SECTORS * S03_SECTOR))
			zero_chunk_writes++;
	memcpy(vol + (size_t)sector * S03_SECTOR, buf, (size_t)count * S03_SECTOR);
	return 0;
}

static int em_write(void *ctx, uint32_t sector, uint32_t count,
		    const uint8_t *buf)
{
	(void)ctx;
	return em_store(OP_WRITE, sector, count, buf);
}

static int em_write_reliable(void *ctx, uint32_t sector, uint32_t count,
			     const uint8_t *buf)
{
	(void)ctx;
	return em_store(OP_RELIABLE, sector, count, buf);
}

/* S-03.003: an erased group reads 0x00. */
static int em_erase(void *ctx, uint32_t first)
{
	(void)ctx;
	erase_calls++;
	if (erase_fail_left > 0u) {
		erase_fail_left--;
		return -1;
	}
	log_op(OP_ERASE, first, S03_GROUP_SECTORS);
	if (!in_volume(first, S03_GROUP_SECTORS))
		return -1;
	memset(vol + (size_t)first * S03_SECTOR, 0,
	       (size_t)S03_GROUP_SECTORS * S03_SECTOR);
	return 0;
}

static int em_switch(void *ctx, uint8_t index, uint8_t value)
{
	(void)ctx;
	(void)index;
	(void)value;
	return 0;
}

static int em_erase_arg(void *ctx, uint32_t first, uint32_t arg)
{
	(void)arg;
	return em_erase(ctx, first);
}

static const struct s03_emmc_ops em_ops = {
	NULL, em_select, em_read, em_write, em_erase, em_switch,
};
static const struct opxy_emmc_more em_more = { em_erase_arg, em_write_reliable };

static struct opxy_slave s;
static struct wb_blockdev bd;
static struct wire_yaffs Y;

static void reset_log(void)
{
	n_ops = 0;
	zero_chunk_writes = 0;
	erase_calls = 0;
	erase_fail_left = 0;
}

/* ---- the erase operation ---- */

static void test_erase_op(void)
{
	reset_log();
	CHECK(bd.erase != NULL);
	if (bd.erase == NULL)
		return;
	CHECK(bd.erase(bd.ctx, 3u * S03_GROUP_SECTORS, S03_GROUP_SECTORS) == 0);
	CHECK(n_ops == 1u && ops[0].kind == OP_ERASE &&
	      ops[0].sector == 3u * S03_GROUP_SECTORS);

	/* one whole group, named by its first sector (S-03.004) */
	reset_log();
	CHECK(bd.erase(bd.ctx, 3u * S03_GROUP_SECTORS + 8u, S03_GROUP_SECTORS) != 0);
	CHECK(bd.erase(bd.ctx, 3u * S03_GROUP_SECTORS, S03_GROUP_SECTORS / 2u) != 0);
	CHECK(bd.erase(bd.ctx, 3u * S03_GROUP_SECTORS, 2u * S03_GROUP_SECTORS) != 0);
	CHECK(erase_calls == 0u && n_ops == 0u);

	/* S-03.007: only while the user area is selected */
	s.st.part = S03_PART_BP1;
	CHECK(bd.erase(bd.ctx, 0u, S03_GROUP_SECTORS) != 0);
	s.st.part = OPXY_PART_UNKNOWN;
	CHECK(bd.erase(bd.ctx, 0u, S03_GROUP_SECTORS) != 0);
	CHECK(erase_calls == 0u);
	s.st.part = S03_PART_USER;

	/* S-03.010: an erase that fails is tried again, a bounded number of
	 * times */
	reset_log();
	erase_fail_left = S03_RETRIES - 1u;
	CHECK(bd.erase(bd.ctx, 5u * S03_GROUP_SECTORS, S03_GROUP_SECTORS) == 0);
	CHECK(erase_calls == S03_RETRIES && n_ops == 1u);
	reset_log();
	erase_fail_left = 100u;
	CHECK(bd.erase(bd.ctx, 5u * S03_GROUP_SECTORS, S03_GROUP_SECTORS) != 0);
	CHECK(erase_calls == S03_RETRIES && n_ops == 0u);
	erase_fail_left = 0;

	/* yaffs2's erase of a block is that erase, and no write */
	reset_log();
	memset(vol + (size_t)7u * S03_GROUP_SECTORS * S03_SECTOR, 0x5A,
	       (size_t)S03_GROUP_SECTORS * S03_SECTOR);
	CHECK(Y.dev.drv.drv_erase_fn(&Y.dev, 7) == YAFFS_OK);
	CHECK(n_ops == 1u && ops[0].kind == OP_ERASE &&
	      ops[0].sector == 7u * S03_GROUP_SECTORS);
	CHECK(all_zero(vol + (size_t)7u * S03_GROUP_SECTORS * S03_SECTOR,
		       (size_t)S03_GROUP_SECTORS * S03_SECTOR));
	/* one whose erase fails after its retries fails */
	reset_log();
	erase_fail_left = 100u;
	CHECK(Y.dev.drv.drv_erase_fn(&Y.dev, 7) == YAFFS_FAIL);
	CHECK(erase_calls == S03_RETRIES && n_ops == 0u);
	erase_fail_left = 0;
}

/* ---- the reliable write ---- */

static void test_reliable_op(void)
{
	static _Alignas(4) uint8_t buf[2u * CHUNK_SECTORS * S03_SECTOR];

	memset(buf, 0x3C, sizeof(buf));
	reset_log();
	CHECK(bd.write_reliable != NULL);
	if (bd.write_reliable == NULL)
		return;
	CHECK(bd.write_reliable(bd.ctx, 9u * CHUNK_SECTORS, buf, CHUNK_SECTORS) == 0);
	CHECK(bd.write_reliable(bd.ctx, 16u * CHUNK_SECTORS, buf,
				2u * CHUNK_SECTORS) == 0);
	CHECK(n_ops == 2u && ops[0].kind == OP_RELIABLE &&
	      ops[0].sector == 9u * CHUNK_SECTORS && ops[0].count == CHUNK_SECTORS &&
	      ops[1].kind == OP_RELIABLE && ops[1].count == 2u * CHUNK_SECTORS);
	CHECK(!memcmp(vol + (size_t)9u * CHUNK_SECTORS * S03_SECTOR, buf,
		      (size_t)CHUNK_SECTORS * S03_SECTOR));

	/* whole chunks at a chunk's first sector, on the user area only */
	reset_log();
	CHECK(bd.write_reliable(bd.ctx, 9u * CHUNK_SECTORS + 1u, buf,
				CHUNK_SECTORS) != 0);
	CHECK(bd.write_reliable(bd.ctx, 9u * CHUNK_SECTORS, buf, 4u) != 0);
	CHECK(bd.write_reliable(bd.ctx, 9u * CHUNK_SECTORS, buf, 0u) != 0);
	s.st.part = S03_PART_BP2;
	CHECK(bd.write_reliable(bd.ctx, 9u * CHUNK_SECTORS, buf, CHUNK_SECTORS) != 0);
	s.st.part = S03_PART_USER;
	s.st.emmc_more = NULL;		/* the eMMC down */
	CHECK(bd.write_reliable(bd.ctx, 9u * CHUNK_SECTORS, buf, CHUNK_SECTORS) != 0);
	s.st.emmc_more = &em_more;
	CHECK(n_ops == 0u);

	/* yaffs2's chunk write is that write */
	reset_log();
	CHECK(Y.dev.drv.drv_write_chunk_fn(&Y.dev, 300, buf,
					   (int)(CHUNK_SECTORS * S03_SECTOR),
					   NULL, 0) == YAFFS_OK);
	CHECK(n_ops == 1u && ops[0].kind == OP_RELIABLE &&
	      ops[0].sector == 300u * CHUNK_SECTORS && ops[0].count == CHUNK_SECTORS);
}

/* ---- SC-12-YAFFS-EMMC ---- */

static uint8_t file_data[300u * 1024u];

static bool put(unsigned n, size_t len)
{
	char path[32];
	size_t i;
	int fd;
	bool ok;

	for (i = 0; i < len; i++)
		file_data[i] = (uint8_t)(i * 7u + n * 13u + 1u);
	snprintf(path, sizeof(path), "/yaffs2/f%u", n);
	fd = yaffs_open(path, O_CREAT | O_TRUNC | O_WRONLY, S_IREAD | S_IWRITE);
	if (fd < 0)
		return false;
	ok = yaffs_write(fd, file_data, (unsigned int)len) == (int)len;
	ok = yaffs_close(fd) == 0 && ok;
	return ok;
}

static bool drop(unsigned n)
{
	char path[32];

	snprintf(path, sizeof(path), "/yaffs2/f%u", n);
	return yaffs_unlink(path) == 0;
}

/* The block's chunk 0 is unused: its sequence number is 0 (S-12.043). */
static bool block_empty(uint32_t b)
{
	const uint8_t *t = vol + ((size_t)b * S03_GROUP_SECTORS) * S03_SECTOR + 0xFF0u;

	return (t[0] | t[1] | t[2] | t[3]) == 0u;
}

static void test_reclaim(void)
{
	const size_t vol_bytes = (size_t)SECTORS * S03_SECTOR;
	bool held[BLOCKS] = { false };
	enum op_kind last[BLOCKS] = { OP_WRITE };
	bool touched[BLOCKS] = { false };
	unsigned reclaimed = 0, n, oldest = 0;
	size_t written = 0, i;
	uint32_t b;

	reset_log();
	CHECK(wire_yaffs_mount(&Y) == WIRE_MOUNT_OK);
	for (n = 0; written < 2u * vol_bytes && n < 1000u; n++) {
		const size_t len = 120u * 1024u + (size_t)(n % 5u) * 37u * 1024u;

		CHECK(put(n, len));
		written += len;
		/* at most four files alive */
		if (n - oldest >= 3u)
			CHECK(drop(oldest++));
	}
	CHECK(written >= 2u * vol_bytes);
	while (oldest < n)
		CHECK(drop(oldest++));
	CHECK(put(n, 5000u));
	wire_yaffs_unmount(&Y);

	/* Each block's history: an erase empties it, a write fills it. */
	for (i = 0; i < n_ops; i++) {
		const struct op *o = &ops[i];

		CHECK(o->sector / S03_GROUP_SECTORS ==
		      (o->sector + o->count - 1u) / S03_GROUP_SECTORS);
		b = o->sector / S03_GROUP_SECTORS;
		if (b >= BLOCKS)
			continue;
		if (o->kind == OP_ERASE) {
			CHECK(o->sector == b * S03_GROUP_SECTORS);
			if (held[b])
				reclaimed++;
			held[b] = false;
		} else {
			/* S-12.060: reliable, whole chunks, none plain */
			CHECK(o->kind == OP_RELIABLE);
			CHECK(o->count > 0u && o->sector % CHUNK_SECTORS == 0u &&
			      o->count % CHUNK_SECTORS == 0u);
			held[b] = true;
		}
		touched[b] = true;
		last[b] = o->kind;
	}
	CHECK(reclaimed > 0u);
	for (b = 0; b < BLOCKS; b++)
		if (touched[b] && block_empty(b))
			CHECK(last[b] == OP_ERASE);
	/* no erase by writing (SC-12-YAFFS-EMMC's third condition) */
	CHECK(zero_chunk_writes == 0u);
}

int main(void)
{
	vol = calloc(SECTORS, S03_SECTOR);
	if (vol == NULL)
		return 2;
	s.st.emmc_ops = em_ops;
	s.st.emmc_more = &em_more;
	s.st.emmc_up = true;
	s.st.part = S03_PART_USER;
	opxy_slave_user_bd(&s, &bd);
	CHECK(bd.ctx == &s.st && bd.n_sectors == S03_USER_SECTORS);
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &bd, 0u, BLOCKS - 1u));
	RUN(test_erase_op);
	RUN(test_reliable_op);
	memset(vol, 0, (size_t)SECTORS * S03_SECTOR);
	RUN(test_reclaim);
	free(ops);
	free(vol);
	return check_summary("test_board_yaffs_emmc");
}

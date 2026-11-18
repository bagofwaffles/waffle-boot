/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The mount that restores the OS's checkpoint (S-12.061) and the scan
 * before a session's first write (S-12.062), through src/wire_yaffs.c and
 * src/wire_s09.c, on volumes whose checkpoint upstream yaffs2 writes here
 * with checkpoint writing on (test code only; the bootloader never writes
 * one, S-12.055):
 * - a valid checkpoint: restored, read only, the scanned tree, nothing
 *   written, far fewer reads;
 * - a stale one (a write into the allocating block after it; a newer
 *   block), a half-written one, one without its first chunk, none, and
 *   reads that fail in the stale check: each scanned, the scanned tree;
 * - the scan before the first write: once, then writes reach the volume,
 *   no chunk with the checkpoint's sequence number is written and no
 *   checkpoint block is erased before it; no later mount of the session
 *   restores;
 * - the bridge's 0x08 (S-09.041): its way and objects, arg0 1, and a
 *   write that finds the mount restored.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "waffle/s09_bridge.h"
#include "waffle/wire_s09.h"
#include "waffle/wire_yaffs.h"
#include "wb_yaffs.h"

#define BLOCKS 64u		/* at least yaffs2's 60 for a checkpoint */
#define BLOCK_BYTES (WB_YAFFS_SECTORS_PER_BLOCK * WB_SECTOR_SIZE)
#define IMAGE_BYTES ((size_t)BLOCKS * BLOCK_BYTES)
#define CHUNK 4096u
#define TAGS 0xFF0u
#define SEQ_CKPT 0x21u
#define MANY 150u
/* the root, lost+found, a, a/b, one, two, three, the link, many and its files */
#define OBJECTS (9u + MANY)
#define MAXPATHS 400u

static struct wire_yaffs Y;
static struct wb_blockdev bd, rec;
static struct wb_ramdisk ram;
static uint8_t *image;
static uint64_t clock_us;

/* ---- the recording device: every write and erase seen, reads failed on
 * request ---- */

static struct {
	uint32_t writes, erases, ckpt_chunks, ckpt_erases;
	uint32_t fail_lba, fail_left;	/* a sector whose next reads fail */
} R;

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static const uint8_t *chunk_of(const uint8_t *img, uint32_t block, uint32_t c)
{
	return img + (size_t)block * BLOCK_BYTES + (size_t)c * CHUNK;
}

static bool is_ckpt_block(const uint8_t *img, uint32_t b)
{
	return le32(chunk_of(img, b, 0) + TAGS) == SEQ_CKPT;
}

static unsigned ckpt_blocks(const uint8_t *img)
{
	unsigned n = 0;

	for (uint32_t b = 0; b < BLOCKS; b++)
		n += is_ckpt_block(img, b) ? 1u : 0u;
	return n;
}

static int r_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	(void)ctx;
	if (R.fail_left != 0u && R.fail_lba >= lba && R.fail_lba < lba + count) {
		R.fail_left--;
		return -1;
	}
	return bd.read(bd.ctx, lba, buf, count);
}

/* Chunk writes: their tags' sequence number noted. */
static void wrote(uint32_t lba, const void *buf, uint32_t count)
{
	const uint8_t *p = buf;

	R.writes++;
	for (uint32_t s = 0; s < count; s++) {
		if ((lba + s) % WB_YAFFS_SECTORS_PER_CHUNK == WB_YAFFS_SECTORS_PER_CHUNK - 1u &&
		    le32(p + s * WB_SECTOR_SIZE + (TAGS - 7u * WB_SECTOR_SIZE)) == SEQ_CKPT)
			R.ckpt_chunks++;
	}
}

static int r_write(void *ctx, uint32_t lba, const void *buf, uint32_t count)
{
	(void)ctx;
	wrote(lba, buf, count);
	return bd.write(bd.ctx, lba, buf, count);
}

static int r_erase(void *ctx, uint32_t lba, uint32_t count)
{
	(void)ctx;
	R.erases++;
	if (is_ckpt_block(image, lba / WB_YAFFS_SECTORS_PER_BLOCK))
		R.ckpt_erases++;
	return bd.erase(bd.ctx, lba, count);
}

static uint64_t now(void)
{
	return clock_us += 10u;
}

/* ---- writing volumes with upstream yaffs2 ---- */

static void attach(bool checkpoint_writes)
{
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &rec, 0, BLOCKS - 1u), "add");
	Y.dev.param.skip_checkpt_wr = checkpoint_writes ? 0 : 1;
	Y.now_us = now;
	CHECK(wire_yaffs_mount(&Y) == WIRE_MOUNT_OK, "mount");
}

static void put_file(const char *path, size_t len, uint8_t seed)
{
	static uint8_t data[1u << 20];
	int fd = yaffs_open(path, O_CREAT | O_RDWR | O_TRUNC, S_IREAD | S_IWRITE);

	for (size_t i = 0; i < len && i < sizeof data; i++)
		data[i] = (uint8_t)(seed + i * 7u);
	CHECK(len <= sizeof data && fd >= 0 && yaffs_write(fd, data, (unsigned)len) == (int)len &&
	      yaffs_close(fd) == 0, "%s written", path);
}

static void first_files(void)
{
	CHECK(yaffs_mkdir("/yaffs2/a", S_IREAD | S_IWRITE | S_IEXEC) == 0 &&
	      yaffs_mkdir("/yaffs2/a/b", S_IREAD | S_IWRITE | S_IEXEC) == 0, "dirs");
	put_file("/yaffs2/one", 5000, 1);
	put_file("/yaffs2/a/two", 70000, 2);
	put_file("/yaffs2/a/b/three", 100, 3);
	CHECK(yaffs_symlink("one", "/yaffs2/a/link") == 0, "symlink");
	CHECK(yaffs_mkdir("/yaffs2/many", S_IREAD | S_IWRITE | S_IEXEC) == 0, "many");
	for (unsigned i = 0; i < MANY; i++) {
		char p[64];

		snprintf(p, sizeof p, "/yaffs2/many/f%u", i);
		put_file(p, 10u + i, (uint8_t)i);
	}
}

/* A volume of the first files, its checkpoint written at the unmount. */
static void checkpointed(void)
{
	memset(image, 0, IMAGE_BYTES);
	attach(true);
	first_files();
	wire_yaffs_detach(&Y);
	CHECK(ckpt_blocks(image) != 0u, "a checkpoint written");
}

/* The checkpointed volume written further after its checkpoint, the
 * checkpoint's blocks put where the later writes left blocks empty
 * (upstream yaffs2 is deterministic: its blocks are those of the
 * checkpointed volume). big: past the allocating block's room. */
static void stale(bool big)
{
	uint8_t *saved = tl_xmalloc(IMAGE_BYTES);
	uint32_t free_b = BLOCKS;

	checkpointed();
	memcpy(saved, image, IMAGE_BYTES);
	memset(image, 0, IMAGE_BYTES);
	attach(false);
	first_files();
	put_file("/yaffs2/later", big ? 900000u : 2000u, 9);
	wire_yaffs_detach(&Y);
	for (uint32_t b = 0; b < BLOCKS; b++) {
		uint32_t to = free_b;

		if (!is_ckpt_block(saved, b))
			continue;
		while (to-- > 0u && le32(chunk_of(image, to, 0) + TAGS) != 0u)
			;
		CHECK(to < BLOCKS, "an empty block for the checkpoint's");
		if (to >= BLOCKS)
			break;
		memcpy(image + (size_t)to * BLOCK_BYTES, saved + (size_t)b * BLOCK_BYTES,
		       BLOCK_BYTES);
		free_b = to;
	}
	free(saved);
}

/* ---- trees ---- */

struct tree {
	char line[MAXPATHS][700];
	unsigned n;
};

static int by_line(const void *a, const void *b)
{
	return strcmp(a, b);
}

static void walk_dir(struct tree *t, const char *dir)
{
	yaffs_DIR *d = yaffs_opendir(dir);
	const struct yaffs_dirent *de;
	char path[600];
	struct yaffs_stat st;

	if (d == NULL)
		return;
	while ((de = yaffs_readdir(d)) != NULL) {
		snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
		if (yaffs_lstat(path, &st) != 0 || t->n >= MAXPATHS)
			continue;
		snprintf(t->line[t->n++], sizeof t->line[0], "%s %u %o %lld", path,
			 (unsigned)st.st_ino, (unsigned)st.st_mode, (long long)st.st_size);
		if ((st.st_mode & S_IFMT) == S_IFDIR)
			walk_dir(t, path);
	}
	yaffs_closedir(d);
}

/* The mounted volume's tree, sorted; the root counted. */
static void tree_of(struct tree *t)
{
	t->n = 0;
	snprintf(t->line[t->n++], sizeof t->line[0], "/yaffs2");
	walk_dir(t, "/yaffs2");
	qsort(t->line, t->n, sizeof t->line[0], by_line);
}

static bool same(const struct tree *a, const struct tree *b)
{
	if (a->n != b->n)
		return false;
	for (unsigned i = 0; i < a->n; i++)
		if (strcmp(a->line[i], b->line[i]) != 0)
			return false;
	return true;
}

static void tree_free(struct tree *t)
{
	t->n = 0;
}

/* The volume's tree by the scan alone, as before S-12.061. */
static void scanned_tree(struct tree *t)
{
	attach(false);
	CHECK(Y.rep.way == WIRE_WAY_NONE, "restoring off: the scan");
	tree_of(t);
	wire_yaffs_detach(&Y);
}

/* The bootloader's mount: restoring on, checkpoint writing off. */
static void boot_mount(void)
{
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &rec, 0, BLOCKS - 1u), "add");
	Y.may_restore = true;
	Y.now_us = now;
	memset(&R, 0, sizeof R);
	CHECK(wire_yaffs_mount(&Y) == WIRE_MOUNT_OK, "the boot's mount");
}

/* A mount that changes nothing: the image as it was, nothing tried. */
static void mount_unchanged(enum wire_yaffs_way want, const char *name)
{
	uint8_t *before = tl_xmalloc(IMAGE_BYTES);
	static struct tree scan, got;	/* large: not on the stack */

	memcpy(before, image, IMAGE_BYTES);
	scanned_tree(&scan);
	boot_mount();
	CHECK(Y.rep.way == want, "%s: way %d, want %d", name, (int)Y.rep.way, (int)want);
	CHECK(Y.restored == (want == WIRE_WAY_RESTORED), "%s: restored as the way says", name);
	tree_of(&got);
	CHECK(same(&scan, &got), "%s: the scanned tree (%u and %u entries)", name, scan.n, got.n);
	CHECK(Y.rep.objects == got.n, "%s: %u objects, the tree's %u", name, Y.rep.objects, got.n);
	wire_yaffs_detach(&Y);
	CHECK(R.writes == 0u && R.erases == 0u && Y.rep.writes_refused == 0u,
	      "%s: nothing written or tried", name);
	CHECK(!memcmp(before, image, IMAGE_BYTES), "%s: the image unchanged", name);
	tree_free(&scan);
	tree_free(&got);
	free(before);
}

/* ---- the cases ---- */

static void test_valid(void)
{
	uint32_t restored_reads, scan_reads;
	int fd;

	checkpointed();
	mount_unchanged(WIRE_WAY_RESTORED, "valid");

	boot_mount();
	CHECK(Y.rep.objects == OBJECTS, "S-12.061 the objects: %u", Y.rep.objects);
	CHECK(Y.rep.stale.blocks == BLOCKS && Y.rep.stale.ckpt_blocks == ckpt_blocks(image) &&
	      Y.rep.stale.newer == 0u && Y.rep.stale.disagree == 0u &&
	      Y.rep.stale.alloc_used == 0u && Y.rep.stale.failed == 0u,
	      "S-12.061 the stale check: every block agrees");
	CHECK(Y.rep.stale.alloc_block >= 0, "S-12.061 the allocating block is checked");
	restored_reads = Y.rep.restore_reads + Y.rep.check_reads;
	CHECK(Y.rep.restore_us != 0u && Y.rep.check_us != 0u, "times taken");
	/* read only: a write is refused before the eMMC, and the guard
	 * refuses any that would reach it */
	fd = yaffs_open("/yaffs2/new", O_CREAT | O_RDWR, S_IREAD | S_IWRITE);
	CHECK(fd < 0 && yaffs_get_error() == -EROFS, "S-12.062 the restored mount is read only");
	CHECK(yaffs_mkdir("/yaffs2/d", 0755) != 0, "S-12.062 no mkdir");
	CHECK(Y.guard.write(Y.guard.ctx, 0, image, 1) == -1 &&
	      Y.guard.erase(Y.guard.ctx, 0, WB_YAFFS_SECTORS_PER_BLOCK) == -1 &&
	      Y.guard.write_reliable(Y.guard.ctx, 0, image, 8) == -1 &&
	      Y.rep.writes_refused == 3u && R.writes == 0u && R.erases == 0u,
	      "S-12.062 the guard refuses and counts");
	wire_yaffs_detach(&Y);

	/* the restore's reads: the checkpoint's chunks and a tags read per
	 * block, with the allocating block's pages; on this small volume the
	 * scan is about as cheap */
	attach(false);
	scan_reads = Y.rep.scan_reads;
	wire_yaffs_detach(&Y);
	CHECK(restored_reads <= 2u * BLOCKS + 2u * WB_YAFFS_CHUNKS_PER_BLOCK,
	      "S-12.061 restored in %u reads (the scan %u)", restored_reads, scan_reads);
}

static void test_stale(void)
{
	stale(false);
	mount_unchanged(WIRE_WAY_CKPT_STALE, "a write into the allocating block");
	boot_mount();
	CHECK(Y.rep.stale.alloc_used != 0u, "S-12.061 the allocating block's later pages seen");
	CHECK(Y.rep.scan_reads != 0u && Y.rep.check_reads != 0u, "the check, then the scan");
	wire_yaffs_detach(&Y);

	stale(true);
	mount_unchanged(WIRE_WAY_CKPT_STALE, "a newer block");
	boot_mount();
	CHECK(Y.rep.stale.newer != 0u && Y.rep.stale.disagree != 0u,
	      "S-12.061 a newer block, recorded empty");
	wire_yaffs_detach(&Y);
}

/* A data block erased after the checkpoint: it disagrees. */
static void test_erased_block(void)
{
	uint32_t victim = BLOCKS;

	checkpointed();
	for (uint32_t b = 0; b < BLOCKS && victim == BLOCKS; b++) {
		const uint32_t seq = le32(chunk_of(image, b, 0) + TAGS);

		if (seq >= 0x1000u && seq != SEQ_CKPT)
			victim = b;
	}
	CHECK(victim < BLOCKS, "a data block");
	if (victim < BLOCKS)
		memset(image + (size_t)victim * BLOCK_BYTES, 0, BLOCK_BYTES);
	mount_unchanged(WIRE_WAY_CKPT_STALE, "an erased data block");
}

static void test_invalid(void)
{
	uint32_t last_b = 0, last_c = 0, last_id = 0;

	/* the last chunk never written: yaffs2 takes no checkpoint */
	checkpointed();
	for (uint32_t b = 0; b < BLOCKS; b++) {
		if (!is_ckpt_block(image, b))
			continue;
		for (uint32_t c = 0; c < 128u; c++) {
			const uint8_t *t = chunk_of(image, b, c) + TAGS;

			if (le32(t) == SEQ_CKPT && le32(t + 8) > last_id) {
				last_id = le32(t + 8);
				last_b = b;
				last_c = c;
			}
		}
	}
	CHECK(last_id > 1u, "a checkpoint of more than one chunk");
	memset(image + (size_t)last_b * BLOCK_BYTES + (size_t)last_c * CHUNK, 0, CHUNK);
	mount_unchanged(WIRE_WAY_CKPT_INVALID, "half written");

	/* its first chunk gone */
	checkpointed();
	for (uint32_t b = 0; b < BLOCKS; b++)
		if (is_ckpt_block(image, b) && le32(chunk_of(image, b, 0) + TAGS + 8) == 1u)
			memset(image + (size_t)b * BLOCK_BYTES, 0, CHUNK);
	mount_unchanged(ckpt_blocks(image) != 0u ? WIRE_WAY_CKPT_INVALID : WIRE_WAY_NO_CKPT,
			"no first chunk");

	/* none */
	memset(image, 0, IMAGE_BYTES);
	attach(false);
	first_files();
	wire_yaffs_detach(&Y);
	CHECK(ckpt_blocks(image) == 0u, "no checkpoint written");
	mount_unchanged(WIRE_WAY_NO_CKPT, "none");
}

/* A read that fails in the stale check (the allocating block's next page,
 * which nothing else reads first): the check cannot complete. */
static void test_unreadable(void)
{
	uint32_t lba;

	checkpointed();
	boot_mount();
	CHECK(Y.rep.way == WIRE_WAY_RESTORED && Y.rep.stale.alloc_block >= 0, "restored");
	lba = ((uint32_t)Y.rep.stale.alloc_block * WB_YAFFS_CHUNKS_PER_BLOCK +
	       Y.rep.stale.alloc_page) * WB_YAFFS_SECTORS_PER_CHUNK + 7u;
	wire_yaffs_detach(&Y);
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &rec, 0, BLOCKS - 1u), "add");
	Y.may_restore = true;
	memset(&R, 0, sizeof R);
	R.fail_lba = lba;
	R.fail_left = 2u;		/* the tags alone, then the whole chunk */
	CHECK(wire_yaffs_mount(&Y) == WIRE_MOUNT_OK, "mounted");
	CHECK(Y.rep.way == WIRE_WAY_CKPT_UNREADABLE && Y.rep.stale.failed != 0u && !Y.restored,
	      "S-12.061 a failed read in the check: scanned (way %d)", (int)Y.rep.way);
	CHECK(Y.rep.objects == OBJECTS && R.writes == 0u && R.erases == 0u, "the tree, nothing written");
	wire_yaffs_detach(&Y);
}

/*
 * S-12.062: the scan before the first write. Then the write reaches the
 * volume; the checkpoint's blocks are erased only after the scan, and no
 * chunk written carries the checkpoint's sequence number, the unmount's
 * included. Once per session: a second call, and a later mount, scan or
 * restore nothing.
 */
static void test_for_write(void)
{
	const unsigned before = (checkpointed(), ckpt_blocks(image));
	uint32_t reads;

	boot_mount();
	CHECK(Y.restored, "restored");
	CHECK(wire_yaffs_for_write(&Y, false) && !Y.restored && Y.mounted &&
	      Y.rep.way == WIRE_WAY_FOR_WRITE && Y.rep.write_scan_reads != 0u,
	      "S-12.062 scanned for the write");
	CHECK(R.writes == 0u && R.erases == 0u && ckpt_blocks(image) == before,
	      "S-12.062 the scan itself writes nothing");
	CHECK(Y.rep.objects == OBJECTS, "the same tree");
	reads = Y.reads;
	CHECK(wire_yaffs_for_write(&Y, false) && Y.reads == reads, "S-12.062 once per session");
	put_file("/yaffs2/new", 3000, 5);
	CHECK(R.writes != 0u && R.ckpt_erases == before && ckpt_blocks(image) == 0u,
	      "S-12.055 the checkpoint erased before the write's chunks");
	wire_yaffs_unmount(&Y);
	CHECK(R.ckpt_chunks == 0u, "S-12.055 no checkpoint chunk written, the unmount's included");
	CHECK(wire_yaffs_mount(&Y) == WIRE_MOUNT_OK && Y.rep.way == WIRE_WAY_FOR_WRITE &&
	      Y.rep.restore_reads == 0u && !Y.restored,
	      "S-12.062 a later mount of the session scans");
	wire_yaffs_detach(&Y);

	/* a scanned mount has nothing to do, and no later mount restores */
	memset(image, 0, IMAGE_BYTES);
	attach(false);
	first_files();
	wire_yaffs_detach(&Y);
	boot_mount();
	reads = Y.reads;
	CHECK(Y.rep.way == WIRE_WAY_NO_CKPT && wire_yaffs_for_write(&Y, false) &&
	      Y.reads == reads && Y.write_scanned, "S-12.062 a scanned mount: no second scan");
	wire_yaffs_detach(&Y);

	/* files open on the restored mount: it stays, read only */
	checkpointed();
	boot_mount();
	{
		const int fd = yaffs_open("/yaffs2/one", O_RDONLY, 0);

		CHECK(fd >= 0 && !wire_yaffs_for_write(&Y, false) && Y.restored && Y.mounted,
		      "S-12.062 busy: stays restored");
		CHECK(yaffs_close(fd) == 0 && wire_yaffs_for_write(&Y, false) && !Y.restored,
		      "then scanned");
	}
	wire_yaffs_detach(&Y);
	CHECK(R.ckpt_chunks == 0u && R.writes == 0u, "nothing written");
}

/* ---- the bridge (S-09.041) ---- */

static struct wire_s09 S;

static int32_t cmd(uint8_t c, uint32_t arg0, const char *path, uint32_t *err)
{
	static uint8_t out[512];
	const struct s09_req r = { c, 0, 0, arg0, 0 };

	return wire_s09_file(&S, &r, path, NULL, NULL, 0, out, err);
}

static void test_bridge(void)
{
	uint32_t err = 99;
	int32_t v;

	checkpointed();
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &rec, 0, BLOCKS - 1u), "add");
	Y.may_restore = true;
	wire_s09_init(&S, &Y);
	memset(&R, 0, sizeof R);
	v = cmd(S09_MOUNT, 0, NULL, &err);
	CHECK(v > 0 && err == 0u && S09_MOUNT_WAY(v) == WIRE_WAY_RESTORED &&
	      S09_MOUNT_OBJECTS(v) == OBJECTS, "S-09.041 0x08: restored, %u objects",
	      S09_MOUNT_OBJECTS(v));
	CHECK(cmd(S09_MOUNT, 0, NULL, &err) == v, "S-09.041 again: the same");
	/* a write finds the mount restored: the scan first, then the write */
	CHECK(cmd(S09_MKDIR, 0755, "/yaffs2/made", &err) == 0 && err == 0u && !Y.restored &&
	      S.implicit_scans == 1u, "S-12.062 a write on the restored mount: scanned first");
	v = cmd(S09_MOUNT, S09_MOUNT_FOR_WRITE, NULL, &err);
	CHECK(S09_MOUNT_WAY(v) == WIRE_WAY_FOR_WRITE && S.implicit_scans == 1u,
	      "S-09.041 arg0 1 on a scanned mount: nothing more");
	CHECK(cmd(S09_UNMOUNT, 0, NULL, &err) == 1, "unmounted");
	CHECK(R.ckpt_chunks == 0u, "S-12.055 no checkpoint written");
	wire_yaffs_detach(&Y);

	/* arg0 1 on a restored mount; with a directory open it stays */
	checkpointed();
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &rec, 0, BLOCKS - 1u), "add");
	Y.may_restore = true;
	wire_s09_init(&S, &Y);
	memset(&R, 0, sizeof R);
	CHECK(S09_MOUNT_WAY(cmd(S09_MOUNT, 0, NULL, &err)) == WIRE_WAY_RESTORED, "restored");
	CHECK(cmd(S09_OPENDIR, 0, "/yaffs2", &err) != 0, "a directory open");
	CHECK(S09_MOUNT_WAY(cmd(S09_MOUNT, S09_MOUNT_FOR_WRITE, NULL, &err)) == WIRE_WAY_RESTORED,
	      "S-09.041 arg0 1 with a directory open: restored still");
	CHECK(cmd(S09_MKDIR, 0755, "/yaffs2/x", &err) == -1 && err == S09_EROFS,
	      "S-12.062 the write then fails with EROFS");
	wire_s09_release(&S);
	v = cmd(S09_MOUNT, S09_MOUNT_FOR_WRITE, NULL, &err);
	CHECK(S09_MOUNT_WAY(v) == WIRE_WAY_FOR_WRITE && S09_MOUNT_OBJECTS(v) == OBJECTS,
	      "S-09.041 arg0 1: scanned for the write");
	CHECK(cmd(S09_MKDIR, 0755, "/yaffs2/x", &err) == 0, "then written");
	(void)cmd(S09_UNMOUNT, 0, NULL, &err);
	CHECK(R.ckpt_chunks == 0u, "S-12.055 no checkpoint written");
	wire_yaffs_detach(&Y);

	/* restoring off (as the test OS): 0, as before */
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &rec, 0, BLOCKS - 1u), "add");
	wire_s09_init(&S, &Y);
	CHECK(cmd(S09_MOUNT, 0, NULL, &err) == 0 && err == 0u, "S-09.041 no checkpoint read: 0");
	wire_yaffs_detach(&Y);
}

int main(void)
{
	image = tl_xmalloc(IMAGE_BYTES);
	wb_ramdisk_init(&bd, &ram, image, (uint32_t)(IMAGE_BYTES / WB_SECTOR_SIZE));
	rec = bd;
	rec.ctx = NULL;
	rec.read = r_read;
	rec.write = r_write;
	rec.erase = bd.erase != NULL ? r_erase : NULL;
	rec.write_reliable = NULL;
	test_valid();
	test_stale();
	test_erased_block();
	test_invalid();
	test_unreadable();
	test_for_write();
	test_bridge();
	free(image);
	return tl_done("test_wire_mount");
}

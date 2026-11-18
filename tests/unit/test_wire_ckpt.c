/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The read-only checkpoint test (src/wire_ckpt.c) on volumes whose
 * checkpoint upstream yaffs2 writes here, with checkpoint writing on (test
 * code only; the bootloader never writes one, S-12.055): a valid
 * checkpoint, one left stale by later writes, one half written, and a
 * volume without one. Each run writes nothing (the image is the same after
 * it, and the refusing device counts no attempt), and a write given to that
 * device is refused and counted.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "waffle/wire_ckpt.h"
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

static struct wire_yaffs Y;
static struct wb_blockdev bd;
static struct wb_ramdisk ram;
static uint8_t *image;
static uint64_t clock_us;

static uint64_t now(void)
{
	return clock_us += 10u;
}

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

/* ---- writing volumes with upstream yaffs2 ---- */

static void attach(bool checkpoint_writes)
{
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &bd, 0, BLOCKS - 1u), "add");
	Y.dev.param.skip_checkpt_wr = checkpoint_writes ? 0 : 1;
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
	/* enough objects for a checkpoint of several chunks */
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

/* ---- the scanned tree, through the bootloader's own (scanning) mount ---- */

static bool sc_full(const char *rel, char *out)
{
	return snprintf(out, 600, "/yaffs2%s%s", *rel ? "/" : "", rel) < 600;
}

static bool sc_list(void *ctx, const char *dir, bool (*each)(void *ectx, const char *name),
		    void *ectx)
{
	char path[600];
	yaffs_DIR *d;
	struct yaffs_dirent *de;

	(void)ctx;
	if (!sc_full(dir, path) || (d = yaffs_opendir(path)) == NULL)
		return false;
	while ((de = yaffs_readdir(d)) != NULL && each(ectx, de->d_name))
		;
	yaffs_closedir(d);
	return true;
}

static bool sc_stat(void *ctx, const char *rel, struct wire_ckpt_obj *o)
{
	char path[600];
	struct yaffs_stat st;

	(void)ctx;
	if (!sc_full(rel, path) || yaffs_lstat(path, &st) != 0)
		return false;
	o->ino = (uint32_t)st.st_ino;
	o->type = (st.st_mode & S_IFMT) == S_IFDIR ? 4u : (st.st_mode & S_IFMT) == S_IFREG ? 8u :
		  (st.st_mode & S_IFMT) == S_IFLNK ? 10u : 0u;
	o->size = (uint32_t)st.st_size;
	return true;
}

static const struct wire_ckpt_tree_ops sc_ops = { NULL, sc_list, sc_stat };

/* The test against the scanned mount; the image unchanged by it. */
static void run(struct wire_ckpt_report *r)
{
	uint8_t *before = tl_xmalloc(IMAGE_BYTES);

	memcpy(before, image, IMAGE_BYTES);
	attach(false);
	CHECK(wire_ckpt_run(&bd, 0, BLOCKS - 1u, &sc_ops, now, r) == WIRE_CKPT_DONE,
	      "the test ran");
	wire_yaffs_detach(&Y);
	CHECK(r->writes_refused == 0u && r->unmounted, "no write tried, unmounted");
	CHECK(!memcmp(before, image, IMAGE_BYTES), "nothing written");
	free(before);
}

static void show(const char *name, const struct wire_ckpt_report *r)
{
	printf("    %s: %u checkpoint blocks, %s (%s); %u reads; trees %u/%u, %u diffs%s%s; "
	       "seq %x, newer %u, alloc %d:%u %s, disagree %u%s%s\n", name, r->ckpt_blocks,
	       r->accepted ? "accepted" : "rejected", r->why, r->mount_reads, r->objs_scan,
	       r->objs_ckpt, r->diffs, r->diffs ? ", first " : "", r->diffs ? r->diff[0] : "",
	       r->ckpt_seq, r->newer, r->alloc_block, r->alloc_page,
	       r->alloc_used ? "used" : "unused", r->disagree, r->disagree ? ", first " : "",
	       r->disagree ? r->disagreement[0] : "");
}

static void test_valid(void)
{
	struct wire_ckpt_report r;

	checkpointed();
	run(&r);
	show("valid", &r);
	CHECK(r.accepted && r.ckpt_blocks == ckpt_blocks(image), "accepted");
	CHECK(r.objs_scan == OBJECTS && r.objs_ckpt == OBJECTS && r.diffs == 0u && !r.scan_cut &&
	      !r.ckpt_cut, "the trees agree");
	CHECK(r.newer == 0u && !r.alloc_used && r.disagree == 0u, "not stale");
	CHECK(r.mount_reads != 0u, "the restore's reads counted");
	/* a write given to the refusing device is refused and counted */
	CHECK(wire_ckpt_refused_write(NULL, 0, image, 1) == -1 && r.writes_refused == 1u,
	      "a write refused and counted");
}

/*
 * A checkpoint left behind: the same volume written further, in the same
 * session as its first files (upstream yaffs2 is deterministic, so its
 * blocks are those of the checkpointed volume), with the checkpoint's block
 * put in a block the later writes left empty (yaffs2 finds checkpoint
 * blocks by their tags). The checkpoint itself is whole, so yaffs2 takes
 * it; the guard sees what was written after it, and the trees differ by it.
 */
static void test_stale(bool big)
{
	struct wire_ckpt_report r;
	uint8_t *saved = tl_xmalloc(IMAGE_BYTES);
	uint32_t free_b = BLOCKS;

	checkpointed();
	memcpy(saved, image, IMAGE_BYTES);
	memset(image, 0, IMAGE_BYTES);
	attach(false);
	first_files();
	/* big: more than the allocating block's room, into new blocks */
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
	run(&r);
	show(big ? "stale, new blocks" : "stale, the same block", &r);
	CHECK(r.accepted, "the stale checkpoint taken by yaffs2");
	CHECK(r.diffs != 0u && r.objs_scan == r.objs_ckpt + 1u, "the later file only scanned");
	CHECK(big ? r.newer != 0u : r.alloc_used, "the guard sees the later writes");
	free(saved);
}

/* A checkpoint whose last chunk never reached the media: the checksum or
 * the closing record fails, yaffs2 scans instead. */
static void test_half(void)
{
	struct wire_ckpt_report r;
	uint32_t last_b = 0, last_c = 0, last_id = 0;

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
	run(&r);
	show("half written", &r);
	CHECK(!r.accepted && r.ckpt_blocks != 0u && strstr(r.why, "later") != NULL,
	      "rejected, past its first chunk");
	CHECK(r.diffs == 0u && r.objs_scan == r.objs_ckpt, "scanned instead: the same tree");
}

/* Its first chunk gone: yaffs2 finds no stream. */
static void test_no_first(void)
{
	struct wire_ckpt_report r;

	checkpointed();
	for (uint32_t b = 0; b < BLOCKS; b++)
		if (is_ckpt_block(image, b) && le32(chunk_of(image, b, 0) + TAGS + 8) == 1u)
			memset(image + (size_t)b * BLOCK_BYTES, 0, CHUNK);
	run(&r);
	show("no first chunk", &r);
	CHECK(!r.accepted, "rejected");
}

static void test_none(void)
{
	struct wire_ckpt_report r;

	memset(image, 0, IMAGE_BYTES);
	attach(false);
	first_files();
	wire_yaffs_detach(&Y);
	CHECK(ckpt_blocks(image) == 0u, "no checkpoint written");
	run(&r);
	show("none", &r);
	CHECK(!r.accepted && r.ckpt_blocks == 0u && strstr(r.why, "no checkpoint") != NULL,
	      "no checkpoint found");
	CHECK(r.diffs == 0u && r.objs_ckpt == OBJECTS, "scanned: the same tree");
}

int main(void)
{
	image = tl_xmalloc(IMAGE_BYTES);
	wb_ramdisk_init(&bd, &ram, image, (uint32_t)(IMAGE_BYTES / WB_SECTOR_SIZE));
	test_valid();
	test_stale(false);
	test_stale(true);
	test_half();
	test_no_first();
	test_none();
	free(image);
	return tl_done("test_wire_ckpt");
}

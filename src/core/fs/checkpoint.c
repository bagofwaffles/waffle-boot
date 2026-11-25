/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The checkpoint test (waffle/fs/checkpoint.h).
 *
 * Nothing reaches the media but reads, in two layers. yaffs2's own: the
 * test device is mounted read only, with checkpoint writing off. Read only,
 * yaffs2 deletes nothing at the mount (the clean-up the port defers to a
 * session's first write is left pending, and yaffs_fix_hanging_objs() and
 * the unlinked and deleted objects' clean-up return at once), needs no
 * checkpoint (yaffs2_checkpt_required()), and invalidates the restored one
 * only at a session's first chunk write or block erase, which yaffsfs
 * refuses on a read-only device with EROFS before it reaches yaffs2's core;
 * collection runs only for a write. The unmount flushes a cache that holds
 * nothing dirty and saves no checkpoint. Under all of it, the device's
 * block device passes reads through and refuses every write, reliable
 * write and erase, counting each: the report says how many were tried.
 */
#include <stdlib.h>
#include <string.h>

#include "waffle/util/bytes.h"
#include "waffle/fs/checkpoint.h"
#include "wb_yaffs.h"
#include "yaffs_getblockinfo.h"

/* yaffsfs.c defines it; yaffsfs.h declares only yaffs_add_device(). */
void yaffs_remove_device(struct yaffs_dev *dev);

#define MOUNT "/ckpt"
#define PATH_MAX_LEN 512u
#define CKPT_HDR 16u		/* struct yaffs_checkpt_chunk_hdr */
#define VALIDITY 16u		/* struct yaffs_checkpt_validity */

/* ---- the refusing block device ---- */

static struct {
	const struct wb_blockdev *under;
	struct wire_ckpt_report *r;
	uint32_t reads, sectors;
} wrap;
static struct wb_blockdev ro_bd;
static struct yaffs_dev ckpt_dev;

static int ro_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	(void)ctx;
	wrap.reads++;
	wrap.sectors += count;
	return wrap.under->read(wrap.under->ctx, lba, buf, count);
}

int wire_ckpt_refused_write(void *ctx, uint32_t lba, const void *buf, uint32_t count)
{
	(void)ctx;
	(void)lba;
	(void)buf;
	(void)count;
	if (wrap.r != NULL)
		wrap.r->writes_refused++;
	return -1;
}

static int ro_erase(void *ctx, uint32_t lba, uint32_t count)
{
	return wire_ckpt_refused_write(ctx, lba, NULL, count);
}

/* ---- the trees ---- */

struct entry {
	char *path;
	struct wire_ckpt_obj o;
};

struct tree {
	struct entry *e;
	uint32_t n, cap;
	bool cut;
	const struct wire_ckpt_tree_ops *ops;
	const char *dir;	/* the directory being listed */
	uint32_t dir_ino;
};

static bool put(struct tree *t, const char *path, const struct wire_ckpt_obj *o)
{
	size_t len = wb_strlen(path);

	if (t->n == t->cap) {
		uint32_t cap = t->cap != 0u ? 2u * t->cap : 256u;
		struct entry *e;

		if (cap > WIRE_CKPT_OBJS) {
			t->cut = true;
			return false;
		}
		e = realloc(t->e, cap * sizeof *e);
		if (e == NULL) {
			t->cut = true;
			return false;
		}
		t->e = e;
		t->cap = cap;
	}
	t->e[t->n].path = malloc(len + 1u);
	if (t->e[t->n].path == NULL) {
		t->cut = true;
		return false;
	}
	memcpy(t->e[t->n].path, path, len + 1u);
	t->e[t->n].o = *o;
	t->n++;
	return true;
}

static bool child(void *ectx, const char *name)
{
	struct tree *t = ectx;
	char path[PATH_MAX_LEN];
	struct wire_ckpt_obj o;
	size_t d = wb_strlen(t->dir), n = wb_strlen(name);

	if (d + 1u + n >= sizeof path) {
		t->cut = true;
		return true;
	}
	memcpy(path, t->dir, d);
	if (d != 0u)
		path[d++] = '/';
	memcpy(path + d, name, n + 1u);
	if (!t->ops->stat(t->ops->ctx, path, &o)) {
		t->cut = true;
		return true;
	}
	o.parent = t->dir_ino;
	return put(t, path, &o);
}

/* Every object reachable from the root, breadth first: each directory's
 * entries listed once, then each in turn. */
static void walk(struct tree *t)
{
	struct wire_ckpt_obj root;
	uint32_t i;

	if (!t->ops->stat(t->ops->ctx, "", &root)) {
		t->cut = true;
		return;
	}
	root.parent = 0;
	if (!put(t, "", &root))
		return;
	for (i = 0; i < t->n; i++) {
		if (t->e[i].o.type != 4u)
			continue;
		t->dir = t->e[i].path;
		t->dir_ino = t->e[i].o.ino;
		if (!t->ops->list(t->ops->ctx, t->e[i].path, child, t))
			t->cut = true;
		/* realloc may have moved the paths' array, not the paths */
	}
}

static void tree_free(struct tree *t)
{
	for (uint32_t i = 0; i < t->n; i++)
		free(t->e[i].path);
	free(t->e);
	memset(t, 0, sizeof *t);
}

static int by_path(const void *a, const void *b)
{
	return strcmp(((const struct entry *)a)->path, ((const struct entry *)b)->path);
}

/* ---- the test device's tree, through yaffsfs ---- */

static bool ck_full(const char *rel, char *out)
{
	size_t m = sizeof MOUNT - 1u, r = wb_strlen(rel);

	if (m + 1u + r >= PATH_MAX_LEN)
		return false;
	memcpy(out, MOUNT, m);
	if (r != 0u) {
		out[m++] = '/';
		memcpy(out + m, rel, r);
	}
	out[m + r] = '\0';
	return true;
}

static bool ck_list(void *ctx, const char *dir, bool (*each)(void *ectx, const char *name),
		    void *ectx)
{
	char path[PATH_MAX_LEN];
	yaffs_DIR *d;
	const struct yaffs_dirent *de;
	bool ok = true;

	(void)ctx;
	if (!ck_full(dir, path) || (d = yaffs_opendir(path)) == NULL)
		return false;
	while (ok && (de = yaffs_readdir(d)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		ok = each(ectx, de->d_name);
	}
	(void)yaffs_closedir(d);
	return true;
}

static bool ck_stat(void *ctx, const char *rel, struct wire_ckpt_obj *o)
{
	char path[PATH_MAX_LEN];
	struct yaffs_stat st;

	(void)ctx;
	if (!ck_full(rel, path) || yaffs_lstat(path, &st) != 0)
		return false;
	o->ino = (uint32_t)st.st_ino;
	switch (st.st_mode & S_IFMT) {
	case S_IFDIR:
		o->type = 4u;
		break;
	case S_IFREG:
		o->type = 8u;
		break;
	case S_IFLNK:
		o->type = 10u;
		break;
	default:
		o->type = 0u;
		break;
	}
	o->size = st.st_size > 0 ? (uint32_t)st.st_size : 0u;
	o->parent = 0;
	return true;
}

static const struct wire_ckpt_tree_ops ck_ops = { NULL, ck_list, ck_stat };

/* ---- the report's text ---- */

struct text {
	char *s;
	size_t n;
};

static void t_str(struct text *t, const char *s)
{
	while (*s != '\0' && t->n + 1u < WIRE_CKPT_TEXT)
		t->s[t->n++] = *s++;
	t->s[t->n] = '\0';
}

static void t_dec(struct text *t, uint32_t v)
{
	char d[11];
	unsigned i = sizeof d - 1u;

	d[i] = '\0';
	do {
		d[--i] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0u && i != 0u);
	t_str(t, d + i);
}

static void t_hex(struct text *t, uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	char d[9];

	for (unsigned i = 0; i < 8u; i++)
		d[i] = hex[v >> (28u - 4u * i) & 0xFu];
	d[8] = '\0';
	t_str(t, d);
}

static struct text t_open(char *s)
{
	struct text t = { s, 0 };

	s[0] = '\0';
	return t;
}

/* ---- the comparison ---- */

static const char *shown(const char *path)
{
	return *path != '\0' ? path : "/";
}

static void differ(struct wire_ckpt_report *r, const char *path, const char *what,
		   uint32_t a, uint32_t b)
{
	if (r->diffs < WIRE_CKPT_KEPT) {
		struct text t = t_open(r->diff[r->diffs]);

		t_str(&t, shown(path));
		t_str(&t, ": ");
		t_str(&t, what);
		if (a != b) {
			t_str(&t, " ");
			t_dec(&t, a);
			t_str(&t, " scanned, ");
			t_dec(&t, b);
			t_str(&t, " restored");
		}
	}
	r->diffs++;
}

static void compare(struct wire_ckpt_report *r, struct tree *s, struct tree *c)
{
	const uint32_t ns = s->e != NULL ? s->n : 0u, nc = c->e != NULL ? c->n : 0u;
	uint32_t i = 0, j = 0;

	if (ns != 0u)
		qsort(s->e, ns, sizeof *s->e, by_path);
	if (nc != 0u)
		qsort(c->e, nc, sizeof *c->e, by_path);
	while (i < ns || j < nc) {
		const int k = i == ns ? 1 : j == nc ? -1 : strcmp(s->e[i].path, c->e[j].path);

		if (k < 0) {
			differ(r, s->e[i++].path, "only in the scanned mount", 0, 0);
		} else if (k > 0) {
			differ(r, c->e[j++].path, "only in the restored mount", 0, 0);
		} else {
			const struct wire_ckpt_obj *a = &s->e[i].o, *b = &c->e[j].o;

			if (a->ino != b->ino)
				differ(r, s->e[i].path, "object id", a->ino, b->ino);
			else if (a->type != b->type)
				differ(r, s->e[i].path, "type", a->type, b->type);
			else if (a->size != b->size)
				differ(r, s->e[i].path, "size", a->size, b->size);
			else if (a->parent != b->parent)
				differ(r, s->e[i].path, "parent id", a->parent, b->parent);
			i++;
			j++;
		}
	}
}

/* ---- the stale guard ---- */

static const char *state_name(enum yaffs_block_state s)
{
	switch (s) {
	case YAFFS_BLOCK_STATE_EMPTY:
		return "empty";
	case YAFFS_BLOCK_STATE_ALLOCATING:
		return "allocating";
	case YAFFS_BLOCK_STATE_FULL:
		return "full";
	case YAFFS_BLOCK_STATE_DIRTY:
		return "dirty";
	case YAFFS_BLOCK_STATE_CHECKPOINT:
		return "checkpoint";
	case YAFFS_BLOCK_STATE_COLLECTING:
		return "collecting";
	case YAFFS_BLOCK_STATE_DEAD:
		return "dead";
	case YAFFS_BLOCK_STATE_UNKNOWN:
	case YAFFS_BLOCK_STATE_SCANNING:
	case YAFFS_BLOCK_STATE_NEEDS_SCAN:
	default:
		return "other";
	}
}

static void disagree(struct wire_ckpt_report *r, uint32_t b, const struct yaffs_block_info *bi,
		     bool used, uint32_t seq)
{
	if (r->disagree < WIRE_CKPT_KEPT) {
		struct text t = t_open(r->disagreement[r->disagree]);

		t_str(&t, "block ");
		t_dec(&t, b);
		t_str(&t, ": restored ");
		t_str(&t, state_name((enum yaffs_block_state)bi->block_state));
		t_str(&t, " sequence ");
		t_hex(&t, bi->seq_number);
		t_str(&t, used ? ", on the eMMC in use, sequence " : ", on the eMMC unused");
		if (used)
			t_hex(&t, seq);
	}
	r->disagree++;
}

/* The restored mount's state for a block against its chunk 0 on the
 * eMMC (fs/checkpoint.h). */
static bool agrees(const struct yaffs_block_info *bi, bool used, uint32_t seq)
{
	if (used && seq == YAFFS_SEQUENCE_CHECKPOINT_DATA)
		return bi->block_state == YAFFS_BLOCK_STATE_CHECKPOINT;
	switch (bi->block_state) {
	case YAFFS_BLOCK_STATE_EMPTY:
		return !used;
	case YAFFS_BLOCK_STATE_ALLOCATING:
	case YAFFS_BLOCK_STATE_FULL:
	case YAFFS_BLOCK_STATE_DIRTY:
	case YAFFS_BLOCK_STATE_COLLECTING:
		return used && seq == bi->seq_number;
	case YAFFS_BLOCK_STATE_CHECKPOINT:
	case YAFFS_BLOCK_STATE_DEAD:
	case YAFFS_BLOCK_STATE_UNKNOWN:
	case YAFFS_BLOCK_STATE_SCANNING:
	case YAFFS_BLOCK_STATE_NEEDS_SCAN:
	default:
		return false;
	}
}

/* A chunk's tags (the driver's numbering); false when the read failed. */
static bool tags_of(struct yaffs_dev *d, int chunk, bool *used, uint32_t *seq)
{
	struct yaffs_ext_tags t;

	memset(&t, 0, sizeof t);
	if (d->tagger.read_chunk_tags_fn(d, chunk, NULL, &t) != YAFFS_OK)
		return false;
	*used = t.chunk_used != 0u;
	*seq = t.seq_number;
	return true;
}

bool wire_ckpt_stale(struct yaffs_dev *d, struct wire_ckpt_stale *s,
		     wire_ckpt_disagree_fn each, void *ctx)
{
	const int per = (int)d->param.chunks_per_block;
	uint32_t b;

	memset(s, 0, sizeof *s);
	s->ckpt_seq = d->seq_number;
	s->alloc_block = d->alloc_block >= 0 ? d->alloc_block - d->block_offset : -1;
	s->alloc_page = d->alloc_page;
	for (b = d->internal_start_block; b <= d->internal_end_block; b++) {
		const struct yaffs_block_info *bi = yaffs_get_block_info(d, (int)b);
		const uint32_t fb = b - (uint32_t)d->block_offset;	/* the volume's numbering */
		bool used = false;
		uint32_t seq = 0;

		if (!tags_of(d, (int)fb * per, &used, &seq)) {
			s->failed++;
			continue;
		}
		s->blocks++;
		if (used && seq == YAFFS_SEQUENCE_CHECKPOINT_DATA)
			s->ckpt_blocks++;
		else if (used && seq > s->ckpt_seq && seq <= YAFFS_HIGHEST_SEQUENCE_NUMBER)
			s->newer++;
		if (!agrees(bi, used, seq)) {
			s->disagree++;
			if (each != NULL)
				each(ctx, fb, bi, used, seq);
		}
	}
	/* yaffs2 writes a block's pages in order, so a write after the
	 * checkpoint into the block it was filling lies at or after the
	 * allocating page; every one of them is read */
	if (d->alloc_block >= 0) {
		for (uint32_t p = d->alloc_page; p < d->param.chunks_per_block; p++) {
			bool used = false;
			uint32_t seq = 0;

			if (!tags_of(d, d->alloc_block * per + (int)p - d->chunk_offset, &used, &seq))
				s->failed++;
			else if (used)
				s->alloc_used++;
		}
	}
	return !d->is_checkpointed || s->newer != 0u || s->disagree != 0u ||
	       s->alloc_used != 0u || s->failed != 0u;
}

static void told(void *ctx, uint32_t block, const struct yaffs_block_info *bi, bool used,
		 uint32_t seq)
{
	disagree(ctx, block, bi, used, seq);
}

/* The stale check on a restored mount (wire_ckpt_stale()); on a scanned
 * one, the checkpoint's blocks counted only. */
static void guard(struct yaffs_dev *d, struct wire_ckpt_report *r, bool restored)
{
	struct wire_ckpt_stale s;
	uint32_t b;

	r->ckpt_seq = d->seq_number;
	r->alloc_block = d->alloc_block >= 0 ? d->alloc_block - d->block_offset : -1;
	r->alloc_page = d->alloc_page;
	if (restored) {
		(void)wire_ckpt_stale(d, &s, told, r);
		r->ckpt_blocks = s.ckpt_blocks;
		r->newer = s.newer;
		r->alloc_used = s.alloc_used != 0u;
		r->read_failed = s.failed;
		return;
	}
	for (b = d->internal_start_block; b <= d->internal_end_block; b++) {
		bool used = false;
		uint32_t seq = 0;

		if (tags_of(d, (int)((b - (uint32_t)d->block_offset) * d->param.chunks_per_block),
			    &used, &seq) &&
		    used && seq == YAFFS_SEQUENCE_CHECKPOINT_DATA)
			r->ckpt_blocks++;
	}
}

/* ---- why a checkpoint was not taken ---- */

/* As far as the stream's first chunk tells: its header, the validity
 * record and the device record's size (yaffs_yaffs2.c's first checks). */
static void why(struct yaffs_dev *d, struct wire_ckpt_report *r)
{
	struct text t = t_open(r->why);
	uint8_t *buf;
	uint32_t b;
	bool found = false;

	if (r->ckpt_blocks == 0u) {
		t_str(&t, "no checkpoint on the volume");
		return;
	}
	buf = yaffs_get_temp_buffer(d);
	for (b = d->internal_start_block; b <= d->internal_end_block && !found; b++) {
		const int chunk = (int)((b - (uint32_t)d->block_offset) * d->param.chunks_per_block);
		struct yaffs_ext_tags tags;

		/* the tags alone first; the data only of the first chunk */
		memset(&tags, 0, sizeof tags);
		if (d->tagger.read_chunk_tags_fn(d, chunk, NULL, &tags) != YAFFS_OK ||
		    tags.seq_number != YAFFS_SEQUENCE_CHECKPOINT_DATA || tags.chunk_id != 1u)
			continue;
		found = d->tagger.read_chunk_tags_fn(d, chunk, buf, &tags) == YAFFS_OK;
	}
	if (!found) {
		t_str(&t, "its first chunk is not on the volume");
	} else if (wb_get_le32(buf) != YAFFS_CHECKPOINT_VERSION) {
		t_str(&t, "its version is ");
		t_dec(&t, wb_get_le32(buf));
	} else if (wb_get_le32(buf + CKPT_HDR) != VALIDITY ||
		   wb_get_le32(buf + CKPT_HDR + 4u) != YAFFS_MAGIC ||
		   wb_get_le32(buf + CKPT_HDR + 8u) != YAFFS_CHECKPOINT_VERSION ||
		   wb_get_le32(buf + CKPT_HDR + 12u) != 1u) {
		t_str(&t, "its opening validity record is not yaffs2's");
	} else if (wb_get_le32(buf + CKPT_HDR + VALIDITY) != sizeof(struct yaffs_checkpt_dev)) {
		t_str(&t, "its device record is ");
		t_dec(&t, wb_get_le32(buf + CKPT_HDR + VALIDITY));
		t_str(&t, " bytes, this build's ");
		t_dec(&t, (uint32_t)sizeof(struct yaffs_checkpt_dev));
	} else {
		t_str(&t, "a later check: the block or object records, the tnodes, the closing "
			  "record or the checksum");
	}
	yaffs_release_temp_buffer(d, buf);
}

/* ---- the run ---- */

static uint32_t since(uint64_t (*now_us)(void), uint64_t t0)
{
	const uint64_t d = now_us() - t0;

	return d > UINT32_MAX ? UINT32_MAX : (uint32_t)d;
}

enum wire_ckpt_outcome wire_ckpt_run(const struct wb_blockdev *bd, uint32_t first,
				     uint32_t last, const struct wire_ckpt_tree_ops *scan,
				     uint64_t (*now_us)(void), struct wire_ckpt_report *r)
{
	struct yaffs_dev *d = &ckpt_dev;
	struct tree s, c;
	const uint64_t t0 = now_us();
	uint64_t m0;

	memset(r, 0, sizeof *r);
	memset(&s, 0, sizeof s);
	memset(&c, 0, sizeof c);
	r->alloc_block = -1;
	wrap.under = bd;
	wrap.r = r;
	wrap.reads = wrap.sectors = 0;
	ro_bd.ctx = NULL;
	ro_bd.n_sectors = bd->n_sectors;
	ro_bd.read = ro_read;
	ro_bd.write = wire_ckpt_refused_write;
	ro_bd.erase = ro_erase;
	ro_bd.erase_zeroes = NULL;
	ro_bd.write_reliable = wire_ckpt_refused_write;
	if (wb_yaffs_add_blockdev(d, MOUNT, &ro_bd, first, last) != 0) {
		r->outcome = WIRE_CKPT_BAD_VOLUME;
		return r->outcome;
	}
	/* S-12.055 stands for the bootloader's own mount; this one reads the
	 * checkpoint and never writes one. */
	d->param.skip_checkpt_rd = 0;
	d->param.skip_checkpt_wr = 1;
	m0 = now_us();
	if (yaffs_mount2(MOUNT, 1) != 0) {
		yaffs_remove_device(d);
		r->mount_us = since(now_us, m0);
		r->mount_reads = wrap.reads;
		r->mount_sectors = wrap.sectors;
		r->outcome = yaffs_get_error() == -ENOMEM ? WIRE_CKPT_NO_MEMORY :
							   WIRE_CKPT_NOT_MOUNTED;
		r->test_us = since(now_us, t0);
		return r->outcome;
	}
	r->mount_us = since(now_us, m0);
	r->mount_reads = wrap.reads;
	r->mount_sectors = wrap.sectors;
	r->accepted = d->is_checkpointed != 0u;

	guard(d, r, r->accepted);
	if (!r->accepted)
		why(d, r);

	s.ops = scan;
	walk(&s);
	c.ops = &ck_ops;
	walk(&c);
	r->objs_scan = s.n;
	r->objs_ckpt = c.n;
	r->scan_cut = s.cut;
	r->ckpt_cut = c.cut;
	compare(r, &s, &c);
	tree_free(&s);
	tree_free(&c);

	r->unmounted = yaffs_unmount2(MOUNT, 1) == 0;
	yaffs_remove_device(d);
	r->test_us = since(now_us, t0);
	r->outcome = WIRE_CKPT_DONE;
	return r->outcome;
}

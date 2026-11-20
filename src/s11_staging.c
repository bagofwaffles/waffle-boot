/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/s11_staging.h"

#include <string.h>

#include "waffle/bytes.h"
#include "waffle/s01_memmap.h"
#include "waffle/s05_handoff.h"
#include "waffle/s11_engine.h"

#define SPACE_RESERVE 0x400000u	/* S-11.033 */

const struct s11_crc_engine *s11_crc_engine;

/* S-11.034, S-11.035 */
const struct s11_component_info s11_components[S11_COMPONENTS] = {
	[S11_C_OS] = { "firmware/blackfin/bin/os.ldr",
		       "firmware/blackfin/bin/os.version", 0x800000u },
	[S11_C_BOOTLOADER] = { "firmware/blackfin/bin/bootloader.ldr",
			       "firmware/blackfin/bin/bootloader.version",
			       0x78000u },
	/* the Nordic refuses a chunk reaching its 0x70000-byte area's end */
	[S11_C_BLE] = { "firmware/ble/bin/ble.bin",
			"firmware/ble/bin/ble.version", 0x6FFFFu },
	[S11_C_NORDIC_BL0] = { "firmware/ble/bin/nordic_bootloader_0.bin",
			       "firmware/ble/bin/nordic_bootloader.version",
			       0x3000u },
	[S11_C_NORDIC_BL1] = { "firmware/ble/bin/nordic_bootloader_1.bin",
			       "firmware/ble/bin/nordic_bootloader.version",
			       0x3000u },
	[S11_C_BACKPLANE] = { "firmware/backplane/bin/backplane.cyacd",
			      "firmware/backplane/bin/backplane.version",
			      0x30000u },
};

/* `a` then `b` into `out` (S11_PATH_MAX); false when too long. */
static bool join(char *out, const char *a, const char *b)
{
	size_t na = wb_strlen(a), nb = wb_strlen(b);

	if (na + nb >= S11_PATH_MAX)
		return false;
	for (size_t i = 0; i < na; i++)
		out[i] = a[i];
	for (size_t i = 0; i <= nb; i++)
		out[na + i] = b[i];
	return true;
}

/* S-11.033 */
enum s11_reason s11_space_rule(uint64_t total, bool known, uint64_t free)
{
	if (!known)
		return S11_REASON_SPACEQUERY;
	return free < total + SPACE_RESERVE ? S11_REASON_SPACE : S11_REASON_NONE;
}

/* A path component of n bytes at c: not "." or "..", and empty only as the
 * last, after a directory member's trailing '/'. */
static bool component_ok(const char *c, size_t n, bool last)
{
	if (n == 0)
		return last;
	return c[0] != '.' || (n != 1 && (n != 2 || c[1] != '.'));
}

/* S-11.031: canonical names only. */
bool s11_member_name_ok(const char *name)
{
	const char *c = name;

	if (name[0] == '\0')
		return false;
	for (const char *p = name;; p++) {
		if (*p == '\\')
			return false;
		if (*p == '/' || *p == '\0') {	/* end of a path component */
			if (!component_ok(c, (size_t)(p - c), *p == '\0'))
				return false;
			if (*p == '\0')
				return true;
			c = p + 1;
		}
	}
}

/*
 * S-05.024's check of a staged OS image (s05_check_image()), made on the
 * bytes as they are extracted: every block header good, no refused flag,
 * every destination one S-01.017 allows, and a FINAL block whose payload
 * is all there. Bytes after it are taken and ignored.
 */
struct os_check {
	uint8_t hdr[S05_LDR_HEADER];
	unsigned hn;			/* header bytes held */
	uint64_t skip;			/* payload bytes still to pass */
	bool in_final;			/* that payload is the FINAL block's */
	bool done;			/* the FINAL block has ended, or a header failed */
	bool ok;
};

static void os_check_begin(struct os_check *c)
{
	memset(c, 0, sizeof(*c));
	c->ok = true;
}

static void os_check_header(struct os_check *c)
{
	struct s05_block b;

	if (!s05_ldr_header_ok(c->hdr)) {
		c->ok = false;
		c->done = true;
		return;
	}
	s05_ldr_decode(c->hdr, &b);
	if ((b.flags & S05_FLAGS_REFUSED) != 0u ||
	    (s05_block_has_destination(&b) &&
	     s01_os_destination(b.target, b.count) != S01_OS_OK))
		c->ok = false;
	c->skip = (b.flags & S05_FLAG_FILL) != 0u ? 0u : b.count;
	c->in_final = (b.flags & S05_FLAG_FINAL) != 0u;
	c->done = !c->ok || (c->in_final && c->skip == 0u);
}

static void os_check_feed(struct os_check *c, const uint8_t *p, size_t n)
{
	while (n != 0u && !c->done) {
		size_t k;

		if (c->skip != 0u) {
			k = n < c->skip ? n : (size_t)c->skip;
			c->skip -= k;
			c->done = c->in_final && c->skip == 0u;
		} else {
			k = S05_LDR_HEADER - c->hn;
			if (k > n)
				k = n;
			memcpy(c->hdr + c->hn, p, k);
			c->hn += (unsigned)k;
			if (c->hn == S05_LDR_HEADER) {
				c->hn = 0;
				os_check_header(c);
			}
		}
		p += k;
		n -= k;
	}
}

/* The verdict on an image of len bytes: it fits the staging area, and its
 * FINAL block ended with every check passed. */
static bool os_check_end(const struct os_check *c, uint64_t len)
{
	return len <= S01_STAGING_MAX && c->done && c->ok;
}

struct extract {
	const struct s11_zip_ops *ops;
	uint32_t crc;
	uint64_t len;
	struct s01_stream *stream;	/* check 18's, for a protected component */
	struct os_check *os;		/* check 18's, for the OS image */
};

static bool sink(void *arg, const uint8_t *p, size_t n)
{
	struct extract *x = arg;

	x->crc = x->ops->crc32(x->ops->ctx, x->crc, p, n);
	x->len += n;
	if (x->stream != NULL)
		s01_stream_feed(x->stream, p, n);
	if (x->os != NULL)
		os_check_feed(x->os, p, n);
	return x->ops->staging_write(x->ops->ctx, p, n);
}

/* The member written to its staging file from the start, through check
 * 18's stream from its start; false when it cannot be read or written in
 * full or has not the stated length. */
static bool extract_once(const struct s11_zip_ops *ops, size_t index,
			 const struct s11_member *m, struct extract *x,
			 struct s01_stream *stream, struct os_check *os)
{
	bool ok;

	x->crc = 0;
	x->len = 0;
	x->stream = NULL;
	x->os = NULL;
	if (wb_streq(m->name, s11_components[S11_C_OS].image)) {
		os_check_begin(os);
		x->os = os;
	} else if (wb_streq(m->name, s11_components[S11_C_BOOTLOADER].image)) {
		s01_bootable_begin(stream, S01_BOOTLOADER);
		x->stream = stream;
	}
	/* the file created empty: a second extraction replaces the first */
	if (!ops->staging_open(ops->ctx, m->name))
		return false;
	ok = ops->extract(ops->ctx, index, sink, x);
	ok = ops->staging_close(ops->ctx) && ok;
	return ok && x->len == m->size;
}

/*
 * S-11.031, S-11.032: one member into the staging area. The bytes of the
 * bootloader image go through S-02.091's check as they are written, those
 * of the OS image through S-05.024's; bootable[c] is the verdict on the
 * file staged last.
 */
static bool stage(const struct s11_zip_ops *ops, size_t index,
		  const struct s11_member *m, struct s11_staged_sizes *staged,
		  bool bootable[S11_COMPONENTS])
{
	size_t n = wb_strlen(m->name);
	struct s01_stream stream;
	struct os_check os;
	struct extract x = { ops, 0, 0, NULL, NULL };

	if (m->name[n - 1] == '/') {		/* a directory entry */
		char dir[S11_PATH_MAX];

		for (size_t i = 0; i < n; i++)
			dir[i] = m->name[i];
		dir[n - 1] = '\0';
		return ops->staging_mkdir(ops->ctx, dir);
	}
	/* S-11.030: inflated in full to the stated length and CRC-32. A
	 * CRC-32 an engine took part in is computed once more in software
	 * before it refuses the member (s11_engine.h). */
	if (!extract_once(ops, index, m, &x, &stream, &os))
		return false;
	if (x.crc != m->crc32 &&
	    (s11_crc_engine == NULL || !s11_crc_engine->crc32_again() ||
	     !extract_once(ops, index, m, &x, &stream, &os) ||
	     x.crc != m->crc32))
		return false;
	for (unsigned c = 0; c < S11_COMPONENTS; c++) {
		if (wb_streq(m->name, s11_components[c].image)) {
			staged->size[c] = m->size;
			staged->crc32[c] = m->crc32;	/* equal to x.crc */
			bootable[c] = x.os != NULL ? os_check_end(x.os, x.len) :
				      x.stream == NULL ||
				      s01_stream_end(x.stream) == 0u;
		}
	}
	return true;
}

/* Checks 16 to 18 (S-11.020) */
enum s11_reason s11_check_zip(const struct s11_zip_ops *ops,
			      struct s11_staged_sizes *staged)
{
	struct s11_member m;
	uint64_t total = 0, free = 0;
	size_t count = 0;
	enum s11_reason r;
	bool known, bootable[S11_COMPONENTS];

	for (unsigned c = 0; c < S11_COMPONENTS; c++) {
		staged->size[c] = 0;
		staged->crc32[c] = 0;
		bootable[c] = true;
	}
	/* S-11.031, S-11.033, S-11.020: the staging area, with any progress
	 * record, is emptied when check 16 begins, so every refusal of checks
	 * 16 and 17 leaves none behind. An area the volume cannot empty is
	 * refused as a member that cannot be written (S-11.031). */
	if (!ops->staging_reset(ops->ctx))
		return S11_REASON_MEMBER;
	/* S-11.030, S-11.031: every member is vouched for before any write */
	if (ops->zip64_end(ops->ctx))
		return S11_REASON_MEMBER;
	while (ops->member(ops->ctx, count, &m)) {
		if (m.zip64 || (m.method != 0 && m.method != 8) ||
		    (m.flags & 1u) != 0 || !s11_member_name_ok(m.name))
			return S11_REASON_MEMBER;
		total += m.size;		/* S-11.033: no overflow */
		count++;
	}
	known = ops->free_space(ops->ctx, &free);
	r = s11_space_rule(total, known, free);
	if (r != S11_REASON_NONE)
		return r;
	/* S-11.031: a member the volume fails to write is refused MEMBER */
	for (size_t i = 0; i < count; i++) {
		if (!ops->member(ops->ctx, i, &m) ||
		    !stage(ops, i, &m, staged, bootable))
			return S11_REASON_MEMBER;
	}
	/* S-11.035: every component, whether or not the run would install it */
	for (unsigned c = 0; c < S11_COMPONENTS; c++) {
		if (staged->size[c] > s11_components[c].limit)
			return S11_REASON_SIZE;
	}
	/* Check 18 (S-11.035, S-02.091, S-05.024): a present bootloader image
	 * is a bootable slot stream, option or not, and a present OS image one
	 * the hand-over accepts. */
	for (unsigned c = 0; c < S11_COMPONENTS; c++) {
		if (staged->size[c] > 0u && !bootable[c])
			return S11_REASON_STREAM;
	}
	return S11_REASON_NONE;
}

/* ---- S-11.095 ---- */

static bool same_read(const struct s11_staged_read *a,
		      const struct s11_staged_read *b)
{
	return a->len == b->len && a->crc32 == b->crc32;
}

/* S-11.095: the read must have the member's stated length and CRC-32. A
 * read that failed is a read error before any comparison. */
enum s11_staged_check s11_staged_check(const struct s11_staged_read *stated,
				       const struct s11_staged_read *read)
{
	if (!read->ok)
		return S11_STAGED_READ_ERROR;
	return same_read(read, stated) ? S11_STAGED_OK : S11_STAGED_BAD;
}

enum s11_result s11_staged_result(enum s11_staged_check c)
{
	switch (c) {
	case S11_STAGED_OK:
		return S11_OK;
	case S11_STAGED_BAD:
		return S11_BAD_FILE;
	case S11_STAGED_READ_ERROR:
	default:
		return S11_INTERNAL;
	}
}

/* ---- the staging area as a run reads it ---- */

/* A file's size, 0 when it is absent or a directory: nothing the stat
 * leaves behind on failure is taken. */
static bool stat_file(const struct s11_fs_ops *fs, const char *path,
		      uint32_t *size)
{
	bool dir = false;
	uint32_t n = 0;

	*size = 0;
	if (!fs->stat(fs->ctx, path, &dir, &n) || dir)
		return false;
	*size = n;
	return true;
}

static struct s11_version read_version(const struct s11_fs_ops *fs,
				       const char *path, bool *present)
{
	uint8_t buf[S11_VERSION_FILE_WINDOW];
	uint32_t size;
	size_t n = 0;

	*present = stat_file(fs, path, &size);
	if (*present)
		n = fs->read(fs->ctx, path, buf, sizeof(buf));
	return s11_version_file(buf, n, *present);	/* S-11.036 */
}

void s11_staged_scan(const struct s11_fs_ops *fs, struct s11_staged *st)
{
	char path[S11_PATH_MAX];
	bool dir = false, present;
	uint32_t size = 0;

	st->exists = fs->stat(fs->ctx, "upgrade", &dir, &size) && dir;
	for (unsigned c = 0; c < S11_COMPONENTS; c++) {
		st->size[c] = 0;
		if (join(path, "upgrade/", s11_components[c].image))
			(void)stat_file(fs, path, &st->size[c]);
		st->version[c] = (struct s11_version){ 1234, 0, 0, 0 };
		if (join(path, "upgrade/", s11_components[c].version))
			st->version[c] = read_version(fs, path, &present);
	}
	dir = false;
	st->content_dir = fs->stat(fs->ctx, "upgrade/firmware/blackfin/content",
				   &dir, &size) && dir;
	st->fw_version = read_version(fs, "upgrade/" S11_FW_VERSION,
				      &st->has_fw_version);
}

bool s11_staged_present(const struct s11_staged *st, enum s11_component c)
{
	return st->size[c] != 0;
}

bool s11_staged_content_only(const struct s11_staged *st)
{
	for (unsigned c = 0; c < S11_COMPONENTS; c++) {
		if (st->size[c] != 0)
			return false;
	}
	return st->content_dir;
}

bool s11_staged_nordic(const struct s11_staged *st)
{
	return st->size[S11_C_BLE] != 0 || st->size[S11_C_NORDIC_BL0] != 0 ||
	       st->size[S11_C_NORDIC_BL1] != 0;
}

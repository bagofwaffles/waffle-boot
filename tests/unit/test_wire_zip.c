/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-11 checks 16 and 17 on minizip and yaffs2 (src/wire_zip.c,
 * src/wire_yaffs.c): every zip of zips.json (zips.txt) through
 * s11_check_zip() with minizip reading it and the members staged into a
 * yaffs2 volume in RAM, against s11_vectors.h's reason, components and
 * staged files (S-11.030-S-11.035). Also: the member table minizip gives
 * (name, method, flags, size, CRC-32, zip64) against Python's zipfile, each
 * member's data against its stated contents, and that check 16 empties the
 * staging area whatever follows (S-11.031). The free space is the vector's
 * (S-11.033 is s11_staging's); the volume's own is checked in
 * test_wire_yaffs. Then the extraction on hand-made zips (zip_build.h):
 * sizes around the window and minizip's reads, member order, and every way
 * a stream ends or fails. test_wire_zip_diff compares the reader with
 * minizip's own read path on mutated zips.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "waffle/s11_staging.h"
#include "waffle/wire_crc.h"
#include "waffle/wire_yaffs.h"
#include "waffle/wire_zip.h"

#include "vectors/s11_vectors.h"
#include "zip_build.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define B(r) (s11v_blob + (r).off)
#define BLOCKS 40u

static struct wb_blockdev bd;
static struct wb_ramdisk ram;
static struct wire_yaffs Y;
static const struct s11v_zip *cur;

static bool vector_free(void *ctx, uint64_t *bytes)
{
	(void)ctx;
	*bytes = cur->free;
	return cur->free_known != 0;
}

/* zips.txt: the raw zip of a case */
static uint8_t *find_zip(const char *name, size_t *len)
{
	FILE *f = tl_open_vectors("zips.txt");
	char *line = NULL, *fld[4];
	size_t cap = 0;
	uint8_t *raw = NULL;
	int n;

	while (raw == NULL && (n = tl_fields(f, &line, &cap, fld, 4)) >= 0) {
		if (n == 3 && !strcmp(fld[0], "zip") && !strcmp(fld[1], name))
			raw = tl_unhex(fld[2], len);
	}
	free(line);
	fclose(f);
	return raw;
}

/* A member's stated contents (run-length) against what extraction gives. */
struct expect {
	const uint8_t *runs;
	size_t nruns, run;
	uint32_t left;
	uint64_t len;
	bool same;
};

static bool expect_sink(void *arg, const uint8_t *p, size_t n)
{
	struct expect *x = arg;

	for (size_t i = 0; i < n; i++) {
		while (x->left == 0 && x->run < x->nruns) {
			const uint8_t *r = x->runs + 5 * x->run++;

			x->left = (uint32_t)r[1] << 24 | (uint32_t)r[2] << 16 |
				  (uint32_t)r[3] << 8 | r[4];
		}
		if (x->left == 0 ||
		    p[i] != x->runs[5 * (x->run - 1)])
			x->same = false;
		else
			x->left--;
	}
	x->len += n;
	return true;
}

static void check_members(const struct s11v_zip *v, struct s11_zip_ops *ops)
{
	struct s11_member m;
	size_t i;

	for (i = 0; ops->member(ops->ctx, i, &m); i++) {
		const struct s11v_member *w = &s11v_members[v->first + i];
		struct expect x = { B(w->runs), w->runs.len / 5, 0, 0, 0, true };
		bool ok;

		if (i >= v->count)
			break;
		CHECK(!strcmp(m.name, w->name) && m.method == w->method &&
		      m.flags == w->flags && m.size == w->size &&
		      m.crc32 == w->crc && m.zip64 == (w->zip64_extra != 0),
		      "%s: member %zu (%s) as minizip reports it", v->name, i,
		      w->name);
		if (m.method != 0 && m.method != 8)
			continue;	/* never extracted (S-11.030) */
		ok = ops->extract(ops->ctx, i, expect_sink, &x);
		if (w->data_ok)
			CHECK(ok && x.same && x.len == w->size, "%s: %s data",
			      v->name, w->name);
		else
			CHECK(!ok || x.len != w->size || !x.same,
			      "%s: %s damage seen", v->name, w->name);
	}
	CHECK(i == v->count, "%s: %zu members, want %zu", v->name, i, v->count);
}

/* The staged files, relative to upgrade/, one per line in `out`. */
static void staged_files(const char *dir, char *out, size_t cap)
{
	char name[S11_PATH_MAX], path[600];
	bool is_dir;

	for (size_t i = 0; wire_yaffs_list(&Y, dir, i, name, sizeof(name), &is_dir,
					   NULL); i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		if (is_dir) {
			staged_files(path, out, cap);
			continue;
		}
		snprintf(out + strlen(out), cap - strlen(out), "%s\n",
			 path + strlen("upgrade/"));
	}
}

static int line_count(const char *s)
{
	int n = 0;

	for (; *s != '\0'; s++)
		n += *s == '\n';
	return n;
}

/* every line of `want` (no trailing newline) is in `got` and no other */
static bool same_lines(const char *want, const char *got)
{
	int lines = 0;

	for (const char *p = want; *p != '\0';) {
		size_t n = strcspn(p, "\n");
		const char *g = got;
		bool found = false;

		while (!found && *g != '\0') {
			size_t k = strcspn(g, "\n");

			found = k == n && !strncmp(g, p, n);
			g += k + (g[k] == '\n');
		}
		if (!found)
			return false;
		lines++;
		p += n + (p[n] == '\n');
	}
	return lines == line_count(got);
}

static void put_file(const char *rel, const char *text)
{
	struct s11_fs_ops fs;
	char p[S11_PATH_MAX];

	wire_yaffs_fs_ops(&Y, &fs);
	snprintf(p, sizeof(p), "%s", rel);
	*strrchr(p, '/') = '\0';
	CHECK(wire_yaffs_mkdir(&Y, p) && wire_yaffs_staging_open(&Y, "x") &&
	      wire_yaffs_staging_write(&Y, (const uint8_t *)text, strlen(text)) &&
	      wire_yaffs_staging_close(&Y) && fs.move(fs.ctx, "upgrade/x", rel),
	      "put %s", rel);
}

static void check_zip(const struct s11v_zip *v)
{
	struct wire_zip z;
	struct s11_zip_ops ops;
	struct s11_staged_sizes st;
	enum s11_reason r;
	size_t len = 0;
	uint8_t *raw = find_zip(v->name, &len);
	char want[2048] = "", got[2048] = "";
	bool dir;
	uint32_t size;
	unsigned mask = 0;

	CHECK(raw != NULL, "%s: not in zips.txt", v->name);
	if (raw == NULL)
		return;
	cur = v;
	wire_zip_init(&z, &ops, NULL, raw, len, &Y);
	check_members(v, &ops);
	ops.free_space = vector_free;

	/* an earlier progress record, and a file outside the staging area */
	CHECK(wire_yaffs_staging_reset(&Y), "reset");
	put_file("upgrade/progress.log", "\x01\x00");
	r = s11_check_zip(&ops, &st);
	wire_zip_close(&z);
	CHECK(r == v->reason, "%s: %s, want %s", v->name, s11_reason_name(r),
	      s11_reason_name(v->reason));
	CHECK(wire_yaffs_stat(&Y, "user/keep", &dir, &size) && size == 4,
	      "%s: nothing outside the staging area touched", v->name);
	/* S-11.031, S-11.033: emptied when check 16 begins, whatever follows */
	CHECK(!wire_yaffs_stat(&Y, "upgrade/progress.log", &dir, &size),
	      "%s: the earlier progress record removed", v->name);
	if (r != S11_REASON_NONE && r != S11_REASON_SIZE)
		goto out;
	for (unsigned c = 0; c < S11_COMPONENTS; c++)
		mask |= (st.size[c] != 0 ? 1u : 0u) << c;
	if (r == S11_REASON_NONE)
		CHECK(mask == v->components, "%s: components %02x, want %02x",
		      v->name, mask, v->components);
	/* the staged files are the members that are not directories */
	if (v->staged != NULL) {
		snprintf(want, sizeof(want), "%s", v->staged);
	} else {
		for (size_t i = 0; i < v->count; i++) {
			const char *name = s11v_members[v->first + i].name;

			if (name[strlen(name) - 1] == '/')
				continue;
			snprintf(want + strlen(want), sizeof(want) - strlen(want),
				 "%s%s", *want ? "\n" : "", name);
		}
	}
	staged_files("upgrade", got, sizeof(got));
	CHECK(same_lines(want, got), "%s: staged\n%s\nwant\n%s", v->name, got,
	      want);
	for (size_t i = 0; i < v->count && r == S11_REASON_NONE; i++) {
		const struct s11v_member *w = &s11v_members[v->first + i];
		char p[S11_PATH_MAX + 8];

		if (w->name[strlen(w->name) - 1] == '/')
			continue;
		snprintf(p, sizeof(p), "upgrade/%s", w->name);
		CHECK(wire_yaffs_stat(&Y, p, &dir, &size) && !dir &&
		      size == w->size, "%s: %s staged in full", v->name, w->name);
	}
out:
	free(raw);
}

/*
 * One stored member "a" holding "hi", with the same extra field in its local
 * and central headers: nrec 500-byte records of id 0xCAFE (with 3, 1500
 * bytes, more than WIRE_ZIP_EXTRA_MAX), then, with zip64, an empty id 1
 * record.
 */
static size_t extra_zip(uint8_t *out, int nrec, bool zip64)
{
	uint8_t extra[1504];
	size_t ne = 0, k = 0, cd, end;
	const uint32_t crc = wire_crc32(0, (const uint8_t *)"hi", 2);

	memset(out, 0, 4096);
	for (int r = 0; r < nrec; r++) {
		ne += put16(extra + ne, 0xCAFEu) + put16(extra + ne + 2, 496);
		memset(extra + ne, 0x55, 496);
		ne += 496;
	}
	if (zip64)
		ne += put16(extra + ne, 1) + put16(extra + ne + 2, 0);
	/* local header (APPNOTE 4.3.7) */
	k += put32(out + k, 0x04034B50u) + put16(out + k + 4, 20);
	k += put16(out + k, 0) + put16(out + k + 2, 0);
	k += 4 + put32(out + k + 4, crc);
	k += put32(out + k, 2) + put32(out + k + 4, 2);
	k += put16(out + k, 1) + put16(out + k + 2, (unsigned)ne);
	out[k++] = 'a';
	memcpy(out + k, extra, ne);
	k += ne;
	memcpy(out + k, "hi", 2);
	k += 2;
	/* central header (APPNOTE 4.3.12) */
	cd = k;
	k += put32(out + k, 0x02014B50u) + put16(out + k + 4, 20) +
	     put16(out + k + 6, 20);
	k += put16(out + k, 0) + put16(out + k + 2, 0);
	k += 4 + put32(out + k + 4, crc);
	k += put32(out + k, 2) + put32(out + k + 4, 2);
	k += put16(out + k, 1) + put16(out + k + 2, (unsigned)ne);
	k += put16(out + k, 0) + put16(out + k + 2, 0) + put16(out + k + 4, 0);
	k += put32(out + k, 0) + put32(out + k + 4, 0);
	out[k++] = 'a';
	memcpy(out + k, extra, ne);
	k += ne;
	/* end of central directory (APPNOTE 4.3.16) */
	end = k;
	k += put32(out + k, 0x06054B50u) + put16(out + k + 4, 0) +
	     put16(out + k + 6, 0);
	k += put16(out + k, 1) + put16(out + k + 2, 1);
	k += put32(out + k, (uint32_t)(end - cd)) +
	     put32(out + k + 4, (uint32_t)cd);
	k += put16(out + k, 0);
	return k;
}

/* S-11.030: an extra field is inspected in full, however long; only a zip64
 * record in it makes the member zip64. A central extra field of any length
 * is read (minizip seeks back over it, unzip_mem.c). */
static void test_long_extra(void)
{
	static uint8_t raw[4096];

	for (int c = 0; c < 4; c++) {
		const int nrec = c < 2 ? 1 : 3, zip64 = c & 1;
		struct wire_zip z;
		struct s11_zip_ops ops;
		struct s11_member m;
		const size_t len = extra_zip(raw, nrec, zip64 != 0);

		memset(&m, 0, sizeof(m));
		wire_zip_init(&z, &ops, NULL, raw, len, &Y);
		CHECK(ops.member(ops.ctx, 0, &m) && !strcmp(m.name, "a") &&
		      m.size == 2 && m.zip64 == (zip64 != 0),
		      "%d-byte extra field, zip64 %d: member read, zip64 %d",
		      500 * nrec + 4 * zip64, zip64, (int)m.zip64);
		CHECK(!ops.member(ops.ctx, 1, &m), "long extra field: one member");
		wire_zip_close(&z);
	}
}

/* ---- extraction (S-11.030): hand-made zips of stored and deflated members */

struct zmember {
	const char *name;
	uint16_t method;
	const uint8_t *data;		/* compressed */
	size_t len;			/* their length */
	uint32_t csize, usize, crc;	/* as the headers state them */
};

/* A zip of the members, at offset 0 of a new buffer. */
static uint8_t *make_zip(const struct zmember *m, size_t count, size_t *len)
{
	size_t k = 0, cd, end, cap = 22;
	uint32_t at[8];
	uint8_t *z;

	for (size_t i = 0; i < count; i++)
		cap += 76 + 2 * strlen(m[i].name) + m[i].len;
	z = tl_xmalloc(cap);
	for (size_t i = 0; i < count && i < 8; i++) {
		const size_t nl = strlen(m[i].name);

		at[i] = (uint32_t)k;
		k += put32(z + k, 0x04034B50u) + put16(z + k + 4, 20);
		k += put16(z + k, 0) + put16(z + k + 2, m[i].method);
		k += put32(z + k, 0) + put32(z + k + 4, m[i].crc);
		k += put32(z + k, m[i].csize) + put32(z + k + 4, m[i].usize);
		k += put16(z + k, (unsigned)nl) + put16(z + k + 2, 0);
		memcpy(z + k, m[i].name, nl);
		k += nl;
		memcpy(z + k, m[i].data, m[i].len);
		k += m[i].len;
	}
	cd = k;
	for (size_t i = 0; i < count && i < 8; i++) {
		const size_t nl = strlen(m[i].name);

		k += put32(z + k, 0x02014B50u) + put16(z + k + 4, 20) +
		     put16(z + k + 6, 20);
		k += put16(z + k, 0) + put16(z + k + 2, m[i].method);
		k += put32(z + k, 0) + put32(z + k + 4, m[i].crc);
		k += put32(z + k, m[i].csize) + put32(z + k + 4, m[i].usize);
		k += put16(z + k, (unsigned)nl) + put16(z + k + 2, 0);
		k += put16(z + k, 0) + put16(z + k + 2, 0) + put16(z + k + 4, 0);
		k += put32(z + k, 0) + put32(z + k + 4, at[i]);
		memcpy(z + k, m[i].name, nl);
		k += nl;
	}
	end = k;
	k += put32(z + k, 0x06054B50u) + put16(z + k + 4, 0) +
	     put16(z + k + 6, 0);
	k += put16(z + k, (unsigned)count) + put16(z + k + 2, (unsigned)count);
	k += put32(z + k, (uint32_t)(end - cd)) + put32(z + k + 4, (uint32_t)cd);
	k += put16(z + k, 0);
	*len = k;
	return z;
}

/* What the sink was given. */
struct got {
	uint8_t *buf;
	size_t cap, len, calls, max_piece, fail_at;
};

static bool got_sink(void *arg, const uint8_t *p, size_t n)
{
	struct got *g = arg;

	if (++g->calls == g->fail_at)
		return false;
	if (n > g->max_piece)
		g->max_piece = n;
	if (n <= g->cap - g->len)
		memcpy(g->buf + g->len, p, n);
	g->len += n;
	return true;
}

/* The zip read in place through a package's source, at offset ZIP_AT. */
#define ZIP_AT 0x40u

struct src_zip {
	const uint8_t *zip;
	size_t len, max_read;
};

static bool src_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	struct src_zip *s = ctx;

	if (off < ZIP_AT || off - ZIP_AT > s->len || n > s->len - (off - ZIP_AT))
		return false;
	memcpy(buf, s->zip + (off - ZIP_AT), n);
	if (n > s->max_read)
		s->max_read = n;
	return true;
}

static struct src_zip src_zip;
static struct wire_package src_pkg;

/* ops over the zip: in RAM, or (in_place) read through a package */
static void open_ops(struct wire_zip *z, struct s11_zip_ops *ops,
		     const uint8_t *zip, size_t len, bool in_place)
{
	if (!in_place) {
		wire_zip_init(z, ops, NULL, zip, len, &Y);
		return;
	}
	memset(&src_pkg, 0, sizeof(src_pkg));
	src_zip.zip = zip;
	src_zip.len = len;
	src_zip.max_read = 0;
	src_pkg.src.read = src_read;
	src_pkg.src.ctx = &src_zip;
	src_pkg.zip_len = len;
	src_pkg.zip_at = ZIP_AT;
	wire_zip_init(z, ops, &src_pkg, NULL, 0, &Y);
}

/* Extracts member i of the zip; g gets what the sink saw. */
static bool extract_one(const uint8_t *zip, size_t len, bool in_place,
			size_t i, struct got *g)
{
	struct wire_zip z;
	struct s11_zip_ops ops;
	bool ok;

	open_ops(&z, &ops, zip, len, in_place);
	ok = ops.extract(ops.ctx, i, got_sink, g);
	wire_zip_close(&z);
	return ok;
}

#define BIG 300000u

static uint8_t *raw_data, *comp, *sink_buf;

static void got_init(struct got *g, size_t fail_at)
{
	memset(g, 0, sizeof(*g));
	g->buf = sink_buf;
	g->cap = BIG + 65536u;
	g->fail_at = fail_at;
}

/* A stored and a deflated member of each size extract to their bytes, in
 * pieces of at most 32 KiB, from RAM and in place. */
static void test_sizes(void)
{
	static const size_t sizes[] = { 0, 1, 4095, 4096, 4097, 32767, 32768,
					32769, 65536, BIG };

	for (size_t s = 0; s < N(sizes); s++) {
		const size_t n = sizes[s];
		struct zmember m[2];
		size_t len, clen;
		uint8_t *zip;
		uint32_t crc;

		fill(raw_data, n, (uint32_t)n);
		clen = deflate_fixed(raw_data, n, comp);
		crc = wire_crc32(0, raw_data, (uint32_t)n);
		m[0] = (struct zmember){ "s", 0, raw_data, n, (uint32_t)n,
					 (uint32_t)n, crc };
		m[1] = (struct zmember){ "d", 8, comp, clen, (uint32_t)clen,
					 (uint32_t)n, crc };
		zip = make_zip(m, 2, &len);
		for (int place = 0; place < 2; place++) {
			for (size_t i = 0; i < 2; i++) {
				struct got g;
				bool ok;

				got_init(&g, 0);
				ok = extract_one(zip, len, place != 0, i, &g);
				CHECK(ok && g.len == n &&
				      !memcmp(g.buf, raw_data, n) &&
				      g.max_piece <= 32768u,
				      "%s member of %zu bytes%s: ok %d, %zu bytes, pieces up to %zu",
				      i ? "deflated" : "stored", n,
				      place ? ", in place" : "", ok, g.len,
				      g.max_piece);
				if (place)
					CHECK(src_zip.max_read <= 16384u,
					      "in place: reads of %zu bytes",
					      src_zip.max_read);
			}
		}
		free(zip);
	}
}

/* Members extract whatever the order (minizip goes back to the first). */
static void test_order(void)
{
	static const size_t sizes[4] = { 5000, 40000, 1, 70000 };
	static const size_t order[] = { 3, 0, 2, 1, 1, 3, 0, 2, 3 };
	uint8_t *data[4], *packed[4];
	struct zmember m[4];
	size_t len;
	uint8_t *zip;

	for (size_t i = 0; i < 4; i++) {
		static const char *const names[4] = { "a", "b/c", "d", "e" };

		data[i] = tl_xmalloc(sizes[i]);
		packed[i] = tl_xmalloc(sizes[i] * 9 / 8 + 16);
		fill(data[i], sizes[i], 77u + (uint32_t)i);
		m[i].name = names[i];
		m[i].method = (uint16_t)(i % 2 ? 0 : 8);
		m[i].data = m[i].method ? packed[i] : data[i];
		m[i].len = m[i].method ? deflate_fixed(data[i], sizes[i], packed[i]) :
					 sizes[i];
		m[i].csize = (uint32_t)m[i].len;
		m[i].usize = (uint32_t)sizes[i];
		m[i].crc = wire_crc32(0, data[i], (uint32_t)sizes[i]);
	}
	zip = make_zip(m, 4, &len);
	for (int place = 0; place < 2; place++) {
		struct wire_zip z;
		struct s11_zip_ops ops;

		open_ops(&z, &ops, zip, len, place != 0);
		for (size_t k = 0; k < N(order); k++) {
			const size_t i = order[k];
			struct got g;
			bool ok;

			got_init(&g, 0);
			ok = ops.extract(ops.ctx, i, got_sink, &g);
			CHECK(ok && g.len == sizes[i] &&
			      !memcmp(g.buf, data[i], sizes[i]),
			      "member %zu, %zu-th extraction%s", i, k,
			      place ? ", in place" : "");
		}
		wire_zip_close(&z);
	}
	free(zip);
	for (size_t i = 0; i < 4; i++) {
		free(data[i]);
		free(packed[i]);
	}
}

/* One member, deflated (or stored) from n bytes of fill(), with the sizes
 * and the CRC-32 stated as given; the extraction's verdict and what the
 * sink got. */
static bool extract_stated(uint16_t method, size_t n, uint32_t csize,
			   uint32_t usize, size_t fail_at, struct got *g)
{
	struct zmember m = { "m", method, raw_data, n, csize, usize, 0 };
	size_t len;
	uint8_t *zip;
	bool ok;

	fill(raw_data, n, 5u);
	if (method == 8)
		m.len = deflate_fixed(raw_data, n, comp);
	m.data = method == 8 ? comp : raw_data;
	if (csize == 0xFFFFFFFFu)
		m.csize = (uint32_t)m.len;
	m.crc = wire_crc32(0, raw_data, usize < n ? usize : (uint32_t)n);
	zip = make_zip(&m, 1, &len);
	got_init(g, fail_at);
	ok = extract_one(zip, len, false, 0, g);
	free(zip);
	return ok;
}

#define ALL 0xFFFFFFFFu		/* the stream's own compressed size */

/* Never more than the member states, and every way a stream fails. */
static void test_failures(void)
{
	static const uint32_t over[][2] = {
		{ 100, 99 }, { 40000, 39999 }, { 70000, 32768 },
		{ 40000, 32767 }, { BIG, BIG - 1 },
	};
	static const uint32_t shortn[] = { 1, 32768, 40000 };
	struct got g;
	uint8_t bad[64];
	struct bits b = { bad, 0, 0, 0 };
	struct zmember m = { "m", 8, bad, 0, 0, 0, 0 };
	size_t len;
	uint8_t *zip;
	bool ok;

	/* a stream that inflates to more than the stated size: its first
	 * stated bytes reach the sink and no more, and the rest is not read,
	 * as minizip did not; the CRC-32 and the length decide */
	for (size_t i = 0; i < N(over); i++) {
		ok = extract_stated(8, over[i][0], ALL, over[i][1], 0, &g);
		CHECK(ok && g.len == over[i][1] &&
		      !memcmp(g.buf, raw_data, over[i][1]),
		      "%u bytes stated %u: ok %d, %zu to the sink", over[i][0],
		      over[i][1], ok, g.len);
	}
	/* stored: the bytes beyond the stated size are not looked at, as
	 * minizip did not; the CRC-32 and the length decide */
	ok = extract_stated(0, 1000, ALL, 999, 0, &g);
	CHECK(ok && g.len == 999, "stored, 1000 bytes stated 999: %d %zu", ok,
	      g.len);
	/* stated empty: not inflated, as minizip did not */
	ok = extract_stated(8, 10, ALL, 0, 0, &g);
	CHECK(ok && g.len == 0, "deflated, stated empty: %d %zu", ok, g.len);
	ok = extract_stated(8, 0, 0, 0, 0, &g);
	CHECK(ok && g.len == 0, "deflated, no data, stated empty: %d %zu", ok,
	      g.len);
	/* a stream that ends short of the stated size, stored or deflated */
	for (size_t i = 0; i < N(shortn); i++) {
		ok = extract_stated(8, shortn[i], ALL, shortn[i] + 1, 0, &g);
		CHECK(!ok && g.len == shortn[i], "deflated %u bytes stated %u: %d",
		      shortn[i], shortn[i] + 1, ok);
		ok = extract_stated(0, shortn[i], ALL, shortn[i] + 1, 0, &g);
		CHECK(!ok, "stored %u bytes stated %u: %d", shortn[i],
		      shortn[i] + 1, ok);
	}
	/* the compressed bytes run out before the stream ends */
	ok = extract_stated(8, BIG, 0, BIG, 0, &g);
	CHECK(!ok && g.len == 0, "deflated, no compressed bytes: %d", ok);
	fill(raw_data, BIG, 5u);
	ok = extract_stated(8, BIG, (uint32_t)(deflate_fixed(raw_data, BIG, comp) - 10),
			    BIG, 0, &g);
	CHECK(!ok && g.len < BIG, "deflated, compressed size 10 short: %d", ok);
	/* a piece that runs past the end of the zip fails, as minizip's read
	 * did, even when the stream ends before it */
	ok = extract_stated(8, 100, 100000, 100, 0, &g);
	CHECK(!ok && g.len == 0, "compressed size past the zip's end: %d", ok);
	/* a sink that fails stops the extraction there */
	for (size_t k = 1; k <= 3; k += 2) {
		ok = extract_stated(8, BIG, ALL, BIG, k, &g);
		CHECK(!ok && g.calls == k, "deflated, sink fails at call %zu: %d %zu",
		      k, ok, g.calls);
		ok = extract_stated(0, BIG, ALL, BIG, k, &g);
		CHECK(!ok && g.calls == k, "stored, sink fails at call %zu: %d %zu",
		      k, ok, g.calls);
	}
	/* corrupt streams: a reserved block type; a distance before the
	 * start, after ten bytes of output */
	put_bits(&b, 1, 1);
	put_bits(&b, 3, 2);
	put_bits(&b, 0, 13);
	m.len = b.n;
	m.csize = (uint32_t)m.len;
	m.usize = 5;
	zip = make_zip(&m, 1, &len);
	got_init(&g, 0);
	ok = extract_one(zip, len, false, 0, &g);
	CHECK(!ok && g.len == 0, "reserved block type: %d", ok);
	free(zip);
	b.n = 0;
	put_bits(&b, 1, 1);
	put_bits(&b, 1, 2);
	for (int i = 0; i < 10; i++)
		put_sym(&b, 'a');
	put_match(&b, 3, 100);
	put_sym(&b, 256);
	put_bits(&b, 0, 8);
	m.len = b.n;
	m.csize = (uint32_t)m.len;
	m.usize = 13;
	zip = make_zip(&m, 1, &len);
	got_init(&g, 0);
	ok = extract_one(zip, len, false, 0, &g);
	CHECK(!ok && g.len <= 13, "distance too far back: %d", ok);
	free(zip);
}

/*
 * Ten bytes stated, and after them, in the same stream: 0 the end, 1 one
 * more byte, 2 a match from before the start, 3 an invalid distance code,
 * 4 nothing (the input ends inside a code), 5 a stored block with bad
 * lengths. As minizip's read: once the stated bytes are out, decoding goes
 * on only until output is wanted again, and a fault it meets on the way
 * fails the member.
 */
static bool after_stated(int tail, struct got *g)
{
	uint8_t buf[64];
	struct bits b = { buf, 0, 0, 0 };
	struct zmember m = { "m", 8, buf, 0, 0, 10, 0 };
	size_t len;
	uint8_t *zip;
	bool ok;

	put_bits(&b, tail == 5 ? 0 : 1, 1);
	put_bits(&b, 1, 2);
	for (int i = 0; i < 10; i++)
		put_sym(&b, 'a');
	if (tail == 1)
		put_sym(&b, 'b');
	if (tail == 2)
		put_match(&b, 3, 100);
	if (tail == 3) {
		put_sym(&b, 257);
		put_code(&b, 30, 5);
	}
	if (tail != 4)
		put_sym(&b, 256);
	if (tail == 5) {
		put_bits(&b, 1, 1);
		put_bits(&b, 0, 2);
		put_bits(&b, 0, 8 - b.have);
		put_bits(&b, 5, 16);
		put_bits(&b, 0x1234, 16);
	}
	if (b.have != 0)
		put_bits(&b, 0, 8 - b.have);
	m.len = b.n;
	m.csize = (uint32_t)b.n;
	m.crc = wire_crc32(0, (const uint8_t *)"aaaaaaaaaa", 10);
	zip = make_zip(&m, 1, &len);
	got_init(g, 0);
	ok = extract_one(zip, len, false, 0, g);
	free(zip);
	return ok;
}

static void test_after_stated(void)
{
	static const bool want[6] = { true, true, true, false, true, false };

	for (int t = 0; t < 6; t++) {
		struct got g;
		const bool ok = after_stated(t, &g);

		CHECK(ok == want[t] && g.len <= 10 && (!ok || g.len == 10),
		      "after the stated size, case %d: ok %d, %zu to the sink", t,
		      ok, g.len);
	}
}

static bool plenty_free(void *ctx, uint64_t *bytes)
{
	(void)ctx;
	*bytes = 1u << 24;
	return true;
}

/* S-11.030 through stage(): a member whose stated CRC-32 is wrong is
 * refused MEMBER; with it right, the zip passes. */
static void test_crc_refused(void)
{
	for (int c = 0; c < 3; c++) {
		uint8_t *zip;
		size_t len, clen;
		struct zmember m[2];
		struct wire_zip z;
		struct s11_zip_ops ops;
		struct s11_staged_sizes st;
		enum s11_reason r;
		uint8_t *data2 = tl_xmalloc(40000);

		fill(raw_data, BIG, 9u);
		fill(data2, 40000, 10u);
		clen = deflate_fixed(raw_data, BIG, comp);
		m[0] = (struct zmember){ "a.bin", 8, comp, clen, (uint32_t)clen,
					 BIG, wire_crc32(0, raw_data, BIG) };
		m[1] = (struct zmember){ "b.bin", 0, data2, 40000, 40000, 40000,
					 wire_crc32(0, data2, 40000) };
		if (c > 0)
			m[c - 1].crc ^= 1u;
		zip = make_zip(m, 2, &len);
		wire_zip_init(&z, &ops, NULL, zip, len, &Y);
		ops.free_space = plenty_free;
		r = s11_check_zip(&ops, &st);
		wire_zip_close(&z);
		CHECK(r == (c == 0 ? S11_REASON_NONE : S11_REASON_MEMBER),
		      "CRC-32 case %d: %s", c, s11_reason_name(r));
		CHECK(wire_yaffs_staging_reset(&Y), "reset");
		free(zip);
		free(data2);
	}
}

int main(void)
{
	size_t len = (size_t)BLOCKS * 128u * 4096u;
	uint8_t *vol = calloc(1, len);

	if (vol == NULL)
		return 2;
	wb_ramdisk_init(&bd, &ram, vol, (uint32_t)(len / 512u));
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &bd, 0, BLOCKS - 1) &&
	      wire_yaffs_mount(&Y) == WIRE_MOUNT_OK, "mount");
	CHECK(wire_yaffs_mkdir(&Y, "user"), "mkdir user");
	put_file("user/keep", "keep");
	for (size_t i = 0; i < N(s11v_zips); i++)
		check_zip(&s11v_zips[i]);
	test_long_extra();
	raw_data = tl_xmalloc(BIG + 16u);
	comp = tl_xmalloc(BIG * 9u / 8u + 16u);
	sink_buf = tl_xmalloc(BIG + 65536u);
	test_sizes();
	test_order();
	test_failures();
	test_after_stated();
	test_crc_refused();
	free(raw_data);
	free(comp);
	free(sink_buf);
	wire_yaffs_unmount(&Y);
	free(vol);
	return tl_done("test_wire_zip");
}

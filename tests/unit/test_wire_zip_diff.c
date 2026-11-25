/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * src/core/install/zip.c against minizip's own read path (S-11.030), on zips built
 * here and mutants of them from a fixed-seed generator.
 *
 * For every mutant both readers list the members, and every member the
 * install would extract (stored or deflated, not encrypted) is read by both:
 * - the reader, through struct s11_zip_ops as s11_check_zip() calls it: the
 *   member's verdict is stage()'s, the extraction in full to the stated
 *   length and CRC-32 (computed with ops->crc32);
 * - the reference: unzOpenCurrentFile(), unzReadCurrentFile() in 4 KiB reads
 *   to the end and unzCloseCurrentFile(), which inflate and compare the
 *   CRC-32 themselves; the member passes when every call succeeds and the
 *   bytes read are the stated length.
 * Both must give the same verdict for every member and, when they accept,
 * the same bytes. Half the mutants are read in place, through a package's
 * source, as the board reads them.
 */
#define _POSIX_C_SOURCE 200809L
#include <time.h>

#include "testlib.h"

#include "waffle/install/staging.h"
#include "waffle/util/crc.h"
#include "waffle/install/zip.h"

#include "zip_build.h"

#define SEED 0x5EEDC0DE2026ull	/* the generator's start */
#define MUTANTS 40000u		/* mutants read, the seeds themselves first */
#define MAX_MEMBERS 64u		/* members compared in one zip */
#define KEEP 0x40000u		/* bytes of a member compared in full */
#define SLACK 4096u		/* room a mutant has to grow */
#define READ 4096u		/* the reference's reads */
#define MAX_FAILURES 20u

#define N(a) (sizeof(a) / sizeof((a)[0]))

/* ---- the generator ---- */

static uint64_t rng = SEED;

static uint32_t rnd(void)
{
	uint64_t z = (rng += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return (uint32_t)((z ^ (z >> 31)) >> 32);
}

static uint32_t below(size_t n)
{
	return n == 0 ? 0 : (uint32_t)(rnd() % n);
}

/* ---- the seeds ---- */

struct zm {
	const char *name;
	uint16_t method, flags;		/* flag 3: a data descriptor */
	const uint8_t *data;		/* as stored */
	size_t len;
	uint32_t crc, csize, usize;	/* as stated */
	const uint8_t *extra;		/* in both headers */
	size_t xlen;
};

/* The members (at most 8) and a comment, as a zip at z. */
static size_t build(uint8_t *z, const struct zm *m, size_t count,
		    const char *comment)
{
	size_t k = 0, cd, end;
	uint32_t at[8];

	for (size_t i = 0; i < count; i++) {
		const size_t nl = strlen(m[i].name);
		const bool dd = (m[i].flags & 8u) != 0;

		at[i] = (uint32_t)k;
		k += put32(z + k, 0x04034B50u) + put16(z + k + 4, 20);
		k += put16(z + k, m[i].flags) + put16(z + k + 2, m[i].method);
		k += put32(z + k, 0) + put32(z + k + 4, dd ? 0 : m[i].crc);
		k += put32(z + k, dd ? 0 : m[i].csize);
		k += put32(z + k, dd ? 0 : m[i].usize);
		k += put16(z + k, (unsigned)nl) +
		     put16(z + k + 2, (unsigned)m[i].xlen);
		memcpy(z + k, m[i].name, nl);
		k += nl;
		if (m[i].xlen != 0)
			memcpy(z + k, m[i].extra, m[i].xlen);
		k += m[i].xlen;
		if (m[i].len != 0)
			memcpy(z + k, m[i].data, m[i].len);
		k += m[i].len;
		if (dd) {
			k += put32(z + k, 0x08074B50u) + put32(z + k + 4, m[i].crc);
			k += put32(z + k, m[i].csize) + put32(z + k + 4, m[i].usize);
		}
	}
	cd = k;
	for (size_t i = 0; i < count; i++) {
		const size_t nl = strlen(m[i].name);

		k += put32(z + k, 0x02014B50u) + put16(z + k + 4, 20) +
		     put16(z + k + 6, 20);
		k += put16(z + k, m[i].flags) + put16(z + k + 2, m[i].method);
		k += put32(z + k, 0) + put32(z + k + 4, m[i].crc);
		k += put32(z + k, m[i].csize) + put32(z + k + 4, m[i].usize);
		k += put16(z + k, (unsigned)nl) +
		     put16(z + k + 2, (unsigned)m[i].xlen);
		k += put16(z + k, 0) + put16(z + k + 2, 0) + put16(z + k + 4, 0);
		k += put32(z + k, 0) + put32(z + k + 4, at[i]);
		memcpy(z + k, m[i].name, nl);
		k += nl;
		if (m[i].xlen != 0)
			memcpy(z + k, m[i].extra, m[i].xlen);
		k += m[i].xlen;
	}
	end = k;
	k += put32(z + k, 0x06054B50u) + put16(z + k + 4, 0) +
	     put16(z + k + 6, 0);
	k += put16(z + k, (unsigned)count) + put16(z + k + 2, (unsigned)count);
	k += put32(z + k, (uint32_t)(end - cd)) + put32(z + k + 4, (uint32_t)cd);
	k += put16(z + k, (unsigned)strlen(comment));
	memcpy(z + k, comment, strlen(comment));
	return k + strlen(comment);
}

struct seed {
	uint8_t *zip;
	size_t len;
};

static struct seed seeds[16];
static size_t nseeds;

/* Plain bytes, kept for the seeds' members; each stays allocated. */
static uint8_t *plain(size_t n, uint32_t s)
{
	uint8_t *p = tl_xmalloc(n);

	fill(p, n, s);
	return p;
}

/* A deflated member of n bytes of fill(s): blocks of `block` bytes,
 * fixed-code and stored in turn, or one fixed-code block (block 0). */
static struct zm deflated(const char *name, size_t n, uint32_t s, size_t block)
{
	const uint8_t *p = plain(n, s);
	uint8_t *c = tl_xmalloc(n * 9 / 8 + 16 + 5 * (n / 512 + 1));
	struct zm m = { name, 8, 0, c, 0, 0, 0, (uint32_t)n, NULL, 0 };

	m.len = block == 0 ? deflate_fixed(p, n, c) : deflate_mixed(p, n, block, c);
	m.csize = (uint32_t)m.len;
	m.crc = wire_crc32(0, p, (uint32_t)n);
	return m;
}

static struct zm stored(const char *name, size_t n, uint32_t s)
{
	const uint8_t *p = plain(n, s);
	struct zm m = { name, 0, 0, p, n, wire_crc32(0, p, (uint32_t)n),
			(uint32_t)n, (uint32_t)n, NULL, 0 };

	return m;
}

static void add_seed(const struct zm *m, size_t count, const char *comment)
{
	size_t cap = 22 + strlen(comment);
	struct seed *s = &seeds[nseeds++];

	for (size_t i = 0; i < count; i++)
		cap += 46 + 30 + 16 + 2 * (strlen(m[i].name) + m[i].xlen) +
		       m[i].len;
	s->zip = tl_xmalloc(cap);
	s->len = build(s->zip, m, count, comment);
	CHECK(s->len <= cap, "seed %zu: %zu bytes in %zu", nseeds, s->len, cap);
}

static void make_seeds(void)
{
	static const uint8_t empty_stream[2] = { 0x03, 0x00 };
	static const uint8_t extra[12] = { 0xFE, 0xCA, 8, 0, 1, 2, 3, 4, 5, 6,
					   7, 8 };
	struct zm m[8];

	/* a stored and a deflated member */
	m[0] = stored("a.txt", 100, 1);
	m[1] = deflated("b.bin", 5000, 2, 0);
	add_seed(m, 2, "");
	/* a directory and empty members: stored, deflated to an empty
	 * stream, and deflated with no data at all */
	m[0] = (struct zm){ "dir/", 0, 0, NULL, 0, 0, 0, 0, NULL, 0 };
	m[1] = (struct zm){ "e0", 0, 0, NULL, 0, 0, 0, 0, NULL, 0 };
	m[2] = (struct zm){ "e1", 8, 0, empty_stream, 2, 0, 2, 0, NULL, 0 };
	m[3] = (struct zm){ "e2", 8, 0, NULL, 0, 0, 0, 0, NULL, 0 };
	add_seed(m, 4, "");
	/* larger than the 32 KiB window and minizip's 16 KiB reads */
	m[0] = deflated("big", 40000, 3, 0);
	m[1] = stored("s", 20000, 4);
	add_seed(m, 2, "");
	/* several members under a directory; stored and fixed-code blocks */
	m[0] = (struct zm){ "x/", 0, 0, NULL, 0, 0, 0, 0, NULL, 0 };
	m[1] = deflated("x/1", 3000, 5, 700);
	m[2] = stored("x/2", 10, 6);
	m[3] = deflated("x/3", 1, 7, 0);
	m[4] = deflated("y", 9000, 8, 0);
	add_seed(m, 5, "");
	/* data descriptors, deflated and stored */
	m[0] = deflated("dd", 6000, 9, 0);
	m[0].flags = 8;
	m[1] = stored("dd2", 300, 10);
	m[1].flags = 8;
	add_seed(m, 2, "");
	/* a wrong CRC-32, deflated and stored */
	m[0] = deflated("c", 2000, 11, 0);
	m[0].crc ^= 1u;
	m[1] = stored("c2", 50, 12);
	m[1].crc ^= 0x80000000u;
	add_seed(m, 2, "");
	/* stated longer than the data: the stream ends first */
	m[0] = deflated("long", 2000, 13, 0);
	m[0].usize++;
	m[1] = stored("long2", 500, 14);
	m[1].usize++;
	add_seed(m, 2, "");
	/* stated shorter: a stream that inflates to more than the stated
	 * size, stored data beyond it */
	m[0] = deflated("short", 3000, 15, 0);
	m[0].usize--;
	m[1] = stored("short2", 100, 16);
	m[1].usize = 99;
	add_seed(m, 2, "");
	/* a stream cut short: its compressed size and data end early */
	m[0] = deflated("cut", 4000, 17, 0);
	m[0].len -= 7;
	m[0].csize = (uint32_t)m[0].len;
	add_seed(m, 1, "");
	/* bytes after the end of the stream, inside the compressed size */
	m[0] = deflated("tail", 1500, 18, 0);
	{
		uint8_t *c = tl_xmalloc(m[0].len + 16);

		memcpy(c, m[0].data, m[0].len);
		for (unsigned i = 0; i < 16; i++)
			c[m[0].len + i] = (uint8_t)(0xA5u ^ i * 37u);
		m[0].data = c;
		m[0].len += 16;
		m[0].csize = (uint32_t)m[0].len;
	}
	add_seed(m, 1, "");
	/* extra fields and an archive comment */
	m[0] = deflated("ex", 700, 19, 0);
	m[0].extra = extra;
	m[0].xlen = sizeof(extra);
	m[1] = stored("ex2", 64, 20);
	m[1].extra = extra;
	m[1].xlen = sizeof(extra);
	add_seed(m, 2, "seed");
	/* a longer stream of fixed-code and stored blocks */
	m[0] = deflated("mixed", 20000, 21, 4096);
	add_seed(m, 1, "");
}

/* ---- the two readers ---- */

/* What a reader gave for one member. */
struct result {
	bool ok;
	uint64_t len;
	uint32_t crc;
	uint8_t *keep;			/* its first KEEP bytes */
};

static void result_add(struct result *r, uint32_t crc, const uint8_t *p,
		       size_t n)
{
	if (r->len < KEEP)
		memcpy(r->keep + r->len, p, n < KEEP - r->len ? n : KEEP - r->len);
	r->len += n;
	r->crc = crc;
}

struct tree_sink {
	const struct s11_zip_ops *ops;
	struct result *r;
};

static bool tree_sink(void *arg, const uint8_t *p, size_t n)
{
	const struct tree_sink *t = arg;

	result_add(t->r, t->ops->crc32(t->ops->ctx, t->r->crc, p, n), p, n);
	return true;
}

/* The zip read in place through a package's source, at offset ZIP_AT. */
#define ZIP_AT 0x40u

struct src_zip {
	const uint8_t *zip;
	size_t len;
};

static bool src_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	const struct src_zip *s = ctx;

	if (off < ZIP_AT || off - ZIP_AT > s->len || n > s->len - (off - ZIP_AT))
		return false;
	memcpy(buf, s->zip + (off - ZIP_AT), n);
	return true;
}

static struct src_zip src_zip;
static struct wire_package src_pkg;

static void tree_open(struct wire_zip *z, struct s11_zip_ops *ops,
		      const uint8_t *zip, size_t len, bool in_place)
{
	if (!in_place) {
		wire_zip_init(z, ops, NULL, zip, len, NULL);
		return;
	}
	memset(&src_pkg, 0, sizeof(src_pkg));
	src_zip.zip = zip;
	src_zip.len = len;
	src_pkg.src.read = src_read;
	src_pkg.src.ctx = &src_zip;
	src_pkg.zip_len = len;
	src_pkg.zip_at = ZIP_AT;
	wire_zip_init(z, ops, &src_pkg, NULL, 0, NULL);
}

/* stage()'s verdict on member i (S-11.030). */
static void tree_extract(const struct s11_zip_ops *ops, size_t i,
			 const struct s11_member *m, struct result *r)
{
	struct tree_sink t = { ops, r };
	bool ok;

	r->len = 0;
	r->crc = 0;
	ok = ops->extract(ops->ctx, i, tree_sink, &t);
	r->ok = ok && r->len == m->size && r->crc == m->crc32;
}

/* The member minizip is at, as zip_member() asks for it. */
static bool ref_member(unzFile uf, struct s11_member *m, unz_file_info64 *info)
{
	static uint8_t extra[WIRE_ZIP_EXTRA_MAX];

	if (unzGetCurrentFileInfo64(uf, info, m->name, sizeof(m->name), extra,
				    sizeof(extra), NULL, 0) != UNZ_OK)
		return false;
	if (info->size_filename >= sizeof(m->name))
		m->name[0] = '\0';
	else
		m->name[info->size_filename] = '\0';
	m->method = (uint16_t)info->compression_method;
	m->flags = (uint16_t)info->flag;
	m->size = (uint32_t)info->uncompressed_size;
	m->crc32 = (uint32_t)info->crc;
	return true;
}

/* minizip's own read of the member it is at. */
static void ref_extract(unzFile uf, const unz_file_info64 *info,
			struct result *r)
{
	static uint8_t buf[READ];
	int n = 0;

	r->ok = false;
	r->len = 0;
	r->crc = 0;
	if (unzOpenCurrentFile(uf) != UNZ_OK)
		return;
	while ((n = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0)
		result_add(r, wire_crc32(r->crc, buf, (uint32_t)n), buf, (size_t)n);
	r->ok = unzCloseCurrentFile(uf) == UNZ_OK && n == 0 &&
		r->len == info->uncompressed_size;
}

/* ---- the comparison ---- */

struct stats {
	unsigned long zips, opened, members, compared, accepted, refused;
};

static struct result tree_r, ref_r;

static bool same_member(const struct s11_member *a, const struct s11_member *b)
{
	return !strcmp(a->name, b->name) && a->method == b->method &&
	       a->flags == b->flags && a->size == b->size &&
	       a->crc32 == b->crc32;
}

/*
 * Both readers on the zip: the same members, and for each one the install
 * would extract, the same verdict and, when it passes, the same bytes.
 * Returns false at the first difference.
 */
static bool zip_diff(const uint8_t *zip, size_t len, bool in_place,
		     struct stats *st)
{
	struct wire_zip z;
	struct s11_zip_ops ops;
	struct wb_unzip_mem mem;
	unzFile uf;
	bool same = true;
	size_t i;

	st->zips++;
	tree_open(&z, &ops, zip, len, in_place);
	uf = wb_unzip_open_mem(&mem, zip, len);
	st->opened += uf != NULL;
	for (i = 0; same && i < MAX_MEMBERS; i++) {
		struct s11_member tm, rm;
		unz_file_info64 info;
		const bool th = ops.member(ops.ctx, i, &tm);
		const bool rh = uf != NULL &&
				(i == 0 ? unzGoToFirstFile(uf) : unzGoToNextFile(uf)) ==
					UNZ_OK &&
				ref_member(uf, &rm, &info);

		same = th == rh && (!th || same_member(&tm, &rm));
		if (!same || !th)
			break;
		st->members++;
		if ((tm.method != 0 && tm.method != 8) || (tm.flags & 1u) != 0)
			continue;
		tree_extract(&ops, i, &tm, &tree_r);
		ref_extract(uf, &info, &ref_r);
		st->compared++;
		same = tree_r.ok == ref_r.ok &&
		       (!tree_r.ok ||
			(tree_r.len == ref_r.len && tree_r.crc == ref_r.crc &&
			 !memcmp(tree_r.keep, ref_r.keep,
				 tree_r.len < KEEP ? tree_r.len : KEEP)));
		if (same)
			*(tree_r.ok ? &st->accepted : &st->refused) += 1;
		else
			printf("  member %zu (%s): reader %s %llu bytes, minizip %s %llu bytes\n",
			       i, tm.name, tree_r.ok ? "accepts" : "refuses",
			       (unsigned long long)tree_r.len,
			       ref_r.ok ? "accepts" : "refuses",
			       (unsigned long long)ref_r.len);
	}
	if (uf != NULL)
		(void)unzClose(uf);
	wire_zip_close(&z);
	return same;
}

/* ---- the mutators ---- */

static uint32_t get(const uint8_t *p, unsigned w)
{
	return w == 2 ? (uint32_t)p[0] | (uint32_t)p[1] << 8 :
			(uint32_t)p[0] | (uint32_t)p[1] << 8 |
				(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t new_value(uint32_t cur, unsigned w)
{
	static const uint32_t odd[] = { 1, 8, 0x7FFF, 0x8000, 0x4000,
					0x10000, 0x7FFFFFFF, 0x80000000 };
	const uint32_t max = w == 2 ? 0xFFFFu : 0xFFFFFFFFu;
	uint32_t v;

	switch (below(8)) {
	case 0: v = 0; break;
	case 1: v = cur + 1; break;
	case 2: v = cur - 1; break;
	case 3: v = cur ^ 1u << below(8 * w); break;
	case 4: v = max; break;
	case 5: v = cur + below(64) - 32; break;
	case 6: v = rnd(); break;
	default: v = odd[below(N(odd))]; break;
	}
	return v & max;
}

struct field {
	uint8_t off, w;
};

/* A field of a local header, central header, end record or data
 * descriptor, found by its signature, set to a new value. */
static void edit_field(uint8_t *z, size_t len)
{
	static const struct field local[] = { { 4, 2 }, { 6, 2 }, { 8, 2 },
		{ 14, 4 }, { 18, 4 }, { 22, 4 }, { 26, 2 }, { 28, 2 } };
	static const struct field central[] = { { 6, 2 }, { 8, 2 }, { 10, 2 },
		{ 16, 4 }, { 20, 4 }, { 24, 4 }, { 28, 2 }, { 30, 2 },
		{ 32, 2 }, { 34, 2 }, { 42, 4 } };
	static const struct field eocd[] = { { 4, 2 }, { 6, 2 }, { 8, 2 },
		{ 10, 2 }, { 12, 4 }, { 16, 4 }, { 20, 2 } };
	static const struct field desc[] = { { 4, 4 }, { 8, 4 }, { 12, 4 } };
	static const struct {
		uint8_t sig;
		const struct field *f;
		size_t n;
	} kinds[] = { { 3, local, N(local) }, { 1, central, N(central) },
		      { 5, eocd, N(eocd) }, { 7, desc, N(desc) } };
	size_t at[64], n = 0;
	const unsigned k = below(N(kinds));
	struct field f;
	uint32_t v;

	for (size_t i = 0; i + 4 <= len && n < N(at); i++) {
		if (z[i] == 'P' && z[i + 1] == 'K' && z[i + 2] == kinds[k].sig &&
		    z[i + 3] == kinds[k].sig + 1)
			at[n++] = i;
	}
	if (n == 0)
		return;
	f = kinds[k].f[below(kinds[k].n)];
	n = at[below(n)];
	if (n + f.off + f.w > len)
		return;
	v = new_value(get(z + n + f.off, f.w), f.w);
	if (f.w == 2)
		put16(z + n + f.off, v);
	else
		put32(z + n + f.off, v);
}

static void mutate(uint8_t *z, size_t *len, size_t cap)
{
	size_t a, n, p;

	switch (below(10)) {
	case 0:			/* a bit flipped */
		if (*len != 0)
			z[below(*len)] ^= (uint8_t)(1u << below(8));
		break;
	case 1:			/* a byte overwritten */
		if (*len != 0)
			z[below(*len)] = below(4) == 0 ? (uint8_t)rnd() :
					 (uint8_t)(below(2) ? 0xFF : 0x00);
		break;
	case 2:			/* the end cut off */
		*len = below(4) == 0 ? below(*len + 1) :
				       *len - below(*len < 64 ? *len + 1 : 64);
		break;
	case 3:
	case 4:
	case 5:
		edit_field(z, *len);
		break;
	case 6:			/* bytes of a seed spliced over these */
	{
		const struct seed *s = &seeds[below(nseeds)];

		n = 1 + below(s->len < 512 ? s->len : 512);
		a = below(s->len - n + 1);
		p = below(*len + 1);
		if (p + n > cap)
			n = cap - p;
		memcpy(z + p, s->zip + a, n);
		if (p + n > *len)
			*len = p + n;
		break;
	}
	case 7:			/* bytes inserted */
		n = 1 + below(64);
		p = below(*len + 1);
		if (*len + n > cap)
			break;
		memmove(z + p + n, z + p, *len - p);
		for (size_t i = 0; i < n; i++)
			z[p + i] = (uint8_t)rnd();
		*len += n;
		break;
	case 8:			/* bytes removed */
		if (*len == 0)
			break;
		p = below(*len);
		n = 1 + below(*len - p < 64 ? *len - p : 64);
		memmove(z + p, z + p + n, *len - p - n);
		*len -= n;
		break;
	default:		/* a range of the zip copied over another */
		if (*len < 2)
			break;
		n = 1 + below(*len / 2);
		a = below(*len - n + 1);
		p = below(*len - n + 1);
		memmove(z + p, z + a, n);
		break;
	}
}

static void test_differential(void)
{
	struct stats st = { 0, 0, 0, 0, 0, 0 };
	size_t cap = 0;
	uint8_t *z;
	unsigned failed = 0;
	const clock_t t0 = clock();

	make_seeds();
	for (size_t s = 0; s < nseeds; s++)
		cap = seeds[s].len > cap ? seeds[s].len : cap;
	cap += SLACK;
	z = tl_xmalloc(cap);
	tree_r.keep = tl_xmalloc(KEEP);
	ref_r.keep = tl_xmalloc(KEEP);
	for (unsigned k = 0; k < MUTANTS && failed < MAX_FAILURES; k++) {
		const size_t s = k < 2 * nseeds ? k / 2 : below(nseeds);
		const unsigned muts = k < 2 * nseeds ? 0 : 1 + below(4);
		size_t len = seeds[s].len;

		memcpy(z, seeds[s].zip, len);
		for (unsigned i = 0; i < muts; i++)
			mutate(z, &len, cap);
		if (!zip_diff(z, len, k % 2 != 0, &st)) {
			failed++;
			CHECK(false, "mutant %u (seed %zu, %u mutations, %s): the readers differ",
			      k, s, muts, k % 2 ? "in place" : "in RAM");
		}
	}
	printf("  %lu zips (%lu opened), %lu members, %lu read by both: %lu accepted, %lu refused, %.1f s\n",
	       st.zips, st.opened, st.members, st.compared, st.accepted,
	       st.refused, (double)(clock() - t0) / CLOCKS_PER_SEC);
	/* the mutants reach both verdicts often */
	CHECK(st.accepted > MUTANTS / 4 && st.refused > MUTANTS / 8,
	      "%lu accepted, %lu refused", st.accepted, st.refused);
	free(z);
	free(tree_r.keep);
	free(ref_r.keep);
}

int main(void)
{
	test_differential();
	return tl_done("test_wire_zip_diff");
}

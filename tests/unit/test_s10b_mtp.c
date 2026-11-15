/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10b: the MTP responder against tests/oracle/S-10b-mtp/vectors (level R), over
 * an in-memory volume holding the vectors' trees, and its container framing
 * (S-10b.001 to S-10b.007) at both packet sizes.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "waffle/s10b_mtp.h"

#include "support/check.h"
#include "s15_sha256.h"
#include "vectors/test_s10b_vectors.h"

#define BIG_FILES 5000u
#define MAX_OBJECTS 6000u
#define MANY 70000u	/* past the handles 0x0001xxxx (S-10b.012) */

/* --- The volume ------------------------------------------------------------ */

struct file {
	const uint8_t *path;
	size_t len;
	uint8_t kind;		/* 0 file, 1 folder */
	uint32_t size, mtime, ctime;
	int seed;
	const uint8_t *data;
};

static struct file files[64 + MANY];
static size_t n_files;
static char big_paths[BIG_FILES][12];
static char many_paths[MANY][8];

static struct {
	const struct v_volume *v;
	/* Failure injection: reads of fail_path fail from fail_at on. */
	const char *fail_path;
	uint32_t fail_at;
	unsigned battery;
	uint32_t clock;
	unsigned lists;
} vol;

static void load_volume(unsigned k)
{
	const struct v_volume *v = &v_volumes[k];
	size_t i;

	vol.v = v;
	vol.fail_path = NULL;
	n_files = 0;
	for (i = 0; i < v->n; i++) {
		const struct v_entry *e = &v_entries[v->first + i];
		struct file *f = &files[n_files++];

		f->path = (const uint8_t *)v_paths[e->path];
		f->len = strlen(v_paths[e->path]);
		f->kind = e->kind;
		f->size = e->size;
		f->mtime = e->mtime;
		f->ctime = e->ctime;
		f->seed = e->seed;
		f->data = v_blob + e->data.off;
	}
	for (i = 0; i < v->big; i++) {
		struct file *f = &files[n_files++];

		(void)snprintf(big_paths[i], sizeof(big_paths[i]), "/big/f%04u",
			       (unsigned)(i % 10000u));
		memset(f, 0, sizeof(*f));
		f->path = (const uint8_t *)big_paths[i];
		f->len = strlen(big_paths[i]);
		f->seed = -1;
	}
}

static uint8_t content(const struct file *f, uint32_t i)
{
	return f->seed < 0 ? f->data[i] :
	       (uint8_t)((7u * i + 3u + (unsigned)f->seed) & 0xFFu);
}

/* The length of P's folder part: "/a" of "/a/b", 0 for "/a". */
static size_t parent_len(const struct file *f)
{
	size_t n = f->len;

	while (n > 0 && f->path[n - 1u] != '/')
		n--;
	return n - 1u;
}

static bool vol_space(void *ctx, uint64_t *total, uint64_t *free_bytes)
{
	(void)ctx;
	if (!vol.v->present)
		return false;
	*total = vol.v->total;
	*free_bytes = vol.v->free;
	return true;
}

static bool vol_list(void *ctx, const char *path, s10b_entry_fn fn,
		     void *arg)
{
	const size_t plen = strcmp(path, "/") == 0 ? 0 : strlen(path);
	size_t i;

	(void)ctx;
	vol.lists++;
	for (i = 0; i < n_files; i++) {
		const struct file *f = &files[i];
		struct s10b_entry e;

		if (parent_len(f) != plen || memcmp(f->path, path, plen) != 0)
			continue;
		e.name = f->path + plen + 1u;
		e.name_len = f->len - plen - 1u;
		e.kind = f->kind ? S10B_KIND_DIR : S10B_KIND_FILE;
		e.size = f->size;
		e.mtime = f->mtime;
		e.ctime = f->ctime;
		fn(arg, &e);
	}
	return true;
}

static const struct file *find(const char *path)
{
	size_t i;

	for (i = 0; i < n_files; i++)
		if (files[i].len == strlen(path) &&
		    memcmp(files[i].path, path, files[i].len) == 0)
			return &files[i];
	return NULL;
}

static long vol_read(void *ctx, const char *path, uint32_t offset,
		     uint8_t *buf, size_t n)
{
	const struct file *f = find(path);
	uint32_t i;

	(void)ctx;
	if (!f || f->kind)
		return -1;
	if (vol.fail_path && strcmp(path, vol.fail_path) == 0 &&
	    offset + n >= vol.fail_at)
		return -1;
	if (offset >= f->size)
		return 0;
	if (n > f->size - offset)
		n = f->size - offset;
	for (i = 0; i < n; i++)
		buf[i] = content(f, offset + i);
	return (long)n;
}

static const struct s10b_volume_ops vol_ops = {
	.space = vol_space, .list = vol_list, .read = vol_read,
};

static unsigned dev_battery(void *ctx)
{
	(void)ctx;
	return vol.battery;
}

static uint32_t dev_clock_read(void *ctx)
{
	(void)ctx;
	return vol.clock;
}

static void dev_clock_write(void *ctx, uint32_t count)
{
	(void)ctx;
	vol.clock = count;
}

static const struct s10b_device_ops dev_ops = {
	.battery = dev_battery,
	.clock_read = dev_clock_read,
	.clock_write = dev_clock_write,
};

/* --- The engine -------------------------------------------------------------- */

static struct s10b m;
static struct s10b_object objects[MAX_OBJECTS];
static uint8_t names[MAX_OBJECTS * 16u];
static uint32_t tid;

static void engine(unsigned volume, size_t max_objects, size_t mps)
{
	const struct s10b_config cfg = {
		.vol = &vol_ops, .dev = &dev_ops,
		.manufacturer = v_strings[0], .model = v_strings[1],
		.version = v_strings[2], .serial = v_strings[3],
		.objects = objects, .max_objects = max_objects,
		.names = names, .names_size = sizeof(names),
	};

	load_volume(volume);
	s10b_init(&m, &cfg);
	s10b_reset(&m, mps);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static unsigned get16(const uint8_t *p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static void header(uint8_t *p, uint32_t len, unsigned type, unsigned code,
		   uint32_t id)
{
	put32(p, len);
	p[4] = (uint8_t)type;
	p[5] = 0;
	p[6] = (uint8_t)code;
	p[7] = (uint8_t)(code >> 8);
	put32(p + 8, id);
}

/* Bytes to the device as the host sends them: transfers of what the device
 * arms, `split` bytes at most in the first. */
static void send(const uint8_t *p, size_t n, size_t split)
{
	size_t first = 1;

	while (n) {
		size_t k = s10b_rx_size(&m);

		CHECK(k != 0);
		if (k == 0)
			return;
		if (first && split && k > split)
			k = split;
		if (k > n)
			k = n;
		s10b_rx(&m, p, k);
		p += k;
		n -= k;
		first = 0;
	}
}

struct reply {
	unsigned code, n;
	uint32_t params[5];
	bool data;
	size_t len;		/* dataset bytes */
	bool zlp;		/* the data phase ended with a zero-length packet */
	bool short_end;		/* it ended before its length */
	unsigned transfers;
};

static uint8_t data[2u << 20];
static uint8_t data_out[(1u << 20) + 64u];

/* The device's answer: a data container, maybe, then the response. */
static void receive(unsigned op, struct reply *r)
{
	static uint8_t xfer[512];
	uint32_t total = 0;
	size_t n, got = 0;
	unsigned k;
	bool in_data = false;

	memset(r, 0, sizeof(*r));
	for (;;) {
		if (!s10b_tx(&m, xfer, sizeof(xfer), &n)) {
			CHECK(0);
			return;
		}
		r->transfers++;
		if (!in_data && n >= 12 && get16(xfer + 4) == 2) {
			/* S-10b.001: the command's code and transaction. */
			CHECK(get16(xfer + 6) == op && get32(xfer + 8) == tid);
			total = get32(xfer);
			in_data = true;
			r->data = true;
		} else if (!in_data) {
			break;
		}
		if (got + n > sizeof(data)) {
			CHECK(0);
			return;
		}
		memcpy(data + got, xfer, n);
		got += n;
		if (n == 0)
			r->zlp = true;
		if (n % m.mps != 0 || n == 0 || got >= total) {
			r->short_end = got < total;
			/* A full last packet is followed by a ZLP (S-10b.003). */
			if (n != 0 && n % m.mps == 0 && got >= total) {
				CHECK(s10b_tx(&m, xfer, sizeof(xfer), &n) &&
				      n == 0);
				r->zlp = true;
			}
			if (!s10b_tx(&m, xfer, sizeof(xfer), &n)) {
				CHECK(0);
				return;
			}
			break;
		}
	}
	/* The response, a transfer of its own. */
	CHECK(n >= 12 && n == get32(xfer) && get16(xfer + 4) == 3 &&
	      get32(xfer + 8) == tid && (n - 12) % 4 == 0 && n <= 32);
	r->code = get16(xfer + 6);
	r->n = (unsigned)(n - 12) / 4u;
	for (k = 0; k < r->n && k < 5; k++)
		r->params[k] = get32(xfer + 12 + 4 * k);
	if (r->data) {
		r->len = got >= 12 ? got - 12 : 0;
		memmove(data, data + 12, r->len);
	}
	CHECK(!s10b_tx(&m, xfer, sizeof(xfer), &n));
}

/* One transaction; out (n bytes) is the host data phase when not NULL. */
static void txn(unsigned op, const uint32_t *p, unsigned np,
		const uint8_t *out, size_t n, size_t split, struct reply *r)
{
	uint8_t cmd[32];
	unsigned i;

	tid++;
	header(cmd, 12u + 4u * np, 1, op, tid);
	for (i = 0; i < np; i++)
		put32(cmd + 12 + 4 * i, p[i]);
	send(cmd, 12u + 4u * np, 0);
	if (out) {
		header(data_out, (uint32_t)(12u + n), 2, op, tid);
		memcpy(data_out + 12, out, n);
		send(data_out, 12u + n, split);
	}
	receive(op, r);
}

static unsigned op0(unsigned op, uint32_t a)
{
	struct reply r;

	txn(op, &a, 1, NULL, 0, 0, &r);
	return r.code;
}

/* --- Handles bound by path ----------------------------------------------- */

static uint32_t bound[ARRAY_LEN(v_paths)];

/* The handle bound to one of the vectors' paths. */
static uint32_t handle_of(const char *path)
{
	size_t i;

	for (i = 0; i < ARRAY_LEN(v_paths); i++)
		if (strcmp(v_paths[i], path) == 0)
			return bound[i];
	CHECK(0);
	return 0;
}

static size_t object_path(size_t i, char *out)
{
	const struct s10b_object *o = &objects[i];
	size_t n = o->parent ? object_path(o->parent - 1u, out) : 0;

	out[n++] = '/';
	memcpy(out + n, names + o->name, o->name_len);
	n += o->name_len;
	out[n] = '\0';
	return n;
}

static void bind(void)
{
	char path[S10B_PATH_MAX + 256];
	size_t i, k;

	memset(bound, 0, sizeof(bound));
	for (i = 0; i < m.n_objects; i++) {
		(void)object_path(i, path);
		for (k = 0; k < ARRAY_LEN(v_paths); k++)
			if (strcmp(path, v_paths[k]) == 0)
				bound[k] = s10b_handle(i);
	}
}

static uint32_t param(const struct v_param *p)
{
	if (!p->handle)
		return p->v;
	CHECK(bound[p->v] != 0);
	return bound[p->v];
}

static int path_of_handle(uint32_t h)
{
	size_t k;

	for (k = 0; k < ARRAY_LEN(v_paths); k++)
		if (bound[k] == h)
			return (int)k;
	return -1;
}

static bool same_bytes(const struct v_bytes *b, const uint8_t *got,
		       size_t n)
{
	static uint8_t want[4096];
	unsigned i;

	if (n != b->b.len || n > sizeof(want)) {
		printf("  %u bytes want %u\n", (unsigned)n, (unsigned)b->b.len);
		return false;
	}
	memcpy(want, v_blob + b->b.off, n);
	for (i = 0; i < b->n_patch; i++) {
		const struct v_patch *p = &v_patches[b->patch + i];

		put32(want + p->off, bound[p->path]);
	}
	if (memcmp(want, got, n) != 0) {
		for (i = 0; i < n && want[i] == got[i]; i++)
			;
		printf("  bytes differ at %u of %u: %02X want %02X\n", i,
		       (unsigned)n, got[i], want[i]);
		return false;
	}
	return true;
}

static int cmp16(const void *a, const void *b)
{
	const int x = *(const int *)a, y = *(const int *)b;

	return (x > y) - (x < y);
}

/* The handles of `n` u32 at p as sorted path indices, against the set. */
static bool same_set(const struct v_step *s, const uint8_t *p, size_t n,
		     size_t stride)
{
	static int got[BIG_FILES + 64], want[BIG_FILES + 64];
	size_t i;

	if (n != s->n)
		return false;
	for (i = 0; i < n; i++) {
		got[i] = path_of_handle(get32(p + i * stride));
		want[i] = (int)v_sets[s->first + i];
	}
	qsort(got, n, sizeof(got[0]), cmp16);
	qsort(want, n, sizeof(want[0]), cmp16);
	return memcmp(got, want, n * sizeof(got[0])) == 0;
}

static size_t value_len(unsigned type, const uint8_t *p)
{
	switch (type) {
	case 0x0004:
		return 2;
	case 0x0006:
		return 4;
	case 0x0008:
		return 8;
	case 0x000A:
		return 16;
	default:
		break;
	}
	return p[0] ? 1u + 2u * p[0] : 1u;
}

/* An ObjectPropList as a set of elements against the expected ones. */
static bool same_list(const struct v_step *s, const uint8_t *p, size_t len)
{
	static size_t at[64], el[64];
	static bool used[64];
	size_t n = get32(p), i, k, pos = 4;

	if (n != s->n || n > ARRAY_LEN(at))
		return false;
	for (i = 0; i < n; i++) {
		at[i] = pos;
		el[i] = 8u + value_len(get16(p + pos + 6), p + pos + 8);
		pos += el[i];
		used[i] = false;
	}
	if (pos != len)
		return false;
	for (k = 0; k < n; k++) {
		for (i = 0; i < n; i++)
			if (!used[i] &&
			    same_bytes(&v_elems[s->first + k], p + at[i], el[i]))
				break;
		if (i == n)
			return false;
		used[i] = true;
	}
	return true;
}

static void hex_of(const uint8_t *b, char out[65])
{
	static const char hex[] = "0123456789abcdef";
	unsigned i;

	for (i = 0; i < 32; i++) {
		out[2 * i] = hex[b[i] >> 4];
		out[2 * i + 1] = hex[b[i] & 15u];
	}
	out[64] = '\0';
}

static bool same_digest(const struct v_step *s, size_t len)
{
	struct s15_sha256 h;
	char got[65], want[65];
	size_t i;

	s15_sha256_init(&h);
	for (i = 0; i < len; i++)
		s15_sha256_byte(&h, data[i]);
	s15_sha256_hex(&h, got);
	hex_of(s->digest, want);
	return len == s->length && strcmp(got, want) == 0;
}

static const char *why;

static bool step_ok(const struct v_step *s)
{
	uint32_t p[5];
	struct reply r;
	unsigned i;

	for (i = 0; i < 5; i++)
		p[i] = param(&s->p[i]);
	txn(s->op, p, 5, s->has_out ? v_blob + s->out.off : NULL,
	    s->out.len, 0, &r);
	if (r.code != s->code || r.n != s->n_reply) {
		why = "response";
		printf("  code 0x%04X want 0x%04X, %u params want %u\n", r.code,
		       s->code, r.n, s->n_reply);
		return false;
	}
	for (i = 0; i < r.n; i++)
		if (r.params[i] != param(&s->reply[i])) {
			why = "parameter";
			return false;
		}
	if (s->has_clock && vol.clock != s->clock) {
		why = "clock";
		return false;
	}
	if (r.data != (s->expect != X_NONE)) {
		why = "data phase";
		return false;
	}
	why = "data";
	switch ((enum v_expect)s->expect) {
	case X_BYTES:
		return same_bytes(&s->data, data, r.len);
	case X_HANDLES:
		return r.len >= 4 && get32(data) == s->n &&
		       r.len == 4u + 4u * s->n && same_set(s, data + 4, s->n, 4);
	case X_PROPLIST:
		return same_list(s, data, r.len);
	case X_OBJECTS:
		return r.len == s->length && get32(data) == s->n &&
		       same_set(s, data + 4, s->n, 12);
	case X_DIGEST:
		return same_digest(s, r.len);
	case X_ANY:
		return !s->length || r.len == s->length;
	case X_NONE:
	default:
		break;
	}
	return true;
}

/* --- The vectors ------------------------------------------------------------- */

static void open_session(void)
{
	CHECK(op0(0x1002, 1) == S10B_OK);
	bind();
}

static void test_scripts(void)
{
	size_t k, i;

	for (k = 0; k < ARRAY_LEN(v_scripts); k++) {
		const struct v_script *sc = &v_scripts[k];

		engine(sc->volume, MAX_OBJECTS, 512);
		vol.battery = sc->battery;
		vol.clock = sc->clock;
		if (sc->open)
			open_session();
		for (i = 0; i < sc->n; i++) {
			const struct v_step *s = &v_steps[sc->first + i];

			if (!step_ok(s)) {
				printf("  %s: step %u (op 0x%04X): %s\n",
				       sc->what, (unsigned)i, s->op, why);
				CHECK(0);
			}
		}
	}
}

/* S-10b.006, .011, .086, .090, .091 */
static void test_sweep(void)
{
	static const uint8_t zero = 0;
	static const uint32_t none[5] = { 0, 0, 0, 0, 0 };
	size_t k;

	engine(V_VOL_TREE_BASIC, MAX_OBJECTS, 512);
	for (k = 0; k < ARRAY_LEN(v_sweep); k++) {
		const struct v_sweep *w = &v_sweep[k];
		struct reply r;

		s10b_reset(&m, 512);
		if (w->in_session)
			CHECK(op0(0x1002, 1) == S10B_OK);
		txn(w->op, none, 0, w->host_data ? &zero : NULL, 1, 0, &r);
		if (r.code != w->code || r.data != (w->device_data != 0)) {
			printf("  op 0x%04X in_session %u: 0x%04X\n", w->op,
			       w->in_session, r.code);
			CHECK(0);
		}
	}
}

/* S-10b.046 */
static void test_formats(void)
{
	size_t k;

	for (k = 0; k < ARRAY_LEN(v_formats); k++) {
		const char *p = v_formats[k].path;
		uint16_t rows = 0;

		while (*p) {
			const char *end = strchr(p + 1, '/');
			const size_t n = end ? (size_t)(end - p - 1) :
					      strlen(p + 1);

			rows |= s10b_format_rows((const uint8_t *)p + 1, n);
			p += n + 1;
		}
		CHECK(s10b_format(rows) == v_formats[k].format);
	}
}

/* S-10b.047, Q-02 */
static void test_names(void)
{
	uint16_t u[S10B_NAME_UNITS];
	uint8_t s[1 + 2 * (S10B_NAME_UNITS + 1)];
	size_t k, i;

	for (k = 0; k < ARRAY_LEN(v_names); k++) {
		const struct v_name *v = &v_names[k];
		const size_t n = s10b_name_utf16(v_blob + v->name.off,
						 v->name.len, u);
		size_t len = 1;

		s[0] = (uint8_t)(n ? n + 1u : 0u);
		for (i = 0; i < n; i++) {
			s[len++] = (uint8_t)u[i];
			s[len++] = (uint8_t)(u[i] >> 8);
		}
		if (n) {
			s[len++] = 0;
			s[len++] = 0;
		}
		CHECK(len == v->presented.len &&
		      memcmp(s, v_blob + v->presented.off, len) == 0);
	}
}

/* Beyond the vectors: an overlong E0 sequence, and a surrogate pair that
 * would end past the 254th unit. */
static void test_names_edges(void)
{
	static const uint8_t overlong[] = { 0xE0, 0x80, 0x80, 'x' };
	uint8_t name[257];
	uint16_t u[S10B_NAME_UNITS];
	size_t n;

	n = s10b_name_utf16(overlong, sizeof(overlong), u);
	CHECK(n == 4 && u[0] == 0xFFFD && u[1] == 0xFFFD && u[2] == 0xFFFD &&
	      u[3] == 'x');
	memset(name, 'a', 253);
	memcpy(name + 253, "\xF0\x9F\x8E\xB5", 4);	/* U+1F3B5 */
	CHECK(s10b_name_utf16(name, 257, u) == 253);
	CHECK(s10b_name_utf16(name + 1, 256, u) == 254 && u[252] == 0xD83C &&
	      u[253] == 0xDFB5);
}

/* S-10b.048 beyond the vectors: 2100 is not a leap year. */
static void test_datetime_text(void)
{
	static const struct {
		uint32_t secs;
		const char *text;
	} t[] = {
		{ 4107542400u, "21000301T000000" },
		{ 951782400u, "20000229T000000" },
		{ 4102444799u, "20991231T235959" },
		{ 0xFFFFFFFFu, "21060207T062815" },
	};
	char out[16];
	size_t k;

	for (k = 0; k < ARRAY_LEN(t); k++) {
		s10b_datetime_text(t[k].secs, out);
		CHECK(strcmp(out, t[k].text) == 0);
	}
}

/* S-10b.062's grammar beyond the vectors: the range is optional. */
static void test_datetime_parse(void)
{
	static const char *const ok[] = {
		"18000101T000000", "20991231T235959.9-1200",
	};
	uint16_t u[32] = { 0 };
	struct s10b_datetime t;
	size_t k, i;

	for (k = 0; k < ARRAY_LEN(ok); k++) {
		for (i = 0; ok[k][i]; i++)
			u[i] = (uint8_t)ok[k][i];
		CHECK(s10b_datetime_parse(u, i, false, &t));
		CHECK(!s10b_datetime_parse(u, i, true, &t));
	}
	u[0] = 0x0661;		/* ARABIC-INDIC DIGIT ONE is not a digit */
	CHECK(!s10b_datetime_parse(u, 15, false, &t));
}

/* --- Framing ------------------------------------------------------------- */

/* S-10b.003 at both packet sizes. */
static void test_zlp(void)
{
	static const size_t mps[2] = { 512, 64 };
	size_t s, k;

	for (s = 0; s < 2; s++) {
		engine(V_VOL_SIZES, MAX_OBJECTS, mps[s]);
		open_session();
		for (k = 0; k < ARRAY_LEN(v_sizes); k++) {
			const struct v_size *z = &v_sizes[k];
			struct reply r;
			uint32_t h = handle_of(z->path);

			txn(0x1009, &h, 1, NULL, 0, 0, &r);
			CHECK(r.code == S10B_OK && r.len == z->size);
			CHECK(r.zlp == (s == 0 ? z->zlp_512 : z->zlp_64));
		}
	}
}

/* S-10b.004: a container out of place gets nothing, of any length. */
static void test_out_of_place(void)
{
	static uint8_t stray[1500];
	struct reply r;
	size_t mps, n;

	for (mps = 64; mps <= 512; mps *= 8) {
		engine(V_VOL_TREE_BASIC, MAX_OBJECTS, mps);
		header(stray, sizeof(stray), 2, 0x1001, 99);
		memset(stray + 12, 0x5A, sizeof(stray) - 12);
		/* Payload that looks like a command must not be taken. */
		header(stray + 128, 12, 1, 0x1002, 7);
		send(stray, sizeof(stray), 0);
		header(stray, 12, 3, 0x2001, 5);	/* a response, too */
		send(stray, 12, 0);
		s10b_rx(&m, stray, 0);			/* a ZLP */
		CHECK(!s10b_tx(&m, stray, sizeof(stray), &n));
		txn(0x1001, NULL, 0, NULL, 0, 0, &r);
		CHECK(r.code == S10B_OK && r.data && r.len > 100);
		CHECK(op0(0x1004, 0) == S10B_SESSION_NOT_OPEN);
	}
}

/* S-10b.006: the data phase comes in whole before the response, however
 * split; refused ones too, of any length. */
static void test_host_data(void)
{
	static const char value[] = "20261027T120000";
	static uint8_t big[1u << 20];
	uint8_t s[1 + 2 * 16];
	uint32_t p[2] = { 0x5011, 0 };
	struct reply r;
	size_t i, mps;

	s[0] = 16;
	for (i = 0; i < 16; i++) {
		s[1 + 2 * i] = (uint8_t)value[i];
		s[2 + 2 * i] = 0;
	}
	for (mps = 64; mps <= 512; mps *= 8) {
		engine(V_VOL_TREE_BASIC, MAX_OBJECTS, mps);
		open_session();
		vol.clock = 0;
		/* 1 byte, then the rest. */
		txn(0x1016, p, 1, s, sizeof(s), 1, &r);
		CHECK(r.code == S10B_OK && vol.clock != 0);
		/* Ending on a full packet, with a ZLP after it. */
		{
			uint8_t pad[64 * 3 - 12];

			memset(pad, 0, sizeof(pad));
			memcpy(pad, s, sizeof(s));
			pad[0] = 16;
			tid++;
			header(data_out, 12u + 4u, 1, 0x1016, tid);
			put32(data_out + 12, 0x5011);
			send(data_out, 16, 0);
			header(data_out, 12u + sizeof(pad), 2, 0x1016, tid);
			memcpy(data_out + 12, pad, sizeof(pad));
			if (mps == 64) {
				send(data_out, 12u + sizeof(pad), 0);
			} else {
				/* At 512 the 192 bytes are one short packet. */
				CHECK(s10b_rx_size(&m) == 512);
				s10b_rx(&m, data_out, 12u + sizeof(pad));
			}
			receive(0x1016, &r);
			CHECK(r.code == S10B_OK);
			/* The ZLP, now in place of a command: nothing. */
			s10b_rx(&m, pad, 0);
			CHECK(!s10b_tx(&m, pad, sizeof(pad), &i));
		}
		/* An OUT transfer never reaches past the container: at 64 a
		 * 128-byte phase arms 64 and then 64, not 512. */
		tid++;
		header(data_out, 16, 1, 0x1016, tid);
		put32(data_out + 12, 0x5011);
		send(data_out, 16, 0);
		CHECK(s10b_rx_size(&m) == mps);
		header(data_out, 128, 2, 0x1016, tid);
		memset(data_out + 12, 0, 128 - 12);
		memcpy(data_out + 12, s, sizeof(s));
		s10b_rx(&m, data_out, mps == 64 ? 64 : 128);
		if (mps == 64) {
			CHECK(s10b_rx_size(&m) == 64);
			s10b_rx(&m, data_out + 64, 64);
		}
		receive(0x1016, &r);
		CHECK(r.code == S10B_OK);
		/* S-10b.086: a 1 MiB SendObject, taken in and refused. */
		memset(big, 0xA5, sizeof(big));
		txn(0x100D, NULL, 0, big, sizeof(big), 0, &r);
		CHECK(r.code == S10B_OP_NOT_SUPPORTED && !r.data);
		/* The data container of SendObjectInfo too. */
		p[0] = S10B_STORAGE_ID;
		p[1] = 0;
		txn(0x100C, p, 2, big, 300, 0, &r);
		CHECK(r.code == S10B_OP_NOT_SUPPORTED && !r.data);
		p[0] = 0x5011;
		CHECK(op0(0x1004, 0) == S10B_OK);
	}
}

/* S-10b.050, S-10b.051: read failures. */
static void test_read_failures(void)
{
	static const uint32_t mps[2] = { 512, 64 };
	uint32_t p[3];
	struct reply r;
	size_t s;

	for (s = 0; s < 2; s++) {
		engine(V_VOL_SIZES, MAX_OBJECTS, mps[s]);
		open_session();
		/* A file that cannot be opened: no data phase. */
		vol.fail_path = "/s1048576.bin";
		vol.fail_at = 0;
		p[0] = handle_of(vol.fail_path);
		txn(0x1009, p, 1, NULL, 0, 0, &r);
		CHECK(r.code == S10B_GENERAL_ERROR && !r.data && r.n == 0);
		p[1] = 0;
		p[2] = 100;
		txn(0x101B, p, 3, NULL, 0, 0, &r);
		CHECK(r.code == S10B_GENERAL_ERROR && !r.data && r.n == 0);
		/* Failing part way: ended short, Incomplete_Transfer, both at
		 * a packet boundary and inside a packet. */
		vol.fail_at = 4096;
		txn(0x1009, p, 1, NULL, 0, 0, &r);
		CHECK(r.code == S10B_INCOMPLETE_TRANSFER && r.data &&
		      r.short_end && r.n == 0);
		vol.fail_at = 1000;
		txn(0x1009, p, 1, NULL, 0, 0, &r);
		CHECK(r.code == S10B_INCOMPLETE_TRANSFER && r.short_end);
		/* A failure where the bytes sent end on a full packet: a
		 * zero-length packet ends the phase. Reads are of
		 * S10B_STAGE_MAX bytes; find one that ends on a packet. */
		{
			uint32_t k = 1;

			while ((12u + k * S10B_STAGE_MAX) % mps[s] != 0)
				k++;
			vol.fail_at = (k + 1u) * S10B_STAGE_MAX;
			txn(0x1009, p, 1, NULL, 0, 0, &r);
			CHECK(r.code == S10B_INCOMPLETE_TRANSFER && r.short_end &&
			      r.zlp && r.len == k * S10B_STAGE_MAX);
		}
		p[2] = S10B_ALL;
		txn(0x101B, p, 3, NULL, 0, 0, &r);
		CHECK(r.code == S10B_INCOMPLETE_TRANSFER && r.n == 0);
		/* The next transaction is answered normally. */
		vol.fail_path = NULL;
		txn(0x1009, p, 1, NULL, 0, 0, &r);
		CHECK(r.code == S10B_OK && r.len == (1u << 20));
	}
}

/* S-10b.005, S-10b.007 */
static void test_cancel_and_reset(void)
{
	uint8_t cmd[16], xfer[512];
	uint32_t old;
	size_t n;
	struct reply r;

	engine(V_VOL_SIZES, MAX_OBJECTS, 512);
	open_session();
	old = handle_of("/s1048576.bin");
	tid++;
	header(cmd, 16, 1, 0x1009, tid);
	put32(cmd + 12, old);
	send(cmd, 16, 0);
	CHECK(s10b_busy(&m) && s10b_rx_size(&m) == 0);
	CHECK(s10b_tx(&m, xfer, sizeof(xfer), &n) && n == 512);
	s10b_cancel(&m);
	CHECK(!s10b_busy(&m) && !s10b_tx(&m, xfer, sizeof(xfer), &n));
	txn(0x1001, NULL, 0, NULL, 0, 0, &r);
	CHECK(r.code == S10B_OK && r.data);
	CHECK(op0(0x1008, old) == S10B_OK);	/* the session stays */

	/* A bus event closes the session; the old handle is no use after. */
	s10b_reset(&m, 64);
	CHECK(op0(0x1008, old) == S10B_SESSION_NOT_OPEN);
	open_session();
	CHECK(op0(0x1008, handle_of("/s1048576.bin")) == S10B_OK);
}

/* S-10b.012: no handle has the form 0x0001xxxx, past 65535 objects too. */
static void test_many_handles(void)
{
	static struct s10b_object many[MANY];
	static uint8_t pool[MANY * 6u];
	static const struct v_volume flat = { 1, 1u << 30, 0, 0, 0, 0 };
	const struct s10b_config cfg = {
		.vol = &vol_ops, .dev = &dev_ops,
		.objects = many, .max_objects = MANY,
		.names = pool, .names_size = sizeof(pool),
	};
	const uint32_t p[3] = { S10B_STORAGE_ID, 0, 0 };
	uint32_t h;
	struct reply r;
	size_t i;
	bool clean = true;

	vol.v = &flat;
	vol.fail_path = NULL;
	for (i = 0; i < MANY; i++) {
		struct file *f = &files[i];

		(void)snprintf(many_paths[i], sizeof(many_paths[i]), "/f%05u",
			       (unsigned)(i % 100000u));
		memset(f, 0, sizeof(*f));
		f->path = (const uint8_t *)many_paths[i];
		f->len = 7;
	}
	n_files = MANY;
	s10b_init(&m, &cfg);
	s10b_reset(&m, 512);
	CHECK(op0(0x1002, 1) == S10B_OK && m.n_objects == MANY);
	txn(0x1007, p, 3, NULL, 0, 0, &r);
	CHECK(r.code == S10B_OK && get32(data) == MANY);
	for (i = 0; i < MANY; i++) {
		h = get32(data + 4 + 4 * i);
		if (h == 0 || h == S10B_ALL || h >> 16 == 1u)
			clean = false;
	}
	CHECK(clean);
	/* Each one names its own object, on both sides of the gap. */
	for (i = 65533; i < 65538; i++) {
		h = s10b_handle(i);
		CHECK(op0(0x1008, h) == S10B_OK);
		/* Filename: count 7, "f" and five digits. */
		CHECK(data[52] == 7u && get16(data + 53) == 'f');
		CHECK(get16(data + 53 + 2 * 4) == '0' + (i / 10u) % 10u &&
		      get16(data + 53 + 2 * 5) == '0' + i % 10u);
	}
	CHECK(op0(0x1008, 0x00010000u) == S10B_INVALID_HANDLE);
	CHECK(op0(0x1008, s10b_handle(MANY)) == S10B_INVALID_HANDLE);
}

/* S-10b.014: past the floor, what does not fit the caller's table is not
 * listed; every listed object's parent is, and the lists agree. */
static void test_table_full(void)
{
	const uint32_t p[3] = { S10B_STORAGE_ID, 0, 0 };
	struct reply r;
	size_t i;

	engine(V_VOL_BIG, 100, 512);
	open_session();
	CHECK(m.n_objects == 100);
	for (i = 0; i < m.n_objects; i++)
		CHECK(objects[i].parent <= i &&
		      (objects[i].parent == 0 ||
		       objects[objects[i].parent - 1u].kind == S10B_KIND_DIR));
	txn(0x1006, p, 3, NULL, 0, 0, &r);
	CHECK(r.code == S10B_OK && r.n == 1 && r.params[0] == 100);
	txn(0x1007, p, 3, NULL, 0, 0, &r);
	CHECK(r.code == S10B_OK && get32(data) == 100);
	/* The listing of a session is the volume when it opened. */
	vol.lists = 0;
	txn(0x1007, p, 3, NULL, 0, 0, &r);
	CHECK(vol.lists == 0);
}

/* S-10b.014: a volume at the floor, 8192 objects whose names total
 * 262,144 bytes, is listed in full by a caller that gives the floor. */
static void test_listing_floor(void)
{
	static struct s10b_object table[S10B_LIST_OBJECTS];
	static uint8_t pool[S10B_LIST_NAMES];
	static char paths[S10B_LIST_OBJECTS][1u + 32u + 1u];
	static const struct v_volume flat = { 1, 1u << 30, 0, 0, 0, 0 };
	const struct s10b_config cfg = {
		.vol = &vol_ops, .dev = &dev_ops,
		.objects = table, .max_objects = S10B_LIST_OBJECTS,
		.names = pool, .names_size = sizeof(pool),
	};
	const uint32_t p[3] = { S10B_STORAGE_ID, 0, 0 };
	struct reply r;
	size_t i;

	CHECK(S10B_LIST_NAMES == S10B_LIST_OBJECTS * 32u);
	vol.v = &flat;
	vol.fail_path = NULL;
	for (i = 0; i < S10B_LIST_OBJECTS; i++) {
		struct file *f = &files[i];

		(void)snprintf(paths[i], sizeof(paths[i]), "/%032u",
			       (unsigned)i);
		memset(f, 0, sizeof(*f));
		f->path = (const uint8_t *)paths[i];
		f->len = 33;
	}
	n_files = S10B_LIST_OBJECTS;
	s10b_init(&m, &cfg);
	s10b_reset(&m, 512);
	CHECK(op0(0x1002, 1) == S10B_OK && m.n_objects == S10B_LIST_OBJECTS);
	txn(0x1006, p, 3, NULL, 0, 0, &r);
	CHECK(r.code == S10B_OK && r.params[0] == S10B_LIST_OBJECTS);
	txn(0x1007, p, 3, NULL, 0, 0, &r);
	CHECK(r.code == S10B_OK && get32(data) == S10B_LIST_OBJECTS);
	CHECK(op0(0x1008, s10b_handle(S10B_LIST_OBJECTS - 1u)) == S10B_OK);
}

int main(void)
{
	RUN(test_scripts);
	RUN(test_sweep);
	RUN(test_formats);
	RUN(test_names);
	RUN(test_names_edges);
	RUN(test_datetime_text);
	RUN(test_datetime_parse);
	RUN(test_zlp);
	RUN(test_out_of_place);
	RUN(test_host_data);
	RUN(test_read_failures);
	RUN(test_cancel_and_reset);
	RUN(test_table_full);
	RUN(test_listing_floor);
	RUN(test_many_handles);
	return check_summary("test_s10b_mtp");
}

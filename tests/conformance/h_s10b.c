/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for S-10b's MTP responder (src/core/mtp/mtp.c) over an in-memory
 * volume. Containers are framed here, the host's data in 512-byte packets;
 * paths are hex (UTF-8 bytes, "/a/b").
 *
 * Commands (one result line each):
 *   begin LEVEL CLOCK FREE      -> ok: an empty volume; LEVEL 0 R, 1 W;
 *                                  CLOCK the RTC count (S-04.070)
 *   dir PATH MTIME CTIME        -> ok
 *   file PATH DATA MTIME CTIME  -> ok
 *   start                       -> ok: a fresh responder over the volume
 *   op CODE NP P1..PNP DATA     -> CODE PARAMS DATA: the response code, its
 *                                  parameters (comma-separated, "-" for none)
 *                                  and the data phase's dataset ("none" when
 *                                  there is none). DATA is the host data
 *                                  phase, "none" for none
 *   opp CODE NP P1..PNP LEN SEED -> as op, the host data LEN bytes
 *                                  (7 i + 3 + SEED) mod 256
 *   tree                        -> the volume: PATH:KIND:SIZE:MTIME:CTIME:
 *                                  ATIME:CRC32 per object, KIND 0 file 1
 *                                  folder, "-" when empty
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hlib.h"
#include "waffle/boot/records.h"
#include "waffle/mtp/mtp.h"

#define MAX_FILES 4096
#define MPS 512u
#define DATA_MAX ((1u << 20) + 64u)

struct file {
	char *path;
	uint8_t *data;
	uint32_t size, room, mtime, ctime, atime;
	int dir, used;
};

static struct file files[MAX_FILES];
static int level;
static uint32_t clock_count;
static uint64_t free_bytes;
static struct s10b m;
static struct s10b_object objects[S10B_LIST_OBJECTS];
static uint8_t pool[S10B_LIST_POOL];
static uint8_t in[DATA_MAX], out[DATA_MAX + 64u];
static uint32_t tid;

static uint32_t now(void)
{
	struct s04_datetime t;
	struct s10b_datetime d;
	uint32_t s = 0;

	s04_rtc_decode(clock_count, &t);
	d.year = t.year;
	d.month = t.month;
	d.day = t.day;
	d.hour = t.hour;
	d.minute = t.minute;
	d.second = t.second;
	(void)s10b_datetime_secs(&d, &s);
	return s;
}

static int find(const char *path)
{
	for (int i = 0; i < MAX_FILES; i++)
		if (files[i].used && strcmp(files[i].path, path) == 0)
			return i;
	return -1;
}

static int add(const char *path, int dir)
{
	for (int i = 0; i < MAX_FILES; i++) {
		if (files[i].used)
			continue;
		memset(&files[i], 0, sizeof files[i]);
		files[i].used = 1;
		files[i].dir = dir;
		files[i].path = malloc(strlen(path) + 1u);
		if (!files[i].path)
			return -1;
		strcpy(files[i].path, path);
		files[i].mtime = files[i].ctime = files[i].atime = now();
		return i;
	}
	return -1;
}

static void drop(int i)
{
	free(files[i].path);
	free(files[i].data);
	memset(&files[i], 0, sizeof files[i]);
}

static int parent_ok(const char *path)
{
	const char *last = strrchr(path, '/');
	char p[4096];
	size_t n = (size_t)(last - path);
	int i;

	if (n == 0)
		return 1;
	if (n >= sizeof p)
		return 0;
	memcpy(p, path, n);
	p[n] = '\0';
	i = find(p);
	return i >= 0 && files[i].dir;
}

/* --- the volume ops --- */

static bool v_space(void *ctx, uint64_t *total, uint64_t *fr)
{
	(void)ctx;
	*total = 2403870720u;
	*fr = free_bytes;
	return true;
}

static bool v_list(void *ctx, const char *path, s10b_entry_fn fn, void *arg)
{
	const size_t plen = strcmp(path, "/") == 0 ? 0 : strlen(path);

	(void)ctx;
	for (int i = 0; i < MAX_FILES; i++) {
		struct s10b_entry e;
		const char *last;

		if (!files[i].used)
			continue;
		last = strrchr(files[i].path, '/');
		if ((size_t)(last - files[i].path) != plen ||
		    memcmp(files[i].path, path, plen) != 0)
			continue;
		e.name = (const uint8_t *)last + 1;
		e.name_len = strlen(last + 1);
		e.kind = files[i].dir ? S10B_KIND_DIR : S10B_KIND_FILE;
		e.size = files[i].size;
		e.mtime = files[i].mtime;
		e.ctime = files[i].ctime;
		fn(arg, &e);
	}
	return true;
}

static long v_read(void *ctx, const char *path, uint32_t off, uint8_t *buf, size_t n)
{
	const int i = find(path);

	(void)ctx;
	if (i < 0 || files[i].dir)
		return -1;
	if (off >= files[i].size)
		return 0;
	if (n > files[i].size - off)
		n = files[i].size - off;
	memcpy(buf, files[i].data + off, n);
	return (long)n;
}

static bool v_stat(void *ctx, const char *path, struct s10b_entry *e)
{
	const int i = find(path);

	(void)ctx;
	if (i < 0)
		return false;
	e->kind = files[i].dir ? S10B_KIND_DIR : S10B_KIND_FILE;
	e->size = files[i].size;
	e->mtime = files[i].mtime;
	e->ctime = files[i].ctime;
	return true;
}

static enum s10b_result v_mkdir(void *ctx, const char *path)
{
	(void)ctx;
	if (find(path) >= 0)
		return S10B_V_EXISTS;
	if (!parent_ok(path))
		return S10B_V_FAIL;
	return add(path, 1) >= 0 ? S10B_V_OK : S10B_V_FULL;
}

static enum s10b_result v_create(void *ctx, const char *path, int32_t *fd)
{
	(void)ctx;
	if (find(path) >= 0)
		return S10B_V_EXISTS;
	if (!parent_ok(path))
		return S10B_V_FAIL;
	*fd = add(path, 0);
	return *fd >= 0 ? S10B_V_OK : S10B_V_FULL;
}

static enum s10b_result v_write(void *ctx, int32_t fd, const uint8_t *p, size_t n)
{
	struct file *f = &files[fd];

	(void)ctx;
	if (f->size + n > f->room) {
		uint8_t *d = realloc(f->data, (f->size + n) * 2u);

		if (!d)
			return S10B_V_FULL;
		f->data = d;
		f->room = (uint32_t)((f->size + n) * 2u);
	}
	memcpy(f->data + f->size, p, n);
	f->size += (uint32_t)n;
	return S10B_V_OK;
}

static enum s10b_result v_close(void *ctx, int32_t fd)
{
	(void)ctx;
	(void)fd;
	return S10B_V_OK;
}

static int under(int i, const char *path)
{
	const size_t n = strlen(path);

	return files[i].used && strncmp(files[i].path, path, n) == 0 &&
	       (files[i].path[n] == '\0' || files[i].path[n] == '/');
}

static enum s10b_result v_remove(void *ctx, const char *path)
{
	(void)ctx;
	for (int i = 0; i < MAX_FILES; i++)
		if (under(i, path))
			drop(i);
	return S10B_V_OK;
}

static enum s10b_result v_rename(void *ctx, const char *from, const char *to)
{
	const size_t a = strlen(from);

	(void)ctx;
	if (find(to) >= 0)
		return S10B_V_EXISTS;
	if (find(from) < 0 || !parent_ok(to))
		return S10B_V_FAIL;
	for (int i = 0; i < MAX_FILES; i++) {
		char *np;

		if (!under(i, from))
			continue;
		np = malloc(strlen(to) + strlen(files[i].path + a) + 1u);
		if (!np)
			return S10B_V_FAIL;
		strcpy(np, to);
		strcat(np, files[i].path + a);
		free(files[i].path);
		files[i].path = np;
	}
	return S10B_V_OK;
}

static enum s10b_result v_times(void *ctx, const char *path, uint32_t at, uint32_t mt)
{
	const int i = find(path);

	(void)ctx;
	if (i < 0)
		return S10B_V_FAIL;
	files[i].atime = at;
	files[i].mtime = mt;
	return S10B_V_OK;
}

static const struct s10b_volume_ops vol_w = {
	.space = v_space, .list = v_list, .read = v_read, .stat = v_stat,
	.mkdir = v_mkdir, .create = v_create, .write = v_write, .close = v_close,
	.remove = v_remove, .rename = v_rename, .set_times = v_times,
};
/* Level R: a change would call a null pointer. */
static const struct s10b_volume_ops vol_r = {
	.space = v_space, .list = v_list, .read = v_read,
};

static unsigned d_battery(void *ctx) { (void)ctx; return 57; }
static uint32_t d_clock(void *ctx) { (void)ctx; return clock_count; }
static void d_set(void *ctx, uint32_t c) { (void)ctx; clock_count = c; }

static const struct s10b_device_ops dev = { NULL, d_battery, d_clock, d_set };

/* --- transactions --- */

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void feed(const uint8_t *p, size_t n)
{
	while (n) {
		size_t k = s10b_rx_size(&m);

		if (k == 0)
			return;
		if (k > n)
			k = n;
		s10b_rx(&m, p, k);
		p += k;
		n -= k;
	}
}

static void txn(unsigned code, const uint32_t *p, unsigned np, const uint8_t *data, long dn)
{
	uint8_t c[32], pkt[MPS];
	size_t got = 0, k = 0;
	int has_data = 0;
	unsigned i;

	tid++;
	put32(c, 12u + 4u * np);
	c[4] = 1;
	c[5] = 0;
	c[6] = (uint8_t)code;
	c[7] = (uint8_t)(code >> 8);
	put32(c + 8, tid);
	for (i = 0; i < np; i++)
		put32(c + 12 + 4 * i, p[i]);
	feed(c, 12u + 4u * np);
	if (dn >= 0) {
		static uint8_t buf[DATA_MAX + 12u];

		put32(buf, (uint32_t)(12 + dn));
		buf[4] = 2;
		buf[5] = 0;
		buf[6] = (uint8_t)code;
		buf[7] = (uint8_t)(code >> 8);
		put32(buf + 8, tid);
		memcpy(buf + 12, data, (size_t)dn);
		feed(buf, (size_t)(12 + dn));
		if ((12 + dn) % MPS == 0)
			s10b_rx(&m, buf, 0);
	}
	for (;;) {
		if (!s10b_tx(&m, pkt, sizeof pkt, &k)) {
			h_put("error no response");
			return;
		}
		if (!has_data && k >= 12 && pkt[4] == 2)
			has_data = 1;
		else if (!has_data)
			break;
		if (got + k <= sizeof out) {
			memcpy(out + got, pkt, k);
			got += k;
		}
		if (k < MPS || got >= get32(out)) {
			if (k == MPS)
				(void)s10b_tx(&m, pkt, sizeof pkt, &k);	/* the ZLP */
			if (!s10b_tx(&m, pkt, sizeof pkt, &k)) {
				h_put("error no response");
				return;
			}
			break;
		}
	}
	h_putx((unsigned long)(pkt[6] | pkt[7] << 8));
	{
		char s[100];
		size_t pos = 0;
		const unsigned n = (unsigned)(get32(pkt) - 12u) / 4u;

		if (n == 0)
			strcpy(s, "-");
		for (i = 0; i < n && i < 5; i++)
			pos += (size_t)snprintf(s + pos, sizeof s - pos, "%s0x%x", i ? "," : "",
						(unsigned)get32(pkt + 12 + 4 * i));
		h_put(s);
	}
	if (has_data)
		h_puthex(out + 12, got - 12u);
	else
		h_put("none");
}

static int hexpath(const struct h_cmd *c, int i, char *p, size_t cap)
{
	size_t n = h_hex(c, i, (uint8_t *)p, cap - 1u);

	p[n] = '\0';
	return n > 0;
}

int main(void)
{
	struct h_cmd c;
	char path[4096];

	while (h_next(&c)) {
		const char *k = c.tok[0];

		if (strcmp(k, "begin") == 0 && c.n == 4) {
			for (int i = 0; i < MAX_FILES; i++)
				if (files[i].used)
					drop(i);
			level = (int)h_num(&c, 1);
			clock_count = (uint32_t)h_num(&c, 2);
			free_bytes = h_num(&c, 3);
			h_put("ok");
		} else if (strcmp(k, "dir") == 0 && c.n == 4 && hexpath(&c, 1, path, sizeof path)) {
			const int i = add(path, 1);

			if (i < 0) {
				h_error(&c, "full");
				continue;
			}
			files[i].mtime = (uint32_t)h_num(&c, 2);
			files[i].ctime = (uint32_t)h_num(&c, 3);
			files[i].atime = 0;
			h_put("ok");
		} else if (strcmp(k, "file") == 0 && c.n == 5 && hexpath(&c, 1, path, sizeof path)) {
			const int i = add(path, 0);
			size_t n;

			if (i < 0) {
				h_error(&c, "full");
				continue;
			}
			n = h_hex(&c, 2, in, sizeof in);
			files[i].data = malloc(n ? n : 1u);
			memcpy(files[i].data, in, n);
			files[i].size = files[i].room = (uint32_t)n;
			files[i].mtime = (uint32_t)h_num(&c, 3);
			files[i].ctime = (uint32_t)h_num(&c, 4);
			files[i].atime = 0;
			h_put("ok");
		} else if (strcmp(k, "start") == 0 && c.n == 1) {
			struct s10b_config cfg;

			memset(&cfg, 0, sizeof cfg);
			cfg.level = level ? S10B_LEVEL_W : S10B_LEVEL_R;
			cfg.vol = level ? &vol_w : &vol_r;
			cfg.dev = &dev;
			cfg.manufacturer = "Manufacturer";
			cfg.model = "Model";
			cfg.version = "1.0.0";
			cfg.serial = "SERIAL0001";
			cfg.objects = objects;
			cfg.max_objects = S10B_LIST_OBJECTS;
			cfg.names = pool;
			cfg.names_size = sizeof pool;
			s10b_init(&m, &cfg);
			s10b_reset(&m, MPS);
			h_put("ok");
		} else if ((strcmp(k, "op") == 0 || strcmp(k, "opp") == 0) && c.n >= 3) {
			const unsigned np = (unsigned)h_num(&c, 2);
			uint32_t p[5];
			long dn = -1;

			if (np > 5 || c.n != (int)(3 + np + (k[2] == 'p' ? 2 : 1))) {
				h_error(&c, "fields");
				continue;
			}
			for (unsigned i = 0; i < np; i++)
				p[i] = (uint32_t)h_num(&c, 3 + (int)i);
			if (k[2] == 'p') {
				const unsigned long len = h_num(&c, 3 + (int)np);
				const unsigned long seed = h_num(&c, 4 + (int)np);

				if (len > DATA_MAX) {
					h_error(&c, "too long");
					continue;
				}
				for (unsigned long i = 0; i < len; i++)
					in[i] = (uint8_t)((7u * i + 3u + seed) & 0xFFu);
				dn = (long)len;
			} else if (strcmp(c.tok[3 + np], "none") != 0) {
				dn = (long)h_hex(&c, 3 + (int)np, in, sizeof in);
			}
			txn((unsigned)h_num(&c, 1), p, np, in, dn);
		} else if (strcmp(k, "tree") == 0 && c.n == 1) {
			int any = 0;

			for (int i = 0; i < MAX_FILES; i++) {
				static char s[2 * sizeof path + 64];
				size_t pos = 0;

				if (!files[i].used)
					continue;
				any = 1;
				for (const char *q = files[i].path; *q && pos + 2 < 2 * sizeof path; q++)
					pos += (size_t)snprintf(s + pos, sizeof s - pos, "%02x", (unsigned)(uint8_t)*q);
				snprintf(s + pos, sizeof s - pos, ":%d:%u:%u:%u:%u:%08x", files[i].dir,
					 (unsigned)files[i].size, (unsigned)files[i].mtime,
					 (unsigned)files[i].ctime, (unsigned)files[i].atime,
					 (unsigned)h_crc32(0, files[i].data, files[i].size));
				h_put(s);
			}
			if (!any)
				h_put("-");
		} else {
			h_error(&c, "unknown command");
			continue;
		}
		h_end();
	}
	return h_status();
}

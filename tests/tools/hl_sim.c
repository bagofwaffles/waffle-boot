/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The host link's protocol (src/hostlink.c) over volumes in memory, as a
 * shared library for the host tools' tests (tests/tools/hl_sim.py): the
 * drive (/yaffs2, read-write), the installed OS's two copies (/os/boot1
 * and boot2) and the OS's FAT volume (/user, when turned on: src/hl_fat.c
 * on a FAT volume sim_user() builds in memory with f_mkfs), an approval
 * the test answers, and jobs the test moves on; an image into RAM
 * (S-10c.135) up to 8 MiB, checked by a stand-in for S-05.024 that refuses
 * one not starting with the LDR signature, and the approval with no hold
 * (S-10c.137) the test turns on.
 */
#include <stdlib.h>
#include <string.h>

#include "waffle/hl_fat.h"
#include "waffle/hostlink.h"
#include "bearssl_hash.h"
#include "wb_fatfs.h"

#define NFILES 64

struct sfile {
	int used, dir;
	char path[HL_PATH_MAX + 16];
	uint8_t *data;
	uint32_t len, cap;
};

static struct {
	int on, user, pending, corrupt, writing;
	struct sfile f[NFILES];
	uint8_t os[2][4096];
	uint32_t os_len[2];
	uint8_t run_action, run_arg, fail_next;
	char run_path[HL_PATH_MAX + 16];
	unsigned steps;
	int no_hold;
	uint8_t *ram;
	uint32_t ram_size;
} S;

static struct hostlink H;

/* /user: 8 MiB of memory, formatted by sim_user() */
#define USER_SECTORS 16384u
static uint8_t user_disk[USER_SECTORS * 512u];

static int u_read(void *c, uint32_t lba, void *buf, uint32_t n)
{
	(void)c;
	if (lba >= USER_SECTORS || n > USER_SECTORS - lba)
		return -1;
	memcpy(buf, user_disk + (size_t)lba * 512u, (size_t)n * 512u);
	return 0;
}

static int u_write(void *c, uint32_t lba, const void *buf, uint32_t n)
{
	(void)c;
	if (lba >= USER_SECTORS || n > USER_SECTORS - lba)
		return -1;
	memcpy(user_disk + (size_t)lba * 512u, buf, (size_t)n * 512u);
	return 0;
}

static const struct wb_blockdev user_dev = { .ctx = NULL, .n_sectors = USER_SECTORS,
					     .read = u_read, .write = u_write };
static struct hl_fat U;

static struct sfile *find(const char *p)
{
	for (int i = 0; i < NFILES; i++)
		if (S.f[i].used && strcmp(S.f[i].path, p) == 0)
			return &S.f[i];
	return NULL;
}

static struct sfile *make(const char *p, int dir)
{
	for (int i = 0; i < NFILES; i++)
		if (!S.f[i].used) {
			free(S.f[i].data);
			memset(&S.f[i], 0, sizeof(S.f[i]));
			S.f[i].used = 1;
			S.f[i].dir = dir;
			strncpy(S.f[i].path, p, sizeof(S.f[i].path) - 1u);
			return &S.f[i];
		}
	return NULL;
}

static int append(struct sfile *f, const uint8_t *b, uint32_t n)
{
	if (f->len + n > f->cap) {
		uint32_t cap = (f->len + n) * 2u + 64u;
		uint8_t *d = realloc(f->data, cap);

		if (d == NULL)
			return HL_E_IO;
		f->data = d;
		f->cap = cap;
	}
	memcpy(f->data + f->len, b, n);
	f->len += n;
	return HL_OK;
}

static unsigned os_part(const struct hl_path *p)
{
	return strcmp(p->rest, "boot1") == 0 ? 1u : strcmp(p->rest, "boot2") == 0 ? 2u : 0u;
}

static bool o_on(void *c)
{
	(void)c;
	return S.on != 0;
}

static bool o_approve(void *c)
{
	(void)c;
	S.pending = 1;
	return true;
}

static void o_info(void *c, uint8_t o[HL_INFO_LEN])
{
	static const uint8_t info[HL_INFO_LEN] = {
		0, 0, 9, 0, 49, 0, 0, 0,	/* 0.9.49+0 */
		22, 0,				/* front 22 */
		0, 1,				/* OP-XY, Slot 2 */
		0, 0,				/* reason 0 */
		0x2A, 0,			/* dev mode, MTP macOS, host link */
		1, 0, 2, 0, 3, 0, 4, 0,		/* OS 1.2.3+4 */
	};

	(void)c;
	memcpy(o, info, HL_INFO_LEN);
}

static bool o_root_ok(void *c, unsigned r)
{
	(void)c;
	return r != HL_ROOT_USER || S.user;
}

static int o_stat(void *c, const struct hl_path *p, struct hl_stat *st)
{
	const struct sfile *f;

	(void)c;
	st->mtime = 0;
	st->size = 0;
	st->type = 2;
	if (p->root == HL_ROOT_USER)
		return hlfat_stat(&U, p->rest, st);
	if (*p->rest == '\0')
		return HL_OK;
	if (p->root == HL_ROOT_OS) {
		if (os_part(p) == 0u)
			return HL_E_NOT_FOUND;
		st->type = 1;
		st->size = S.os_len[os_part(p) - 1u];
		return HL_OK;
	}
	f = find(p->full);
	if (f == NULL)
		return HL_E_NOT_FOUND;
	st->type = f->dir ? 2u : 1u;
	st->size = f->dir ? 0u : f->len;
	return HL_OK;
}

static int o_list(void *c, const struct hl_path *p, uint32_t start, hl_each_fn each,
		  void *arg, bool *end)
{
	const size_t n = strlen(p->full);
	uint32_t k = 0;

	(void)c;
	*end = false;
	if (p->root == HL_ROOT_USER)
		return hlfat_list(&U, p->rest, start, each, arg, end);
	if (p->root == HL_ROOT_OS) {
		static const char *const names[] = { "boot1", "boot2" };

		for (uint32_t i = start; i < 2u; i++) {
			struct hl_stat st = { 1, S.os_len[i], 0 };

			if (!each(arg, names[i], &st))
				return HL_OK;
		}
		*end = true;
		return HL_OK;
	}
	if (*p->rest != '\0' && (find(p->full) == NULL || !find(p->full)->dir))
		return HL_E_NOT_FOUND;
	for (int i = 0; i < NFILES; i++) {
		const struct sfile *f = &S.f[i];
		struct hl_stat st = { f->dir ? 2u : 1u, f->dir ? 0u : f->len, 0 };

		if (!f->used || strncmp(f->path, p->full, n) != 0 || f->path[n] != '/' ||
		    strchr(f->path + n + 1, '/') != NULL)
			continue;
		if (k++ < start)
			continue;
		if (!each(arg, f->path + n + 1, &st))
			return HL_OK;
	}
	*end = true;
	return HL_OK;
}

static int o_read(void *c, const struct hl_path *p, uint32_t off, uint8_t *buf, uint32_t n,
		  uint32_t *got)
{
	const uint8_t *d;
	uint32_t len;

	(void)c;
	*got = 0;
	if (p->root == HL_ROOT_USER)
		return hlfat_read(&U, p->rest, off, buf, n, got);
	if (p->root == HL_ROOT_OS) {
		if (os_part(p) == 0u)
			return HL_E_NOT_FOUND;
		d = S.os[os_part(p) - 1u];
		len = S.os_len[os_part(p) - 1u];
	} else {
		const struct sfile *f = find(p->full);

		if (f == NULL || f->dir)
			return HL_E_NOT_FOUND;
		d = f->data;
		len = f->len;
	}
	if (off < len) {
		*got = len - off < n ? len - off : n;
		memcpy(buf, d + off, *got);
	}
	return HL_OK;
}

static int o_hash(void *c, const struct hl_path *p, uint8_t sha[32], uint32_t *size)
{
	static uint8_t buf[4096];
	br_sha256_context x;
	uint32_t got;
	int rc;

	br_sha256_init(&x);
	*size = 0;
	do {
		rc = o_read(c, p, *size, buf, sizeof(buf), &got);
		br_sha256_update(&x, buf, got);
		*size += got;
	} while (rc == HL_OK && got == sizeof(buf));
	br_sha256_out(&x, sha);
	return rc;
}

static int put_user;

static int o_create(void *c, const struct hl_path *p)
{
	struct sfile *f = find(p->full);

	(void)c;
	put_user = p->root == HL_ROOT_USER;
	if (put_user)
		return hlfat_create(&U, p->rest);
	if (f == NULL)
		f = make(p->full, 0);
	if (f == NULL)
		return HL_E_IO;
	f->len = 0;
	S.writing = (int)(f - S.f);
	return HL_OK;
}

static int o_write(void *c, const uint8_t *b, uint32_t n)
{
	struct sfile *f;
	int rc;

	(void)c;
	if (put_user) {
		if (S.corrupt && n > 0u) {
			uint8_t t[HL_CHUNK];

			memcpy(t, b, n);
			t[n - 1u] ^= 0x01u;
			S.corrupt = 0;
			return hlfat_write(&U, t, n);
		}
		return hlfat_write(&U, b, n);
	}
	if (S.writing < 0)
		return HL_E_BAD;
	f = &S.f[S.writing];
	rc = append(f, b, n);
	if (rc == HL_OK && S.corrupt && n > 0u) {
		f->data[f->len - 1u] ^= 0x01u;
		S.corrupt = 0;
	}
	return rc;
}

static int o_finish(void *c)
{
	(void)c;
	if (put_user)
		return hlfat_finish(&U);
	S.writing = -1;
	return HL_OK;
}

static void o_abandon(void *c)
{
	(void)c;
	if (put_user)
		hlfat_abandon(&U);
	S.writing = -1;
}

static int o_remove(void *c, const struct hl_path *p)
{
	struct sfile *f = find(p->full);
	const size_t n = strlen(p->full);

	(void)c;
	if (p->root == HL_ROOT_USER)
		return hlfat_remove(&U, p->rest);
	if (f == NULL)
		return HL_E_NOT_FOUND;
	for (int i = 0; f->dir && i < NFILES; i++)
		if (S.f[i].used && strncmp(S.f[i].path, p->full, n) == 0 && S.f[i].path[n] == '/')
			return HL_E_REFUSED;	/* not empty */
	f->used = 0;
	return HL_OK;
}

static int o_mkdir(void *c, const struct hl_path *p)
{
	(void)c;
	if (p->root == HL_ROOT_USER)
		return hlfat_mkdir(&U, p->rest);
	if (find(p->full) != NULL)
		return HL_E_EXISTS;
	return make(p->full, 1) != NULL ? HL_OK : HL_E_IO;
}

static int o_rename(void *c, const struct hl_path *from, const struct hl_path *to)
{
	struct sfile *f = find(from->full), *t = find(to->full);

	(void)c;
	if (from->root == HL_ROOT_USER)
		return hlfat_rename(&U, from->rest, to->rest);
	if (f == NULL)
		return HL_E_NOT_FOUND;
	if (t != NULL)
		t->used = 0;
	strncpy(f->path, to->full, sizeof(f->path) - 1u);
	return HL_OK;
}

static int o_run(void *c, uint8_t action, uint8_t arg, const struct hl_path *p)
{
	(void)c;
	if (p != NULL && find(p->full) == NULL)
		return HL_E_NOT_FOUND;
	S.run_action = action;
	S.run_arg = arg;
	S.run_path[0] = '\0';
	if (p != NULL)
		strncpy(S.run_path, p->full, sizeof(S.run_path) - 1u);
	S.steps = 0;
	return HL_OK;
}

static int o_set(void *c, uint8_t setting, uint8_t value)
{
	(void)c;
	if (setting == HL_SET_HOST_LINK && value == 0u) {
		S.on = 0;
		hl_end(&H);
	}
	return HL_OK;
}

static bool o_no_hold(void *c)
{
	(void)c;
	return S.no_hold != 0;
}

#define RAM_MAX 0x800000u

static void o_ram_drop(void *c)
{
	(void)c;
	free(S.ram);
	S.ram = NULL;
	S.ram_size = 0;
}

static int o_ram_open(void *c, uint32_t size)
{
	o_ram_drop(c);
	if (size > RAM_MAX)
		return HL_E_REFUSED;
	S.ram = malloc(size);
	if (S.ram == NULL)
		return HL_E_IO;
	S.ram_size = size;
	return HL_OK;
}

static int o_ram_write(void *c, uint32_t off, const uint8_t *b, uint32_t n)
{
	(void)c;
	if (S.ram == NULL || off + n > S.ram_size)
		return HL_E_BAD;
	memcpy(S.ram + off, b, n);
	if (S.corrupt && n > 0u) {
		S.ram[off + n - 1u] ^= 1u;	/* not the signature: the hash finds it */
		S.corrupt = 0;
	}
	return HL_OK;
}

static int o_ram_end(void *c, uint32_t size, uint8_t sha[32], uint8_t *why)
{
	br_sha256_context x;

	(void)c;
	br_sha256_init(&x);
	br_sha256_update(&x, S.ram, size);
	br_sha256_out(&x, sha);
	*why = S.ram[0] == 0xADu ? 0u : 2u;	/* S05_REFUSE_PARSE */
	return *why == 0u ? HL_OK : HL_E_REFUSED;
}

static const struct hl_ops ops = {
	.link_on = o_on, .approve_open = o_approve, .info = o_info, .root_ok = o_root_ok,
	.stat = o_stat, .list = o_list, .read = o_read, .hash = o_hash,
	.create = o_create, .write = o_write, .finish = o_finish, .abandon = o_abandon,
	.remove = o_remove, .mkdir = o_mkdir, .rename = o_rename, .run = o_run, .set = o_set,
	.no_hold = o_no_hold, .ram_open = o_ram_open, .ram_write = o_ram_write,
	.ram_end = o_ram_end, .ram_drop = o_ram_drop,
};

/* ---- what the tests call ---- */

void sim_reset(int on, int user);
size_t sim_request(const uint8_t *req, size_t n, uint8_t *out);
int sim_pending(void);
void sim_answer(int outcome);
size_t sim_step(uint8_t *out);
int sim_put(const char *path, const uint8_t *d, size_t n, int dir);
long sim_get(const char *path, uint8_t *buf, size_t cap);
void sim_os(unsigned part, const uint8_t *d, size_t n);
void sim_corrupt(void);
int sim_user(int blank);
unsigned sim_user_flushes(void);
void sim_fail_next(void);
int sim_last_run(uint8_t *action, uint8_t *arg, char *path, size_t cap);
int sim_session(void);
void sim_no_hold(int on);
void sim_poll(void);
long sim_ram(uint8_t *buf, size_t cap);

void sim_reset(int on, int user)
{
	for (int i = 0; i < NFILES; i++)
		free(S.f[i].data);
	free(S.ram);
	memset(&S, 0, sizeof(S));
	S.on = on;
	S.user = user;
	S.writing = -1;
	hl_init(&H, &ops);
	(void)make("/yaffs2/lost+found", 1);
}

size_t sim_request(const uint8_t *req, size_t n, uint8_t *out)
{
	return hl_request(&H, req, n, out);
}

int sim_pending(void)
{
	return S.pending;
}

int sim_session(void)
{
	return H.session;
}

void sim_answer(int outcome)
{
	S.pending = 0;
	hl_answer(&H, (uint8_t)outcome);
}

/* A queued job: running at 50 %, then done (or failed once asked); each
 * change's HL_EVENT payload, 0 when nothing moved. */
size_t sim_step(uint8_t *out)
{
	const struct hl_job *j = &H.job;
	bool moved = false;

	if (j->state == HL_J_QUEUED)
		moved = hl_job_set(&H, HL_J_RUNNING, 1, 50, HL_OK, "working");
	else if (j->state == HL_J_RUNNING && S.fail_next)
		moved = hl_job_set(&H, HL_J_FAILED, 1, HL_PERCENT_NONE, HL_E_REFUSED,
				   "PRODUCT: incorrect sku");
	else if (j->state == HL_J_RUNNING)
		moved = hl_job_set(&H, HL_J_DONE, 2, 100, HL_OK, "[done]");
	if (j->state == HL_J_FAILED)
		S.fail_next = 0;
	return moved ? hl_event(&H, out) : 0u;
}

int sim_put(const char *path, const uint8_t *d, size_t n, int dir)
{
	struct sfile *f = find(path);

	if (f == NULL)
		f = make(path, dir);
	if (f == NULL)
		return -1;
	f->len = 0;
	return append(f, d, (uint32_t)n);
}

long sim_get(const char *path, uint8_t *buf, size_t cap)
{
	const struct sfile *f = find(path);

	if (f == NULL)
		return -1;
	if (f->len <= cap)
		memcpy(buf, f->data, f->len);
	return (long)f->len;
}

void sim_os(unsigned part, const uint8_t *d, size_t n)
{
	if (part < 1u || part > 2u || n > sizeof(S.os[0]))
		return;
	memcpy(S.os[part - 1u], d, n);
	S.os_len[part - 1u] = (uint32_t)n;
}

static unsigned flushes;

static int u_flush(void *c)
{
	(void)c;
	flushes++;
	return 0;
}

/* /user's volume built afresh: FAT, or (blank) none FatFs reads. */
int sim_user(int blank)
{
	FRESULT r = FR_OK;

	memset(user_disk, 0, sizeof user_disk);
	memset(&U, 0, sizeof U);
	U.dev = &user_dev;
	U.flush = u_flush;
	flushes = 0;
	if (!blank) {
		static uint8_t work[4096];
		const MKFS_PARM opt = { FM_FAT, 1, 1, 512, 0 };

		wb_fatfs_attach(&user_dev);
		r = f_mkfs("", &opt, work, sizeof work);
		wb_fatfs_attach(NULL);
	}
	return (int)r;
}

unsigned sim_user_flushes(void)
{
	return flushes;
}

void sim_corrupt(void)
{
	S.corrupt = 1;
}

void sim_fail_next(void)
{
	S.fail_next = 1;
}

int sim_last_run(uint8_t *action, uint8_t *arg, char *path, size_t cap)
{
	*action = S.run_action;
	*arg = S.run_arg;
	strncpy(path, S.run_path, cap - 1u);
	path[cap - 1u] = '\0';
	return S.run_action != 0u;
}

void sim_no_hold(int on)
{
	S.no_hold = on;
}

/* The image held in RAM: its length (-1 when none), copied when it fits. */
long sim_ram(uint8_t *buf, size_t cap)
{
	if (S.ram == NULL)
		return -1;
	if (S.ram_size <= cap)
		memcpy(buf, S.ram, S.ram_size);
	return (long)S.ram_size;
}

void sim_poll(void)
{
	hl_poll(&H);
}

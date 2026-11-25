/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The host link's protocol (src/hostlink.c): the path check and its roots,
 * the one classification table, the session's approval, the file calls
 * over a volume in memory (a put's temporary name, its check before the
 * rename), the jobs, and the calls that are never served; an image into
 * RAM and its start (S-10c.135, S-10c.136), and the approval with no hold
 * (S-10c.137).
 */
#include <string.h>

#include "support/check.h"

#include "waffle/hostlink.h"
#include "bearssl_hash.h"

static void wb_put_le32_t(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* ---- a volume in memory, behind the ops ---- */

#define NFILES 16
#define FILE_MAX 2048

struct ffile {
	bool used, dir;
	char path[HL_PATH_MAX + 16];
	uint8_t data[FILE_MAX];
	uint32_t len;
};

static struct {
	bool on, screen_ok;
	unsigned screens, runs, sets;
	uint8_t run_action, run_arg, set_id, set_value;
	struct ffile f[NFILES];
	int writing;			/* the file a put writes, or -1 */
	bool corrupt;			/* the next write lands one byte wrong */
	unsigned renames, removes;
	bool user_root;
	bool no_hold;			/* the Developer item on */
	uint8_t ram_buf[FILE_MAX];
	uint32_t ram_cap, ram_size;
	unsigned ram_opens, ram_drops;
	uint8_t ram_why;		/* S-05.024's refusal, or 0 */
} V;

static struct ffile *find(const char *path)
{
	for (int i = 0; i < NFILES; i++)
		if (V.f[i].used && strcmp(V.f[i].path, path) == 0)
			return &V.f[i];
	return NULL;
}

static struct ffile *make(const char *path, bool dir)
{
	for (int i = 0; i < NFILES; i++)
		if (!V.f[i].used) {
			memset(&V.f[i], 0, sizeof(V.f[i]));
			V.f[i].used = true;
			V.f[i].dir = dir;
			strcpy(V.f[i].path, path);
			return &V.f[i];
		}
	return NULL;
}

static bool o_on(void *ctx)
{
	(void)ctx;
	return V.on;
}

static bool o_approve(void *ctx)
{
	(void)ctx;
	V.screens++;
	return V.screen_ok;
}

static void o_info(void *ctx, uint8_t out[HL_INFO_LEN])
{
	(void)ctx;
	memset(out, 0xA5, HL_INFO_LEN);
}

static bool o_root_ok(void *ctx, unsigned root)
{
	(void)ctx;
	return root != HL_ROOT_USER || V.user_root;
}

static int o_stat(void *ctx, const struct hl_path *p, struct hl_stat *st)
{
	const struct ffile *f = find(p->full);

	(void)ctx;
	if (*p->rest == '\0') {
		st->type = 2;
		st->size = st->mtime = 0;
		return HL_OK;
	}
	if (f == NULL)
		return HL_E_NOT_FOUND;
	st->type = f->dir ? 2u : 1u;
	st->size = f->len;
	st->mtime = 0;
	return HL_OK;
}

static int o_list(void *ctx, const struct hl_path *p, uint32_t start, hl_each_fn each,
		  void *arg, bool *end)
{
	const size_t n = strlen(p->full);
	uint32_t k = 0;

	(void)ctx;
	*end = false;
	for (int i = 0; i < NFILES; i++) {
		const struct ffile *f = &V.f[i];
		struct hl_stat st = { f->dir ? 2u : 1u, f->len, 0 };

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

static int o_read(void *ctx, const struct hl_path *p, uint32_t off, uint8_t *buf,
		  uint32_t n, uint32_t *got)
{
	const struct ffile *f = find(p->full);

	(void)ctx;
	*got = 0;
	if (f == NULL || f->dir)
		return HL_E_NOT_FOUND;
	if (off < f->len) {
		*got = f->len - off < n ? f->len - off : n;
		memcpy(buf, f->data + off, *got);
	}
	return HL_OK;
}

static int o_hash(void *ctx, const struct hl_path *p, uint8_t sha[32], uint32_t *size)
{
	const struct ffile *f = find(p->full);
	br_sha256_context c;

	(void)ctx;
	if (f == NULL)
		return HL_E_NOT_FOUND;
	br_sha256_init(&c);
	br_sha256_update(&c, f->data, f->len);
	br_sha256_out(&c, sha);
	*size = f->len;
	return HL_OK;
}

static int o_create(void *ctx, const struct hl_path *p)
{
	struct ffile *f = find(p->full);

	(void)ctx;
	if (f == NULL)
		f = make(p->full, false);
	if (f == NULL)
		return HL_E_IO;
	f->len = 0;
	V.writing = (int)(f - V.f);
	return HL_OK;
}

static int o_write(void *ctx, const uint8_t *b, uint32_t n)
{
	struct ffile *f;

	(void)ctx;
	if (V.writing < 0)
		return HL_E_BAD;
	f = &V.f[V.writing];
	if (f->len + n > FILE_MAX)
		return HL_E_IO;
	memcpy(f->data + f->len, b, n);
	if (V.corrupt && n > 0u) {
		f->data[f->len] ^= 1u;
		V.corrupt = false;
	}
	f->len += n;
	return HL_OK;
}

static int o_finish(void *ctx)
{
	(void)ctx;
	V.writing = -1;
	return HL_OK;
}

static void o_abandon(void *ctx)
{
	(void)ctx;
	V.writing = -1;
}

static int o_remove(void *ctx, const struct hl_path *p)
{
	struct ffile *f = find(p->full);

	(void)ctx;
	V.removes++;
	if (f == NULL)
		return HL_E_NOT_FOUND;
	f->used = false;
	return HL_OK;
}

static int o_mkdir(void *ctx, const struct hl_path *p)
{
	(void)ctx;
	if (find(p->full) != NULL)
		return HL_E_EXISTS;
	return make(p->full, true) != NULL ? HL_OK : HL_E_IO;
}

static int o_rename(void *ctx, const struct hl_path *from, const struct hl_path *to)
{
	struct ffile *f = find(from->full), *t = find(to->full);

	(void)ctx;
	V.renames++;
	if (f == NULL)
		return HL_E_NOT_FOUND;
	if (t != NULL)
		t->used = false;
	strcpy(f->path, to->full);
	return HL_OK;
}

static int o_run(void *ctx, uint8_t action, uint8_t arg, const struct hl_path *p)
{
	(void)ctx;
	(void)p;
	V.runs++;
	V.run_action = action;
	V.run_arg = arg;
	return HL_OK;
}

static int o_set(void *ctx, uint8_t setting, uint8_t value)
{
	(void)ctx;
	V.sets++;
	V.set_id = setting;
	V.set_value = value;
	return HL_OK;
}

static bool o_no_hold(void *ctx)
{
	(void)ctx;
	return V.no_hold;
}

static int o_ram_open(void *ctx, uint32_t size)
{
	(void)ctx;
	V.ram_opens++;
	if (size > V.ram_cap)
		return HL_E_REFUSED;
	V.ram_size = size;
	return HL_OK;
}

static int o_ram_write(void *ctx, uint32_t off, const uint8_t *b, uint32_t n)
{
	(void)ctx;
	if (off + n > V.ram_size)
		return HL_E_IO;
	memcpy(V.ram_buf + off, b, n);
	if (V.corrupt && n > 0u) {
		V.ram_buf[off] ^= 1u;
		V.corrupt = false;
	}
	return HL_OK;
}

static int o_ram_end(void *ctx, uint32_t size, uint8_t sha[32], uint8_t *why)
{
	br_sha256_context c;

	(void)ctx;
	br_sha256_init(&c);
	br_sha256_update(&c, V.ram_buf, size);
	br_sha256_out(&c, sha);
	*why = V.ram_why;
	return V.ram_why != 0u ? HL_E_REFUSED : HL_OK;
}

static void o_ram_drop(void *ctx)
{
	(void)ctx;
	V.ram_drops++;
}

static const struct hl_ops ops = {
	.link_on = o_on, .approve_open = o_approve, .info = o_info, .root_ok = o_root_ok,
	.stat = o_stat, .list = o_list, .read = o_read, .hash = o_hash,
	.create = o_create, .write = o_write, .finish = o_finish, .abandon = o_abandon,
	.remove = o_remove, .mkdir = o_mkdir, .rename = o_rename, .run = o_run, .set = o_set,
	.no_hold = o_no_hold, .ram_open = o_ram_open, .ram_write = o_ram_write,
	.ram_end = o_ram_end, .ram_drop = o_ram_drop,
};

/* The same, as a build with no RAM image and no Developer item. */
static const struct hl_ops ops_plain = {
	.link_on = o_on, .approve_open = o_approve, .info = o_info, .root_ok = o_root_ok,
	.stat = o_stat, .list = o_list, .read = o_read, .hash = o_hash,
	.create = o_create, .write = o_write, .finish = o_finish, .abandon = o_abandon,
	.remove = o_remove, .mkdir = o_mkdir, .rename = o_rename, .run = o_run, .set = o_set,
};

static struct hostlink H;
static uint8_t out[HL_PAYLOAD];
static size_t out_n;

static void fresh(void)
{
	memset(&V, 0, sizeof(V));
	V.on = true;
	V.screen_ok = true;
	V.writing = -1;
	V.ram_cap = FILE_MAX;
	hl_init(&H, &ops);
}

/* A request: call, tag 0x5A, then the arguments; the reply's status. */
static int req(uint8_t call, const void *args, size_t n)
{
	uint8_t r[HL_PAYLOAD];

	r[0] = call;
	r[1] = 0x5A;
	if (n != 0u)
		memcpy(r + 2, args, n);
	out_n = hl_request(&H, r, n + 2u, out);
	CHECK(out_n >= 3u && out[0] == call && out[1] == 0x5A);
	return out[2];
}

static int req_str(uint8_t call, const char *s)
{
	return req(call, s, strlen(s));
}

/* Whether the reply holds these bytes. */
static bool holds(const char *s, size_t k)
{
	for (size_t i = 0; i + k <= out_n; i++)
		if (memcmp(out + i, s, k) == 0)
			return true;
	return false;
}

static void approve(void)
{
	CHECK(req(HL_APPROVE, NULL, 0) == HL_OK && out[3] == HL_S_PENDING);
	hl_answer(&H, HL_S_APPROVED);
	CHECK(hl_approved(&H));
}

/* ---- the path check ---- */

static int parse(const char *s, struct hl_path *p)
{
	return hl_path_parse((const uint8_t *)s, strlen(s), p);
}

static void test_paths(void)
{
	static const char *const good[] = {
		"/yaffs2", "/yaffs2/os.ldr", "/yaffs2/a/b/c.txt", "/yaffs2/.hidden",
		"/yaffs2/..x", "/yaffs2/x..", "/os", "/os/boot1", "/user", "/user/A B/c",
		"/yaffs2/\xC3\xA9t\xC3\xA9",
	};
	static const char *const bad[] = {
		"", "/", "yaffs2/a", "//yaffs2", "/yaffs2/", "/yaffs2//a", "/yaffs2/.",
		"/yaffs2/..", "/yaffs2/../os/boot1", "/yaffs2/a/../../os", "/yaffs2/./a",
		"/yaffs2x", "/yaffs2x/a", "/yaff", "/os/../yaffs2/a", "/user/..",
		"/user/../yaffs2", "/yaffs2/a\\b", "/yaffs2/c:d", "/user/0:x",
		"/yaffs2/a\tb", "/yaffs2/a\x7F", "/flash", "/mem/0", "/YAFFS2/a",
	};
	struct hl_path p;
	char long_path[HL_PATH_MAX + 2];
	char long_name[300];

	for (size_t i = 0; i < ARRAY_LEN(good); i++)
		CHECK(parse(good[i], &p) == HL_OK);
	for (size_t i = 0; i < ARRAY_LEN(bad); i++)
		CHECK(parse(bad[i], &p) == HL_E_PATH);
	/* a NUL inside is a byte below 0x20 */
	CHECK(hl_path_parse((const uint8_t *)"/yaffs2/a\0b", 11, &p) == HL_E_PATH);
	/* the root and the rest of each root */
	CHECK(parse("/yaffs2/a/b", &p) == HL_OK && p.root == HL_ROOT_DRIVE &&
	      strcmp(p.full, "/yaffs2/a/b") == 0 && strcmp(p.rest, "a/b") == 0);
	CHECK(parse("/yaffs2", &p) == HL_OK && *p.rest == '\0');
	CHECK(parse("/os/boot2", &p) == HL_OK && p.root == HL_ROOT_OS &&
	      strcmp(p.rest, "boot2") == 0);
	CHECK(parse("/user/DCIM/x.wav", &p) == HL_OK && p.root == HL_ROOT_USER &&
	      strcmp(p.rest, "DCIM/x.wav") == 0);
	/* the length: HL_PATH_MAX in all, 255 a component */
	memset(long_path, 'a', sizeof(long_path));
	memcpy(long_path, "/yaffs2/", 8);
	CHECK(hl_path_parse((const uint8_t *)long_path, HL_PATH_MAX, &p) == HL_OK);
	CHECK(hl_path_parse((const uint8_t *)long_path, HL_PATH_MAX + 1u, &p) == HL_E_PATH);
	memset(long_name, 'b', sizeof(long_name));
	memcpy(long_name, "/user/", 6);
	CHECK(hl_path_parse((const uint8_t *)long_name, 6u + 255u, &p) == HL_E_PATH);
	(void)long_name;
}

/* ---- the classification ---- */

static void test_table(void)
{
	static const uint8_t needs[] = { HL_GET, HL_HASH, HL_PUT_OPEN, HL_PUT_DATA,
					 HL_PUT_END, HL_REMOVE, HL_MKDIR, HL_RENAME,
					 HL_RUN, HL_SET, HL_RAM_OPEN, HL_RAM_DATA,
					 HL_RAM_END };
	static const uint8_t safe[] = { HL_HELLO, HL_SESSION, HL_APPROVE, HL_END,
					HL_INFO, HL_JOB };

	/* every id once in each table */
	for (size_t i = 0; i < hl_ncalls; i++)
		for (size_t j = i + 1u; j < hl_ncalls; j++)
			CHECK(hl_calls[i].id != hl_calls[j].id);
	for (size_t i = 0; i < hl_nactions; i++)
		for (size_t j = i + 1u; j < hl_nactions; j++)
			CHECK(hl_actions[i].id != hl_actions[j].id);
	for (size_t i = 0; i < hl_nsettings; i++)
		for (size_t j = i + 1u; j < hl_nsettings; j++)
			CHECK(hl_settings[i].id != hl_settings[j].id);
	CHECK(hl_ncalls == ARRAY_LEN(needs) + ARRAY_LEN(safe) + 2u);
	for (size_t i = 0; i < ARRAY_LEN(needs); i++)
		CHECK(hl_class_of(hl_calls, hl_ncalls, needs[i]) == HL_NEEDS);
	for (size_t i = 0; i < ARRAY_LEN(safe); i++)
		CHECK(hl_class_of(hl_calls, hl_ncalls, safe[i]) == HL_SAFE);
	CHECK(hl_class_of(hl_calls, hl_ncalls, HL_STAT) == HL_BY_ROOT);
	CHECK(hl_class_of(hl_calls, hl_ncalls, HL_LIST) == HL_BY_ROOT);
	CHECK(hl_class_of(hl_calls, hl_ncalls, HL_EVENT) == 0xFFu);
	/* never: the factory reset and Protected writes, as an action and a setting */
	CHECK(hl_class_of(hl_actions, hl_nactions, HL_A_FACTORY_RESET) == HL_NEVER_C);
	CHECK(hl_class_of(hl_actions, hl_nactions, HL_A_PROTECTED_WRITES) == HL_NEVER_C);
	CHECK(hl_class_of(hl_settings, hl_nsettings, HL_SET_PROTECTED) == HL_NEVER_C);
	/* S-10c.137: the install alone takes the hold whatever the Developer
	 * item says */
	for (size_t i = 0; i < hl_nactions; i++)
		CHECK(hl_actions[i].cls == HL_NEVER_C ||
		      hl_actions[i].cls == (hl_actions[i].id == HL_A_INSTALL ? HL_HOLD :
									    HL_NEEDS));
	CHECK(hl_class_of(hl_actions, hl_nactions, HL_A_RAM_START) == HL_NEEDS);
	/* the roots: only the drive lists openly; the OS copies take no write */
	CHECK(hl_roots[HL_ROOT_DRIVE].flags == (HL_RF_READ | HL_RF_WRITE | HL_RF_OPEN));
	CHECK(hl_roots[HL_ROOT_OS].flags == HL_RF_READ);
	CHECK(hl_roots[HL_ROOT_USER].flags == (HL_RF_READ | HL_RF_WRITE));
}

/* ---- the session ---- */

static void test_hello_and_off(void)
{
	fresh();
	CHECK(req(HL_HELLO, (const uint8_t[]){ 1 }, 1) == HL_OK);
	CHECK(out[3] == 0x57 && out[4] == 0x42 && out[5] == 0x48 && out[6] == 0x4C &&
	      out[7] == HL_VERSION && out[8] == HL_MIN_CLIENT && out[9] == 1u &&
	      out[10] == HL_S_NONE && out[11] == HL_PAYLOAD && out[12] == HL_CHUNK);
	CHECK(out[13 + HL_INFO_LEN] == hl_ncalls);
	/* the roots this build offers: without user, two */
	CHECK(out[out_n - 1u] == '\0' && holds("yaffs2", 7) &&
	      !holds("user", 5));
	V.user_root = true;
	CHECK(req(HL_HELLO, NULL, 0) == HL_OK && holds("user", 5));
	/* the actions listed leave the never ones out */
	{
		const size_t a = 13u + HL_INFO_LEN + 1u + hl_ncalls;

		CHECK(out[a] == 7u);
		for (size_t i = 0; i < out[a]; i++)
			CHECK(out[a + 1u + i] != HL_A_FACTORY_RESET &&
			      out[a + 1u + i] != HL_A_PROTECTED_WRITES);
	}
	/* off: HELLO says so and no more; everything else is HL_E_OFF */
	V.on = false;
	CHECK(req(HL_HELLO, NULL, 0) == HL_OK && out_n == 3u + 7u && out[9] == 0u);
	CHECK(req(HL_SESSION, NULL, 0) == HL_E_OFF && out_n == 3u);
	CHECK(req(HL_APPROVE, NULL, 0) == HL_E_OFF && V.screens == 0u);
	CHECK(req_str(HL_STAT, "/yaffs2") == HL_E_OFF);
	CHECK(req(HL_INFO, NULL, 0) == HL_E_OFF);
	/* malformed: no tag */
	out_n = hl_request(&H, (const uint8_t[]){ HL_INFO }, 1, out);
	CHECK(out_n == 3u && out[2] == HL_E_BAD);
	/* an unknown call: told apart from a refusal */
	V.on = true;
	CHECK(req(0x6E, NULL, 0) == HL_E_UNKNOWN);
	CHECK(req(HL_EVENT, NULL, 0) == HL_E_UNKNOWN);
	/* a build with no RAM image lists neither its calls nor its action
	 * (S-10c.135) and serves neither */
	hl_init(&H, &ops_plain);
	CHECK(req(HL_HELLO, NULL, 0) == HL_OK && out[13 + HL_INFO_LEN] == hl_ncalls - 3u);
	{
		const size_t a = 13u + HL_INFO_LEN + 1u + hl_ncalls - 3u;

		CHECK(out[a] == 6u);
		for (size_t i = 0; i < out[a]; i++)
			CHECK(out[a + 1u + i] != HL_A_RAM_START);
	}
	approve();
	CHECK(req(HL_RAM_END, NULL, 0) == HL_E_UNKNOWN);
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RAM_START }, 1) == HL_E_UNKNOWN);
	hl_init(&H, &ops);
}

static void test_approval(void)
{
	fresh();
	/* unapproved: the one distinct error, and nothing done */
	make("/yaffs2/a", false)->len = 3;
	CHECK(req_str(HL_REMOVE, "/yaffs2/a") == HL_E_APPROVAL && find("/yaffs2/a") != NULL);
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RESTART }, 1) == HL_E_APPROVAL && V.runs == 0u);
	CHECK(req(HL_SET, (const uint8_t[]){ HL_SET_HOST_LINK, 0 }, 2) == HL_E_APPROVAL &&
	      V.sets == 0u);
	/* a screen that cannot open now */
	V.screen_ok = false;
	CHECK(req(HL_APPROVE, NULL, 0) == HL_E_BUSY && H.session == HL_S_NONE);
	/* asked: pending, asked once however often the host asks */
	V.screen_ok = true;
	CHECK(req(HL_APPROVE, NULL, 0) == HL_OK && out[3] == HL_S_PENDING && V.screens == 2u);
	CHECK(req(HL_APPROVE, NULL, 0) == HL_OK && out[3] == HL_S_PENDING && V.screens == 2u);
	CHECK(req_str(HL_REMOVE, "/yaffs2/a") == HL_E_APPROVAL);
	/* refused: kept to be read; nothing approved */
	hl_answer(&H, HL_S_REFUSED);
	CHECK(req(HL_SESSION, NULL, 0) == HL_OK && out[3] == HL_S_REFUSED);
	CHECK(req_str(HL_REMOVE, "/yaffs2/a") == HL_E_APPROVAL);
	/* an answer with no request pending changes nothing */
	hl_answer(&H, HL_S_APPROVED);
	CHECK(!hl_approved(&H));
	/* asked again: timed out */
	CHECK(req(HL_APPROVE, NULL, 0) == HL_OK && V.screens == 3u);
	hl_answer(&H, HL_S_TIMEOUT);
	CHECK(req(HL_SESSION, NULL, 0) == HL_OK && out[3] == HL_S_TIMEOUT && !hl_approved(&H));
	/* allowed */
	CHECK(req(HL_APPROVE, NULL, 0) == HL_OK);
	hl_answer(&H, HL_S_APPROVED);
	CHECK(req(HL_APPROVE, NULL, 0) == HL_OK && out[3] == HL_S_APPROVED && V.screens == 4u);
	CHECK(req_str(HL_REMOVE, "/yaffs2/a") == HL_OK && find("/yaffs2/a") == NULL);
	/* the setting off ends it */
	V.on = false;
	hl_poll(&H);
	V.on = true;
	CHECK(H.session == HL_S_NONE && !hl_approved(&H));
	CHECK(req_str(HL_MKDIR, "/yaffs2/d") == HL_E_APPROVAL);
	/* and with the setting off, an approved session serves nothing */
	approve();
	V.on = false;
	CHECK(!hl_approved(&H));
	V.on = true;
	/* HL_END ends it */
	CHECK(req(HL_END, NULL, 0) == HL_OK && H.session == HL_S_NONE);
	/* a restart: a new start's state is a new session */
	approve();
	hl_init(&H, &ops);
	CHECK(!hl_approved(&H) && req_str(HL_MKDIR, "/yaffs2/d") == HL_E_APPROVAL);
}

/* The factory reset and Protected writes: refused, approved or not. */
static void test_never(void)
{
	fresh();
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_FACTORY_RESET }, 1) == HL_E_NEVER);
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_PROTECTED_WRITES }, 1) == HL_E_NEVER);
	CHECK(req(HL_SET, (const uint8_t[]){ HL_SET_PROTECTED, 1 }, 2) == HL_E_NEVER);
	approve();
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_FACTORY_RESET }, 1) == HL_E_NEVER);
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_PROTECTED_WRITES, 0 }, 2) == HL_E_NEVER);
	CHECK(req(HL_SET, (const uint8_t[]){ HL_SET_PROTECTED, 1 }, 2) == HL_E_NEVER);
	CHECK(req(HL_SET, (const uint8_t[]){ HL_SET_PROTECTED, 0 }, 2) == HL_E_NEVER);
	CHECK(V.runs == 0u && V.sets == 0u);
	/* the setting a host may change, approved */
	CHECK(req(HL_SET, (const uint8_t[]){ HL_SET_HOST_LINK, 0 }, 2) == HL_OK &&
	      V.sets == 1u && V.set_id == HL_SET_HOST_LINK && V.set_value == 0u);
	CHECK(req(HL_SET, (const uint8_t[]){ 3, 1 }, 2) == HL_E_UNKNOWN);
	CHECK(req(HL_RUN, (const uint8_t[]){ 0x7E }, 1) == HL_E_UNKNOWN);
}

/* ---- the file calls ---- */

static void test_reads(void)
{
	struct ffile *f;
	uint8_t a[64];

	fresh();
	f = make("/yaffs2/big", false);
	for (unsigned i = 0; i < 600u; i++)
		f->data[i] = (uint8_t)i;
	f->len = 600;
	make("/yaffs2/sub", true);
	make("/yaffs2/sub/x", false)->len = 5;
	/* stat and list of the drive: open */
	CHECK(req_str(HL_STAT, "/yaffs2/big") == HL_OK && out[3] == 1u &&
	      out[4] == 0x58 && out[5] == 0x02 && out_n == 12u);
	CHECK(req_str(HL_STAT, "/yaffs2/none") == HL_E_NOT_FOUND);
	memcpy(a, "\0\0/yaffs2", 9);
	CHECK(req(HL_LIST, a, 9) == HL_OK && out[3] == 0xFF && out[4] == 0xFF);
	CHECK(out_n == 5u + (6u + 3u) + (6u + 3u));
	/* from the second entry */
	a[0] = 1;
	CHECK(req(HL_LIST, a, 9) == HL_OK && out_n == 5u + 9u && out[5] == 2u);
	/* the other roots: approval even to list or stat */
	CHECK(req_str(HL_STAT, "/os/boot1") == HL_E_APPROVAL);
	memcpy(a, "\0\0/os", 5);
	CHECK(req(HL_LIST, a, 5) == HL_E_APPROVAL);
	CHECK(req_str(HL_STAT, "/user/x") == HL_E_PATH);	/* not on this device */
	V.user_root = true;
	CHECK(req_str(HL_STAT, "/user/x") == HL_E_APPROVAL);
	/* get and hash need it on the drive too */
	memcpy(a, "\0\0\0\0\0/yaffs2/big", 16);
	CHECK(req(HL_GET, a, 16) == HL_E_APPROVAL);
	CHECK(req_str(HL_HASH, "/yaffs2/big") == HL_E_APPROVAL);
	approve();
	/* chunked, by offset: a whole chunk, then the rest, then nothing */
	CHECK(req(HL_GET, a, 16) == HL_OK && out_n == 7u + HL_CHUNK && out[7] == 0u);
	a[0] = 0x20;
	a[1] = 0x02;			/* 544 */
	a[4] = 100;
	CHECK(req(HL_GET, a, 16) == HL_OK && out_n == 7u + 56u && out[3] == 0x20 &&
	      out[4] == 0x02 && out[7] == (uint8_t)544u);
	a[1] = 0x03;
	CHECK(req(HL_GET, a, 16) == HL_OK && out_n == 7u);
	CHECK(req_str(HL_HASH, "/yaffs2/big") == HL_OK && out_n == 3u + 36u &&
	      out[3] == 0x58 && out[4] == 0x02);
	CHECK(req(HL_GET, a, 4) == HL_E_BAD);
}

static void sha_of(const uint8_t *d, size_t n, uint8_t sha[32])
{
	br_sha256_context c;

	br_sha256_init(&c);
	br_sha256_update(&c, d, n);
	br_sha256_out(&c, sha);
}

/* PUT_OPEN size:4 sha:32 path */
static int put_open(const char *path, uint32_t size, const uint8_t sha[32])
{
	uint8_t a[HL_PAYLOAD];
	const size_t n = strlen(path);

	memcpy(a, &size, 4);		/* the host is little-endian too */
	memcpy(a + 4, sha, 32);
	memcpy(a + 36, path, n);
	return req(HL_PUT_OPEN, a, 36u + n);
}

static int put_data(uint32_t off, const uint8_t *d, size_t n)
{
	uint8_t a[HL_PAYLOAD];

	memcpy(a, &off, 4);
	memcpy(a + 4, d, n);
	return req(HL_PUT_DATA, a, 4u + n);
}

static void test_put(void)
{
	uint8_t data[500], sha[32], wrong[32];
	struct ffile *old;

	for (unsigned i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 7u);
	sha_of(data, sizeof(data), sha);
	memset(wrong, 0, sizeof(wrong));

	fresh();
	CHECK(put_open("/yaffs2/os.ldr", 500, sha) == HL_E_APPROVAL);
	approve();
	old = make("/yaffs2/os.ldr", false);
	old->len = 1;
	old->data[0] = 0xEE;
	/* to the temporary name; the old file stays until the rename */
	CHECK(put_open("/yaffs2/os.ldr", 500, sha) == HL_OK && out_n == 7u);
	CHECK(find("/yaffs2/os.ldr" HL_PUT_SUFFIX) != NULL);
	/* the put's own paths, their rests inside them */
	CHECK(strcmp(H.put.path.rest, "os.ldr") == 0 && H.put.path.rest > H.put.path.full &&
	      H.put.path.rest < H.put.path.full + sizeof H.put.path.full);
	CHECK(strcmp(H.put.tmp.rest, "os.ldr" HL_PUT_SUFFIX) == 0 &&
	      H.put.tmp.rest > H.put.tmp.full && H.put.tmp.rest < H.put.tmp.full + sizeof H.put.tmp.full);
	CHECK(put_data(0, data, 200) == HL_OK && out[3] == 200u);
	/* a wrong offset: the offset held, for a resume */
	CHECK(put_data(100, data, 10) == HL_E_OFFSET && out[3] == 200u && out_n == 7u);
	CHECK(put_data(200, data + 200, HL_CHUNK + 1u) == HL_E_BAD);
	/* the same open again resumes where it stands */
	CHECK(put_open("/yaffs2/os.ldr", 500, sha) == HL_OK && out[3] == 200u);
	CHECK(put_data(200, data + 200, 200) == HL_OK);
	/* ended short */
	CHECK(req(HL_PUT_END, NULL, 0) == HL_E_OFFSET && out[3] == (uint8_t)400u);
	CHECK(put_data(400, data + 400, 101) == HL_E_BAD);	/* past the size */
	CHECK(put_data(400, data + 400, 100) == HL_OK);
	CHECK(find("/yaffs2/os.ldr")->len == 1u);
	CHECK(req(HL_PUT_END, NULL, 0) == HL_OK && V.renames == 1u);
	CHECK(find("/yaffs2/os.ldr" HL_PUT_SUFFIX) == NULL);
	CHECK(find("/yaffs2/os.ldr")->len == 500u &&
	      memcmp(find("/yaffs2/os.ldr")->data, data, 500) == 0);
	CHECK(req(HL_PUT_END, NULL, 0) == HL_E_BAD);		/* none open */

	/* a byte wrong on the way: the check refuses, the temporary file
	 * goes, the old file stays */
	V.corrupt = true;
	CHECK(put_open("/yaffs2/os.ldr", 500, sha) == HL_OK);
	CHECK(put_data(0, data, 224) == HL_OK && put_data(224, data + 224, 224) == HL_OK &&
	      put_data(448, data + 448, 52) == HL_OK);
	CHECK(req(HL_PUT_END, NULL, 0) == HL_E_CHECKSUM && V.renames == 1u);
	CHECK(find("/yaffs2/os.ldr" HL_PUT_SUFFIX) == NULL &&
	      memcmp(find("/yaffs2/os.ldr")->data, data, 500) == 0);
	/* the declared hash wrong: refused the same */
	CHECK(put_open("/yaffs2/new", 500, wrong) == HL_OK);
	CHECK(put_data(0, data, 224) == HL_OK && put_data(224, data + 224, 224) == HL_OK &&
	      put_data(448, data + 448, 52) == HL_OK);
	CHECK(req(HL_PUT_END, NULL, 0) == HL_E_CHECKSUM && find("/yaffs2/new") == NULL &&
	      find("/yaffs2/new" HL_PUT_SUFFIX) == NULL);

	/* the OS copies take no write; the user drive does; never a root itself */
	CHECK(put_open("/os/boot1", 500, sha) == HL_E_READ_ONLY);
	V.user_root = true;
	CHECK(put_open("/user/x", 500, sha) == HL_OK && find("/user/x" HL_PUT_SUFFIX) != NULL);
	hl_end(&H);
	approve();
	CHECK(put_open("/yaffs2", 500, sha) == HL_E_PATH);
	CHECK(put_open("/yaffs2/../os/boot1", 500, sha) == HL_E_PATH);
	/* the session's end abandons a put under way and removes its file */
	CHECK(put_open("/yaffs2/half", 500, sha) == HL_OK && put_data(0, data, 10) == HL_OK);
	hl_end(&H);
	CHECK(find("/yaffs2/half" HL_PUT_SUFFIX) == NULL && find("/yaffs2/half") == NULL);
}

static void test_changes(void)
{
	uint8_t a[64];

	fresh();
	approve();
	make("/yaffs2/a", false);
	make("/yaffs2/b", false);
	CHECK(req_str(HL_MKDIR, "/yaffs2/d") == HL_OK && find("/yaffs2/d")->dir);
	CHECK(req_str(HL_MKDIR, "/yaffs2/d") == HL_E_EXISTS);
	CHECK(req_str(HL_MKDIR, "/yaffs2") == HL_E_PATH);
	CHECK(req_str(HL_REMOVE, "/yaffs2") == HL_E_PATH);
	CHECK(req_str(HL_REMOVE, "/os/boot1") == HL_E_READ_ONLY);
	/* rename: within one root, never across, never over a name */
	memcpy(a, "/yaffs2/a\0/yaffs2/c", 19);
	CHECK(req(HL_RENAME, a, 19) == HL_OK && find("/yaffs2/c") != NULL);
	memcpy(a, "/yaffs2/c\0/yaffs2/b", 19);
	CHECK(req(HL_RENAME, a, 19) == HL_E_EXISTS && find("/yaffs2/c") != NULL);
	memcpy(a, "/yaffs2/c\0/os/boot1", 19);
	CHECK(req(HL_RENAME, a, 19) == HL_E_PATH);
	V.user_root = true;
	make("/user/x", false);
	memcpy(a, "/user/x\0/user/y", 15);
	CHECK(req(HL_RENAME, a, 15) == HL_OK && find("/user/y") != NULL);
	memcpy(a, "/user/y\0/yaffs2/y", 17);
	CHECK(req(HL_RENAME, a, 17) == HL_E_PATH);		/* never across roots */
	memcpy(a, "/yaffs2/c/yaffs2/e", 18);
	CHECK(req(HL_RENAME, a, 18) == HL_E_BAD);		/* no NUL */
	memcpy(a, "/yaffs2/c\0/yaffs2/../os", 23);
	CHECK(req(HL_RENAME, a, 23) == HL_E_PATH);
	CHECK(V.renames == 2u);
}

/* ---- jobs ---- */

static void test_jobs(void)
{
	uint8_t a[64];

	fresh();
	approve();
	/* an action with a file on the drive; the file's root is checked */
	a[0] = HL_A_INSTALL;
	a[1] = 0x03;
	memcpy(a + 2, "/yaffs2/p.tfw", 13);
	CHECK(req(HL_RUN, a, 15) == HL_OK && V.runs == 1u && V.run_action == HL_A_INSTALL &&
	      V.run_arg == 0x03);
	CHECK(out[3] == 1u && out[4] == HL_A_INSTALL && out[5] == HL_J_QUEUED &&
	      out[7] == HL_PERCENT_NONE && out[9] == '\0');
	/* one at a time */
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RESTART }, 1) == HL_E_BUSY);
	CHECK(req(HL_JOB, (const uint8_t[]){ 1 }, 1) == HL_OK && out[5] == HL_J_QUEUED);
	CHECK(req(HL_JOB, (const uint8_t[]){ 2 }, 1) == HL_E_NOT_FOUND);
	/* the record as the board moves it on: an event per change */
	CHECK(hl_job_set(&H, HL_J_RUNNING, 1, 40, HL_OK, "[extracting firmware]"));
	CHECK(!hl_job_set(&H, HL_J_RUNNING, 1, 40, HL_OK, "[extracting firmware]"));
	CHECK(hl_job_set(&H, HL_J_RUNNING, 1, 41, HL_OK, "[extracting firmware]"));
	out_n = hl_event(&H, out);
	CHECK(out[0] == HL_EVENT && out[2] == HL_OK && out[3] == 1u &&
	      out[5] == HL_J_RUNNING && out[7] == 41u &&
	      strcmp((const char *)out + 9, "[extracting firmware]") == 0 &&
	      out_n == 3u + 7u + 21u);
	/* a long text is cut */
	CHECK(hl_job_set(&H, HL_J_FAILED, 3, HL_PERCENT_NONE, HL_E_REFUSED,
			 "a text that is much longer than forty characters in all"));
	CHECK(strlen(H.job.text) == HL_TEXT_MAX);
	/* done: the next may run, with the next id */
	a[0] = HL_A_OS_WRITE;
	memcpy(a + 1, "/os/boot1", 9);
	CHECK(req(HL_RUN, a, 10) == HL_E_PATH);			/* from the drive only */
	memcpy(a + 1, "/yaffs2/os.ldr", 14);
	CHECK(req(HL_RUN, a, 15) == HL_OK && out[3] == 2u && V.runs == 2u);
	(void)hl_job_set(&H, HL_J_DONE, 0, 100, HL_OK, "written");
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RESTART, 9 }, 2) == HL_E_BAD);
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RESTART_MODE }, 1) == HL_E_BAD);
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RESTART_MODE, 2 }, 2) == HL_OK &&
	      V.run_arg == 2u && out[3] == 3u);
	/* the job's progress is safe to read without approval */
	hl_end(&H);
	CHECK(req(HL_JOB, (const uint8_t[]){ 3 }, 1) == HL_OK);
}

/* ---- S-10c.135, S-10c.136: an image into RAM, then its start ---- */

static void ram_send(const uint8_t *img, uint32_t n)
{
	uint8_t a[4u + HL_CHUNK];
	br_sha256_context c;
	uint32_t at = 0;

	br_sha256_init(&c);
	br_sha256_update(&c, img, n);
	wb_put_le32_t(a, n);
	br_sha256_out(&c, a + 4);
	CHECK(req(HL_RAM_OPEN, a, 36) == HL_OK && out_n == 7u);
	while (at < n) {
		const uint32_t k = n - at < HL_CHUNK ? n - at : HL_CHUNK;

		wb_put_le32_t(a, at);
		memcpy(a + 4, img + at, k);
		CHECK(req(HL_RAM_DATA, a, 4u + k) == HL_OK);
		at += k;
		CHECK(out[3] == (uint8_t)at && out[4] == (uint8_t)(at >> 8));
	}
}

static void test_ram(void)
{
	static uint8_t img[1000];
	uint8_t a[4u + HL_CHUNK];

	for (size_t i = 0; i < sizeof img; i++)
		img[i] = (uint8_t)(i * 7u + 3u);
	fresh();
	/* needs the session's approval, as every change does */
	memset(a, 0, sizeof a);
	a[0] = 10;
	CHECK(req(HL_RAM_OPEN, a, 36) == HL_E_APPROVAL && V.ram_opens == 0u);
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RAM_START }, 1) == HL_E_APPROVAL);
	approve();
	/* nothing to start before an image is accepted */
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RAM_START }, 1) == HL_E_NOT_FOUND);
	/* malformed: a short open, an empty image, data or an end with none open */
	CHECK(req(HL_RAM_OPEN, a, 35) == HL_E_BAD);
	memset(a, 0, 4);
	CHECK(req(HL_RAM_OPEN, a, 36) == HL_E_BAD);
	CHECK(req(HL_RAM_DATA, a, 8) == HL_E_BAD);
	CHECK(req(HL_RAM_END, NULL, 0) == HL_E_BAD);
	/* more than the board has room for: its own refusal */
	wb_put_le32_t(a, FILE_MAX + 1u);
	CHECK(req(HL_RAM_OPEN, a, 36) == HL_E_REFUSED && !H.ram.open);
	/* sent whole and checked: kept for the start, no file touched */
	ram_send(img, sizeof img);
	CHECK(req(HL_RAM_END, (const uint8_t[]){ 0 }, 1) == HL_E_BAD);
	CHECK(req(HL_RAM_END, NULL, 0) == HL_OK && H.ram.ready && !H.ram.open);
	CHECK(memcmp(V.ram_buf, img, sizeof img) == 0);
	for (int i = 0; i < NFILES; i++)
		CHECK(!V.f[i].used);
	/* the start: queued as a job like every action, once */
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RAM_START, 0 }, 2) == HL_E_BAD);
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RAM_START }, 1) == HL_OK &&
	      V.run_action == HL_A_RAM_START && out[4] == HL_A_RAM_START &&
	      out[5] == HL_J_QUEUED);
	CHECK(req(HL_RAM_OPEN, a, 36) == HL_E_BUSY);
	(void)hl_job_set(&H, HL_J_FAILED, 0, HL_PERCENT_NONE, HL_E_REFUSED, "not started");
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RAM_START }, 1) == HL_E_NOT_FOUND);

	/* the end before the whole has come: the offset the unit holds */
	ram_send(img, 300);
	{
		uint8_t b[36];
		br_sha256_context c;

		/* resumed: the same size and hash again give the offset held */
		br_sha256_init(&c);
		br_sha256_update(&c, img, 300);
		wb_put_le32_t(b, 300);
		br_sha256_out(&c, b + 4);
		CHECK(req(HL_RAM_OPEN, b, 36) == HL_OK && out[3] == 44u && out[4] == 1u);
	}
	/* a data call at another offset is told the unit's */
	wb_put_le32_t(a, 7);
	CHECK(req(HL_RAM_DATA, a, 10) == HL_E_OFFSET && out[3] == 44u && out[4] == 1u);
	/* more than declared */
	wb_put_le32_t(a, 300);
	CHECK(req(HL_RAM_DATA, a, 4u + 1u) == HL_E_BAD);

	/* a byte wrong on the way: the hash refuses it and it is dropped */
	V.corrupt = true;
	ram_send(img, sizeof img);
	V.ram_drops = 0;
	CHECK(req(HL_RAM_END, NULL, 0) == HL_E_CHECKSUM && !H.ram.ready && !H.ram.open);
	CHECK(V.ram_drops == 1u);
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RAM_START }, 1) == HL_E_NOT_FOUND);
	/* S-05.024 refuses it: why, and dropped */
	V.ram_why = 3;
	ram_send(img, sizeof img);
	CHECK(req(HL_RAM_END, NULL, 0) == HL_E_REFUSED && out_n == 4u && out[3] == 3u &&
	      !H.ram.ready);
	V.ram_why = 0;
	/* the session's end drops an image held */
	ram_send(img, sizeof img);
	CHECK(req(HL_RAM_END, NULL, 0) == HL_OK && H.ram.ready);
	V.ram_drops = 0;
	CHECK(req(HL_END, NULL, 0) == HL_OK && !H.ram.ready && V.ram_drops == 1u);
	/* and the board can take its memory back: no start then */
	approve();
	ram_send(img, sizeof img);
	CHECK(req(HL_RAM_END, NULL, 0) == HL_OK);
	hl_ram_drop(&H);
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RAM_START }, 1) == HL_E_NOT_FOUND);
}

/* ---- S-10c.137: the approval with no hold ---- */

static void test_no_hold(void)
{
	uint8_t a[16];

	fresh();
	/* the item off: the screen, as ever */
	CHECK(req(HL_APPROVE, NULL, 0) == HL_OK && out[3] == HL_S_PENDING && V.screens == 1u);
	CHECK(req(HL_END, NULL, 0) == HL_OK);
	/* on: allowed at once, no screen */
	V.no_hold = true;
	CHECK(req(HL_APPROVE, NULL, 0) == HL_OK && out[3] == HL_S_NO_HOLD && V.screens == 1u);
	CHECK(hl_approved(&H) && !hl_held(&H));
	CHECK(req(HL_SESSION, NULL, 0) == HL_OK && out[3] == HL_S_NO_HOLD);
	CHECK(req(HL_HELLO, NULL, 0) == HL_OK && out[10] == HL_S_NO_HOLD);
	/* what a hold allows, but the install */
	make("/yaffs2/a", false);
	CHECK(req_str(HL_REMOVE, "/yaffs2/a") == HL_OK);
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_RESTART }, 1) == HL_OK);
	(void)hl_job_set(&H, HL_J_DONE, 0, 100, HL_OK, "");
	a[0] = HL_A_INSTALL;
	a[1] = HL_PARTS_ALL;
	memcpy(a + 2, "/yaffs2/p.tfw", 13);
	CHECK(req(HL_RUN, a, 15) == HL_E_APPROVAL && V.runs == 1u);
	/* never stays never */
	CHECK(req(HL_RUN, (const uint8_t[]){ HL_A_FACTORY_RESET }, 1) == HL_E_NEVER);
	CHECK(req(HL_SET, (const uint8_t[]){ HL_SET_PROTECTED, 1 }, 2) == HL_E_NEVER);
	/* APPROVE again asks for the hold the install needs */
	CHECK(req(HL_APPROVE, NULL, 0) == HL_OK && out[3] == HL_S_PENDING && V.screens == 2u);
	hl_answer(&H, HL_S_APPROVED);
	CHECK(hl_held(&H) && req(HL_RUN, a, 15) == HL_OK && V.runs == 2u);
	(void)hl_job_set(&H, HL_J_DONE, 0, 100, HL_OK, "");
	/* a held session is not ended by the item going off */
	V.no_hold = false;
	hl_poll(&H);
	CHECK(H.session == HL_S_APPROVED);
	/* one it allowed is */
	CHECK(req(HL_END, NULL, 0) == HL_OK);
	V.no_hold = true;
	CHECK(req(HL_APPROVE, NULL, 0) == HL_OK && out[3] == HL_S_NO_HOLD);
	hl_poll(&H);
	CHECK(H.session == HL_S_NO_HOLD);
	V.no_hold = false;
	hl_poll(&H);
	CHECK(H.session == HL_S_NONE && !hl_approved(&H));
	CHECK(req_str(HL_MKDIR, "/yaffs2/d") == HL_E_APPROVAL);
	/* and Host link off ends it as any session */
	V.no_hold = true;
	CHECK(req(HL_APPROVE, NULL, 0) == HL_OK && out[3] == HL_S_NO_HOLD);
	V.on = false;
	CHECK(!hl_approved(&H));
	hl_poll(&H);
	V.on = true;
	CHECK(H.session == HL_S_NONE);
	/* a build without the item: the screen */
	hl_init(&H, &ops_plain);
	CHECK(req(HL_APPROVE, NULL, 0) == HL_OK && out[3] == HL_S_PENDING);
}

int main(void)
{
	RUN(test_paths);
	RUN(test_table);
	RUN(test_hello_and_off);
	RUN(test_approval);
	RUN(test_never);
	RUN(test_reads);
	RUN(test_put);
	RUN(test_changes);
	RUN(test_jobs);
	RUN(test_ram);
	RUN(test_no_hold);
	return check_summary("test_hostlink");
}

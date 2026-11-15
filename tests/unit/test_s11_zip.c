/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-11 zip checks 16 and 17 and staging. Checks S-11.020, S-11.030, S-11.031,
 * S-11.032, S-11.033, S-11.034, S-11.035 (and the SC-11-STAGE and SC-11-SPACE
 * legs on an earlier staging area). The zip reader is the vendored one: the test
 * plays it from member tables Python's zipfile made of the published zips.
 */
#include <stdio.h>
#include <string.h>

#include "waffle/s11_engine.h"
#include "waffle/s11_staging.h"

#include "s11_check.h"
#include "s11_ref.h"
#include "vectors/s11_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define B(r) (s11v_blob + (r).off)

struct staged_file {
	char name[S11_PATH_MAX];
	uint64_t size;
};

struct fake {
	const struct s11v_zip *z;
	struct staged_file files[32];
	size_t nfiles, cur;		/* cur: the file open */
	int open, write_fails;
	unsigned resets, mkdirs, writes_before_reset, reads_before_reset;
};

static struct fake F;

/* An engine behind the CRC-32 (s11_engine.h): while `wrong`, every CRC-32
 * comes back with its top bit flipped; crc32_again() replaces it with the
 * software when `replaceable`, and stays so with `always`. */
struct engine_model {
	bool wrong, replaceable, always;
	unsigned asked, extracts;
};

static struct engine_model E;

/* The callbacks reach the fake through F: the ops signatures fix ctx. */
static bool f_member(void *ctx, size_t i, struct s11_member *m)
{
	const struct fake *f = &F;
	const struct s11v_member *v;

	(void)ctx;
	F.reads_before_reset += F.resets == 0 ? 1u : 0u;
	if (i >= f->z->count)
		return false;
	v = &s11v_members[f->z->first + i];
	memset(m, 0, sizeof(*m));
	snprintf(m->name, sizeof(m->name), "%s", v->name);
	m->method = (uint16_t)v->method;
	m->flags = (uint16_t)v->flags;
	m->size = v->size;
	m->crc32 = v->crc;
	m->zip64 = v->zip64_extra != 0;
	return true;
}

static bool f_zip64_end(void *ctx)
{
	(void)ctx;
	F.reads_before_reset += F.resets == 0 ? 1u : 0u;
	return F.z->zip64_end != 0;
}

static bool f_extract(void *ctx, size_t i, s11_sink_fn sink, void *arg)
{
	const struct fake *f = &F;
	const struct s11v_member *v = &s11v_members[f->z->first + i];
	const uint8_t *r = B(v->runs);
	uint8_t chunk[4096];

	(void)ctx;
	E.extracts++;
	if (!v->data_ok)
		return false;
	for (size_t k = 0; k + 5 <= v->runs.len; k += 5) {
		uint32_t count = (uint32_t)r[k + 1] << 24 | (uint32_t)r[k + 2] << 16 |
				 (uint32_t)r[k + 3] << 8 | r[k + 4];

		memset(chunk, r[k], sizeof(chunk));
		while (count != 0) {
			size_t n = count < sizeof(chunk) ? count : sizeof(chunk);

			if (!sink(arg, chunk, n))
				return false;
			count -= (uint32_t)n;
		}
	}
	return true;
}

static uint32_t f_crc32(void *ctx, uint32_t crc, const uint8_t *p, size_t n)
{
	(void)ctx;
	if (E.wrong)
		crc ^= 0x80000000u;
	crc = ref_crc32(crc, p, n);
	return E.wrong ? crc ^ 0x80000000u : crc;
}

static bool e_again(void)
{
	E.asked++;
	if (!E.replaceable)
		return false;
	E.wrong = false;
	E.replaceable = E.always;
	return true;
}

static void e_refused(void)
{
}

static const struct s11_crc_engine engine = { e_again, e_refused };

static bool f_free(void *ctx, uint64_t *bytes)
{
	const struct fake *f = &F;

	(void)ctx;
	F.reads_before_reset += F.resets == 0 ? 1u : 0u;
	*bytes = f->z->free;
	return f->z->free_known != 0;
}

static bool f_reset(void *ctx)
{
	struct fake *f = ctx;

	f->resets++;
	f->nfiles = 0;
	return true;
}

static bool f_mkdir(void *ctx, const char *path)
{
	(void)path;
	((struct fake *)ctx)->mkdirs++;
	return true;
}

static bool f_open(void *ctx, const char *path)
{
	struct fake *f = ctx;

	if (f->resets == 0)
		f->writes_before_reset++;
	/* created empty: a file of the same name is replaced */
	for (f->cur = 0; f->cur < f->nfiles; f->cur++) {
		if (strcmp(f->files[f->cur].name, path) == 0)
			break;
	}
	if (f->cur == N(f->files))
		return false;
	snprintf(f->files[f->cur].name, S11_PATH_MAX, "%s", path);
	f->files[f->cur].size = 0;
	if (f->cur == f->nfiles)
		f->nfiles++;
	f->open = 1;
	return true;
}

static bool f_write(void *ctx, const uint8_t *p, size_t n)
{
	struct fake *f = ctx;

	(void)p;
	if (!f->open || f->write_fails)
		return false;
	f->files[f->cur].size += n;
	return true;
}

static bool f_close(void *ctx)
{
	((struct fake *)ctx)->open = 0;
	return true;
}

static const struct s11_zip_ops ops = {
	f_member, f_zip64_end, f_extract, f_crc32, f_free, f_reset, f_mkdir,
	f_open, f_write, f_close, &F,
};

/* The staged files are the expected ones, one name per line, in any order
 * (the members' order is not significant, S-11.030). */
static int staged_is(const char *want)
{
	size_t lines = 0;

	for (const char *p = want; *p != '\0';) {
		size_t n = strcspn(p, "\n");
		int found = 0;

		for (size_t i = 0; i < F.nfiles; i++)
			found |= strlen(F.files[i].name) == n &&
				 strncmp(F.files[i].name, p, n) == 0;
		if (!found)
			return 0;
		lines++;
		p += n + (p[n] == '\n');
	}
	return lines == F.nfiles;
}

static void test_zips(void)
{
	for (size_t i = 0; i < N(s11v_zips); i++) {
		const struct s11v_zip *z = &s11v_zips[i];
		struct s11_staged_sizes st;
		enum s11_reason r;
		unsigned mask = 0;

		memset(&F, 0, sizeof(F));
		F.z = z;
		r = s11_check_zip(&ops, &st);
		CHECK(r == z->reason, "%s: %s, want %s", z->name, s11_reason_name(r),
		      s11_reason_name(z->reason));
		CHECK(F.writes_before_reset == 0, "%s: nothing staged before the reset",
		      z->name);
		/* S-11.031: emptied when check 16 begins, whatever follows */
		CHECK(F.resets == 1 && F.reads_before_reset == 0,
		      "%s: staging area emptied first", z->name);
		if (r == S11_REASON_MEMBER || r == S11_REASON_SPACE ||
		    r == S11_REASON_SPACEQUERY)
			continue;
		for (unsigned c = 0; c < S11_COMPONENTS; c++)
			mask |= (st.size[c] != 0 ? 1u : 0u) << c;
		if (r == S11_REASON_NONE)
			CHECK(mask == z->components, "%s: components %02x, want %02x",
			      z->name, mask, z->components);
		/* S-11.030: each staged component's stated CRC-32 is kept, for
		 * the steps' read-back check */
		for (unsigned c = 0; c < S11_COMPONENTS; c++) {
			for (size_t k = 0; st.size[c] != 0 && k < z->count; k++) {
				const struct s11v_member *v = &s11v_members[z->first + k];

				if (strcmp(v->name, s11_components[c].image) == 0)
					CHECK(st.crc32[c] == v->crc,
					      "%s: component %u crc32 %08x, want %08x",
					      z->name, c, st.crc32[c], v->crc);
			}
		}
		if (z->staged != NULL)
			CHECK(staged_is(z->staged), "%s: staged files", z->name);
	}
}

static const struct s11v_zip *zip_named(const char *name)
{
	for (size_t i = 0; i < N(s11v_zips); i++) {
		if (strcmp(s11v_zips[i].name, name) == 0)
			return &s11v_zips[i];
	}
	return NULL;
}

/* An earlier install's progress record and a stale file, then `name`. */
static enum s11_reason over_earlier(const char *name, int write_fails)
{
	struct s11_staged_sizes st;

	memset(&F, 0, sizeof(F));
	F.z = zip_named(name);
	F.nfiles = 2;
	snprintf(F.files[0].name, S11_PATH_MAX, "progress.log");
	F.files[0].size = 2;
	snprintf(F.files[1].name, S11_PATH_MAX, "old.bin");
	F.files[1].size = 1;
	F.write_fails = write_fails;
	if (F.z == NULL) {
		CHECK(0, "zip %s not found", name);
		return S11_REASON_NONE;
	}
	return s11_check_zip(&ops, &st);
}

static int earlier_gone(void)
{
	for (size_t i = 0; i < F.nfiles; i++) {
		if (strcmp(F.files[i].name, "progress.log") == 0 ||
		    strcmp(F.files[i].name, "old.bin") == 0)
			return 0;
	}
	return 1;
}

/* S-11.031 (SC-11-STAGE), S-11.033 (SC-11-SPACE), S-11.020: a refusal of
 * check 16 leaves no earlier progress record behind. */
static void test_earlier_record(void)
{
	CHECK(over_earlier("name_dotdot", 0) == S11_REASON_MEMBER && earlier_gone(),
	      "name_dotdot: refused MEMBER, earlier files gone");
	CHECK(over_earlier("stored", 1) == S11_REASON_MEMBER && earlier_gone(),
	      "a member write the volume fails: refused MEMBER, earlier files gone");
	CHECK(over_earlier("space_one_less", 0) == S11_REASON_SPACE &&
	      F.nfiles == 0, "SPACE: the staging area exists and is empty");
	CHECK(over_earlier("space_query_fails", 0) == S11_REASON_SPACEQUERY &&
	      F.nfiles == 0, "SPACEQUERY: the staging area exists and is empty");
}

/* S-11.033: totals at and over 2^31 compared in full */
static void test_space(void)
{
	for (size_t i = 0; i < N(s11v_space); i++) {
		const struct s11v_space *s = &s11v_space[i];

		CHECK(s11_space_rule(s->total, s->known != 0, s->free) == s->reason,
		      "space %zu", i);
	}
}

/* S-11.031 */
static void test_names(void)
{
	CHECK(s11_member_name_ok("firmware/a..b/x") && s11_member_name_ok("a/") &&
	      s11_member_name_ok(".a"), "ordinary names");
	CHECK(!s11_member_name_ok("") && !s11_member_name_ok("/a") &&
	      !s11_member_name_ok("a\\b") && !s11_member_name_ok("..") &&
	      !s11_member_name_ok("a/../b") && !s11_member_name_ok("a/.."),
	      "refused names");
	/* canonical only: no "." component, none empty but a directory's
	 * trailing one */
	CHECK(s11_member_name_ok("firmware/") && s11_member_name_ok("a/b/") &&
	      s11_member_name_ok("..a/...") && s11_member_name_ok("a/.b"),
	      "canonical names");
	CHECK(!s11_member_name_ok(".") && !s11_member_name_ok("./firmware/x") &&
	      !s11_member_name_ok("a/./b") && !s11_member_name_ok("a/.") &&
	      !s11_member_name_ok("a/./") && !s11_member_name_ok("a//b") &&
	      !s11_member_name_ok("a//") && !s11_member_name_ok("/") &&
	      !s11_member_name_ok("../a"), "names not canonical");
}

/* `name` checked with the engine as E says; the result's reason. */
static enum s11_reason with_engine(const char *name, struct s11_staged_sizes *st)
{
	enum s11_reason r;

	memset(&F, 0, sizeof(F));
	F.z = zip_named(name);
	E.asked = E.extracts = 0;
	s11_crc_engine = &engine;
	r = F.z != NULL ? s11_check_zip(&ops, st) : S11_REASON_NONE;
	s11_crc_engine = NULL;
	CHECK(F.z != NULL, "zip %s not found", name);
	return r;
}

/* S-11.030: a CRC-32 an engine got wrong is computed again in software
 * before it refuses a member; every verdict is the software's. */
static void test_engine(void)
{
	static const char *const zips[] = { "all_components", "bad_member_crc",
		"stream_bootloader_not_ldr", "with_dir_entries" };
	struct s11_staged_sizes st;
	struct staged_file soft[32];
	size_t nsoft;
	enum s11_reason r;

	for (size_t i = 0; i < N(zips); i++) {
		const struct s11v_zip *z = zip_named(zips[i]);

		if (z == NULL) {
			CHECK(0, "zip %s not found", zips[i]);
			continue;
		}
		/* the software alone: no engine */
		E = (struct engine_model){ false, false, false, 0, 0 };
		memset(&F, 0, sizeof(F));
		F.z = z;
		CHECK(s11_check_zip(&ops, &st) == z->reason, "%s", z->name);
		nsoft = F.nfiles;
		memcpy(soft, F.files, sizeof(soft));

		/* wrong, then replaced: the software's verdict and files, the
		 * engine asked once, the member that failed extracted twice
		 * (its file replaced, check 18's stream restarted) */
		E = (struct engine_model){ true, true, false, 0, 0 };
		r = with_engine(zips[i], &st);
		CHECK(r == z->reason, "%s, engine wrong once: %s, want %s", z->name,
		      s11_reason_name(r), s11_reason_name(z->reason));
		CHECK(F.nfiles == nsoft && !memcmp(F.files, soft, sizeof(soft[0]) * nsoft),
		      "%s: the staged files as the software's", z->name);
		CHECK(E.asked <= 1u && !E.wrong, "%s: asked %u", z->name, E.asked);

		/* wrong and not replaceable: refused MEMBER, asked once */
		E = (struct engine_model){ true, false, false, 0, 0 };
		r = with_engine(zips[i], &st);
		CHECK(E.asked == 1u, "%s: asked %u", z->name, E.asked);
		CHECK(r == S11_REASON_MEMBER, "%s, engine wrong: %s", z->name,
		      s11_reason_name(r));

		/* an engine that says yes every time: still one retry a member */
		E = (struct engine_model){ false, true, true, 0, 0 };
		r = with_engine(zips[i], &st);
		CHECK(r == z->reason, "%s, engine right: %s", z->name,
		      s11_reason_name(r));
	}
	/* a member truly bad: asked once, extracted twice, refused MEMBER */
	E = (struct engine_model){ false, true, true, 0, 0 };
	r = with_engine("bad_member_crc", &st);
	CHECK(r == S11_REASON_MEMBER && E.asked == 1u && E.extracts == 2u,
	      "bad_member_crc: %s, asked %u, extracted %u", s11_reason_name(r),
	      E.asked, E.extracts);
	E = (struct engine_model){ false, false, false, 0, 0 };
	r = with_engine("bad_member_crc", &st);
	CHECK(r == S11_REASON_MEMBER && E.asked == 1u && E.extracts == 1u,
	      "bad_member_crc, nothing to replace: asked %u, extracted %u",
	      E.asked, E.extracts);
}

int main(void)
{
	test_zips();
	test_engine();
	test_earlier_record();
	test_space();
	test_names();
	return s11_check_report("test_s11_zip");
}

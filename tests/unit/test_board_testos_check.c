/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The check suite's pure parts (src/testos/check_logic.c, check_emmc.c,
 * waffle/board/testos_check.h, wiki/Test-OS.md "The check suite"): the switch-back
 * against the RAM eMMC for every starting state and failure, its guard, the
 * read of both boot partitions, the temporary file against a fake file
 * layer, the verdicts, the Nordic gate, the summary's pages and the report's
 * redaction. The sources are built here in the `check` variant.
 */
#define OPXY_TESTOS_CHECK 1
#include "../../src/testos/check_logic.c"
#include "../../src/testos/check_emmc.c"

#include <stdio.h>
#include <string.h>

#include "s03_fake_emmc.h"
#include "waffle/s08_nordic.h"
#include "support/check.h"

#define B 0x40000u		/* 256 KiB boot partitions */
#define IMG 20000u

static struct fake_emmc fe;
static struct s03_emmc_ops fe_ops;
static uint8_t image_os[IMG], image_suite[IMG];
static uint8_t snap1[B], snap2[B];

/* --- failure injection over the fake: an op that fails all its attempts --- */

struct inj {
	char kind;		/* s r w x, 0 none */
	unsigned part;		/* partition selected (r, w) or selected to (s) */
	uint32_t sector;	/* r, w */
	unsigned skip;		/* matching calls let through first */
	unsigned fails;		/* matching calls that fail after those, 0 every one */
	bool corrupt;		/* w: stored with byte 0x05 flipped, reported done */
	unsigned hits;
	unsigned writes;	/* writes that reached the fake */
	unsigned erases;
};
static struct inj inj;

static bool inj_match(char kind, unsigned part, uint32_t sector)
{
	if (inj.kind != kind || inj.part != part || (kind != 's' && kind != 'x' && inj.sector != sector))
		return false;
	inj.hits++;
	if (inj.hits <= inj.skip)
		return false;
	return inj.fails == 0u || inj.hits - inj.skip <= inj.fails;
}

static int i_select(void *ctx, unsigned part)
{
	if (inj_match('s', part, 0))
		return -1;
	return fe_select(ctx, part);
}

static int i_read(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf)
{
	if (inj_match('r', fe.sel, sector))
		return -1;
	return fe_read(ctx, sector, count, buf);
}

static int i_write(void *ctx, uint32_t sector, uint32_t count, const uint8_t *buf)
{
	if (inj.corrupt ? false : inj_match('w', fe.sel, sector))
		return -1;
	inj.writes++;
	if (inj.corrupt && inj_match('w', fe.sel, sector)) {
		uint8_t c[S03_SECTOR];

		memcpy(c, buf, S03_SECTOR);
		c[0x05] ^= 0x01u;
		return fe_write(ctx, sector, count, c);
	}
	return fe_write(ctx, sector, count, buf);
}

static int i_erase(void *ctx, uint32_t first)
{
	inj.erases++;
	return fe_erase(ctx, first);
}

static int i_switch(void *ctx, uint8_t index, uint8_t value)
{
	if (inj_match('x', 0, 0))
		return -1;
	return fe_switch(ctx, index, value);
}

static const struct s03_emmc_ops inj_ops = { &fe, i_select, i_read, i_write, i_erase, i_switch };

static struct tchk_guard guard;
static uint8_t scratch[16u * S03_SECTOR];

static struct s03_env env_guarded(void)
{
	struct s03_env env;

	(void)fe_env(&fe, &fe_ops);	/* the fake's ops and ctx */
	tchk_guard_init(&guard, &inj_ops);
	env.ops = &guard.ops;
	env.crc32 = ref_crc32;
	env.boot_size = B;
	env.scratch = scratch;
	env.scratch_len = sizeof scratch;
	env.max_xfer = 8u;
	env.crc32_fast = NULL;
	return env;
}

/* --- the starting states --- */

enum other_kind { O_ZERO, O_FF, O_OS, O_OS_ACTIVE, O_BAD_CRC, O_ENCRYPTED, O_GARBAGE };

static void fill_images(void)
{
	uint32_t i;

	for (i = 0; i < IMG; i++) {
		image_os[i] = (uint8_t)(i * 7u + 3u);
		image_suite[i] = (uint8_t)(i * 13u + 1u);
	}
}

/* The suite in `mine`, active; the other partition as `kind`. */
static void start(unsigned mine, enum other_kind kind)
{
	const unsigned other = mine == S03_PART_BP1 ? S03_PART_BP2 : S03_PART_BP1;
	uint8_t *o;

	fe_init(&fe, B);
	memset(&inj, 0, sizeof inj);
	fill_images();
	fe_install(&fe, mine, image_suite, IMG, 3u, 1u, 0u, 0u, 0u);
	o = fe_part(&fe, other);
	switch (kind) {
	case O_ZERO:
		break;
	case O_FF:
		memset(o, 0xFF, B);
		break;
	case O_OS:
	case O_OS_ACTIVE:
		fe_install(&fe, other, image_os, IMG, 3u, kind == O_OS_ACTIVE ? 1u : 0u, 0u, 0u, 0u);
		break;
	case O_BAD_CRC:
		fe_install(&fe, other, image_os, IMG, 3u, 0u, 0u, 0u, 0u);
		o[S03_SECTOR + 100u] ^= 0x40u;
		break;
	case O_ENCRYPTED:
		fe_install(&fe, other, image_os, IMG, 3u, 0u, 1u, 0u, 0u);
		o[S03_SECTOR + 100u] ^= 0x40u;	/* ciphertext: no CRC holds */
		break;
	case O_GARBAGE:
	default:
		memset(o, 0x5A, S03_SECTOR);
		break;
	}
	memcpy(snap1, g_fe_bp1, B);
	memcpy(snap2, g_fe_bp2, B);
}

/* Nothing changed but byte 0x0C of the two headers. */
static bool only_active_bytes_changed(void)
{
	uint32_t i;

	for (i = 0; i < B; i++) {
		if (i == S03_ACTIVE_OFF)
			continue;
		if (g_fe_bp1[i] != snap1[i] || g_fe_bp2[i] != snap2[i]) {
			printf("    changed at 0x%x\n", i);
			return false;
		}
	}
	return !fe.user_access && !fe.past_end && inj.erases == 0u;
}

static unsigned takes_now(void)
{
	return tchk_loader_takes(g_fe_bp1, g_fe_bp2);
}

static bool unchanged(void)
{
	return memcmp(g_fe_bp1, snap1, B) == 0 && memcmp(g_fe_bp2, snap2, B) == 0;
}

static void run(struct tchk_switch *sw)
{
	const struct s03_env env = env_guarded();

	tchk_switch_back(&env, sw);
	/* the user area last, always (S-03.007) */
	CHECK(fe.sel == S03_PART_USER || sw->select0_failed);
}

/* --- the switch-back --- */

static void test_other_erased(void)
{
	struct tchk_switch sw;

	start(S03_PART_BP1, O_ZERO);
	run(&sw);
	CHECK(sw.result == TCHK_SW_NO_OTHER_OS);
	CHECK(sw.look == TCHK_LOOK_ZERO);
	CHECK(sw.mine == S03_PART_BP1 && sw.other == S03_PART_BP2);
	CHECK(unchanged() && inj.writes == 0u);
	CHECK(takes_now() == S03_PART_BP1);

	start(S03_PART_BP1, O_FF);
	run(&sw);
	CHECK(sw.result == TCHK_SW_NO_OTHER_OS);
	CHECK(sw.look == TCHK_LOOK_FF);
	CHECK(unchanged() && inj.writes == 0u);
	CHECK(takes_now() == S03_PART_BP1);

	start(S03_PART_BP1, O_GARBAGE);
	run(&sw);
	CHECK(sw.result == TCHK_SW_NO_OTHER_OS && sw.look == TCHK_LOOK_OTHER);
	CHECK(unchanged());

	/* BP1 erased to FF reads active, and no suite could have come from
	 * it (S-03.045): nothing is written */
	start(S03_PART_BP2, O_FF);
	run(&sw);
	CHECK(sw.result == TCHK_SW_NOT_ACTIVE);
	CHECK(unchanged() && inj.writes == 0u);
	start(S03_PART_BP2, O_GARBAGE);
	run(&sw);
	CHECK(sw.result == TCHK_SW_NOT_ACTIVE && unchanged());
}

static void test_other_intact(void)
{
	struct tchk_switch sw;

	/* the suite in BP2 behind an inactive OS in BP1 */
	start(S03_PART_BP2, O_OS);
	run(&sw);
	CHECK(sw.result == TCHK_SW_RESTORED);
	CHECK(sw.look == TCHK_LOOK_INACTIVE);
	CHECK(sw.wrote_a && sw.wrote_b && !sw.unverified);
	CHECK(sw.other_crc_checked && sw.other_crc_ok);
	CHECK(takes_now() == S03_PART_BP1 && sw.takes == S03_PART_BP1);
	CHECK(g_fe_bp1[S03_ACTIVE_OFF] == 1u && g_fe_bp2[S03_ACTIVE_OFF] == 0u);
	CHECK(only_active_bytes_changed());

	/* the suite in BP1, the OS in BP2 */
	start(S03_PART_BP1, O_OS);
	run(&sw);
	CHECK(sw.result == TCHK_SW_RESTORED);
	CHECK(takes_now() == S03_PART_BP2);
	CHECK(only_active_bytes_changed());
}

static void test_other_bad_or_encrypted(void)
{
	struct tchk_switch sw;

	start(S03_PART_BP2, O_BAD_CRC);
	run(&sw);
	CHECK(sw.result == TCHK_SW_OTHER_BAD);
	CHECK(sw.other_crc_checked && !sw.other_crc_ok);
	CHECK(unchanged() && inj.writes == 0u);
	CHECK(takes_now() == S03_PART_BP2);

	start(S03_PART_BP2, O_ENCRYPTED);
	run(&sw);
	CHECK(sw.result == TCHK_SW_RESTORED && sw.unverified && !sw.other_crc_checked);
	CHECK(takes_now() == S03_PART_BP1);
	CHECK(only_active_bytes_changed());
}

static void test_both_and_neither(void)
{
	struct tchk_switch sw;

	/* both active: the suite is BP1 (S-03.040); a second run after (a)
	 * alone is the same state: (b) only */
	start(S03_PART_BP1, O_OS_ACTIVE);
	run(&sw);
	CHECK(sw.result == TCHK_SW_RESTORED);
	CHECK(!sw.wrote_a && sw.wrote_b);
	CHECK(inj.writes == 1u);
	CHECK(takes_now() == S03_PART_BP2);
	CHECK(only_active_bytes_changed());

	/* neither active: no write */
	start(S03_PART_BP1, O_ZERO);
	g_fe_bp1[S03_ACTIVE_OFF] = 0u;
	memcpy(snap1, g_fe_bp1, B);
	run(&sw);
	CHECK(sw.result == TCHK_SW_NOT_ACTIVE);
	CHECK(unchanged() && inj.writes == 0u);
	CHECK(takes_now() == 0u);
}

static void test_second_run_after_a(void)
{
	struct tchk_switch sw;

	/* the suite in BP1, the OS in BP2: a first run cut after (a) */
	start(S03_PART_BP1, O_OS);
	inj.kind = 'w';
	inj.part = S03_PART_BP1;
	inj.sector = 0;
	run(&sw);
	CHECK(sw.result == TCHK_SW_WRITE_FAILED && sw.wrote_a && sw.b_failed);
	CHECK(takes_now() == S03_PART_BP1);		/* the suite still starts */
	CHECK(only_active_bytes_changed());
	/* the second run: both active, BP2 checked again, then (b) */
	memset(&inj, 0, sizeof inj);
	run(&sw);
	CHECK(sw.result == TCHK_SW_RESTORED && !sw.wrote_a && sw.wrote_b);
	CHECK(takes_now() == S03_PART_BP2);
	CHECK(only_active_bytes_changed());
}

/* Each select and read failing: no write, IO. */
static void test_io_failures(void)
{
	static const struct {
		char kind;
		unsigned part;
		uint32_t sector;
	} cases[] = {
		{ 's', 1, 0 }, { 's', 2, 0 }, { 'r', 1, 0 }, { 'r', 2, 0 },
		{ 'r', 1, 1 },	/* the OS's image read (OS in BP1) */
	};
	unsigned i;

	for (i = 0; i < ARRAY_LEN(cases); i++) {
		struct tchk_switch sw;

		start(S03_PART_BP2, O_OS);
		inj.kind = cases[i].kind;
		inj.part = cases[i].part;
		inj.sector = cases[i].sector;
		run(&sw);
		CHECK(sw.result == TCHK_SW_IO);
		CHECK(unchanged() && inj.writes == 0u);
		CHECK(takes_now() == S03_PART_BP2);
	}
	/* the last select of the user area */
	{
		struct tchk_switch sw;

		start(S03_PART_BP2, O_OS);
		inj.kind = 's';
		inj.part = 0;
		run(&sw);
		CHECK(sw.result == TCHK_SW_RESTORED && sw.select0_failed);
		CHECK(only_active_bytes_changed());
	}
	/* no B */
	{
		struct tchk_switch sw;
		struct s03_env env;

		start(S03_PART_BP2, O_OS);
		env = env_guarded();
		env.boot_size = 0;
		tchk_switch_back(&env, &sw);
		CHECK(sw.result == TCHK_SW_IO && unchanged());
	}
}

/* Each write, flush and read-back failing. */
static void test_write_failures(void)
{
	struct tchk_switch sw;

	/* (a)'s write fails on every attempt: undo fails too, nothing stored */
	start(S03_PART_BP2, O_OS);
	inj.kind = 'w';
	inj.part = S03_PART_BP1;
	run(&sw);
	CHECK(sw.result == TCHK_SW_WRITE_FAILED && !sw.wrote_a && sw.undo_failed);
	CHECK(takes_now() == S03_PART_BP2 && unchanged());

	/* (a)'s write fails three times, the undo goes through */
	start(S03_PART_BP2, O_OS);
	inj.kind = 'w';
	inj.part = S03_PART_BP1;
	inj.fails = S03_RETRIES;
	run(&sw);
	CHECK(sw.result == TCHK_SW_WRITE_FAILED && !sw.undo_failed);
	CHECK(takes_now() == S03_PART_BP2 && unchanged());

	/* (a)'s flush fails, and so the undo's: the sector written back as read */
	start(S03_PART_BP2, O_OS);
	inj.kind = 'x';
	run(&sw);
	CHECK(sw.result == TCHK_SW_WRITE_FAILED && sw.undo_failed);
	CHECK(takes_now() == S03_PART_BP2 && unchanged());

	/* (a)'s read-back fails */
	start(S03_PART_BP2, O_OS);
	inj.kind = 'r';
	inj.part = S03_PART_BP1;
	inj.sector = 0;
	inj.skip = 1;			/* the first header read goes through */
	inj.fails = S03_RETRIES;
	run(&sw);
	CHECK(sw.result == TCHK_SW_WRITE_FAILED);
	CHECK(takes_now() == S03_PART_BP2 && unchanged());

	/* (a)'s read-back differs: stored wrongly once, then undone */
	start(S03_PART_BP2, O_OS);
	inj.kind = 'w';
	inj.part = S03_PART_BP1;
	inj.corrupt = true;
	inj.fails = 1;
	run(&sw);
	CHECK(sw.result == TCHK_SW_WRITE_FAILED && !sw.undo_failed);
	CHECK(takes_now() == S03_PART_BP2 && unchanged());

	/* (b) fails with the OS in BP1: it already starts: restored, noted */
	start(S03_PART_BP2, O_OS);
	inj.kind = 'w';
	inj.part = S03_PART_BP2;
	run(&sw);
	CHECK(sw.result == TCHK_SW_RESTORED && sw.wrote_a && sw.b_failed);
	CHECK(takes_now() == S03_PART_BP1);
	CHECK(only_active_bytes_changed());

	/* (b) fails with the OS in BP2: the suite still starts */
	start(S03_PART_BP1, O_OS);
	inj.kind = 'w';
	inj.part = S03_PART_BP1;
	run(&sw);
	CHECK(sw.result == TCHK_SW_WRITE_FAILED && sw.wrote_a);
	CHECK(takes_now() == S03_PART_BP1);
	CHECK(only_active_bytes_changed());

	/* (b)'s read-back differs, the OS in BP2: its byte reached the eMMC,
	 * the rest is as before only if the corruption was undone, so the
	 * loader's choice decides */
	start(S03_PART_BP1, O_OS);
	inj.kind = 'w';
	inj.part = S03_PART_BP1;
	inj.corrupt = true;
	run(&sw);
	CHECK(sw.b_failed);
	CHECK(takes_now() == S03_PART_BP2 ? sw.result == TCHK_SW_RESTORED :
					    sw.result == TCHK_SW_WRITE_FAILED);
}

static void test_guard(void)
{
	struct s03_env env;
	uint8_t s[S03_SECTOR] = { 0 };

	start(S03_PART_BP2, O_OS);
	env = env_guarded();
	/* nothing selected yet: no write */
	CHECK(guard.ops.write(guard.ops.ctx, 0, 1, s) != 0);
	CHECK(guard.ops.select(guard.ops.ctx, S03_PART_USER) == 0);
	CHECK(guard.ops.write(guard.ops.ctx, 0, 1, s) != 0);
	CHECK(guard.ops.select(guard.ops.ctx, S03_PART_BP1) == 0);
	CHECK(guard.ops.write(guard.ops.ctx, 1, 1, s) != 0);
	CHECK(guard.ops.write(guard.ops.ctx, 0, 2, s) != 0);
	CHECK(guard.ops.erase(guard.ops.ctx, 0) != 0);
	CHECK(guard.ops.switch_byte(guard.ops.ctx, 179, 1) != 0);
	CHECK(guard.ops.switch_byte(guard.ops.ctx, 33, 0) != 0);
	CHECK(guard.ops.select(guard.ops.ctx, 3) != 0);
	CHECK(guard.refused == 8u);
	CHECK(guard.ops.switch_byte(guard.ops.ctx, 32, 1) == 0);
	/* a failed select leaves the partition unknown: no write */
	inj.kind = 's';
	inj.part = S03_PART_BP2;
	CHECK(guard.ops.select(guard.ops.ctx, S03_PART_BP2) != 0);
	CHECK(guard.ops.write(guard.ops.ctx, 0, 1, s) != 0);
	CHECK(unchanged() && inj.writes == 0u && inj.erases == 0u);
	(void)env;
}

/* --- emmc-boot --- */

static uint64_t fake_us;
static uint64_t now_us(void)
{
	return fake_us += 1000u;
}

static void test_emmc_boot(void)
{
	struct tchk_switch sw;
	struct tchk_boot b;
	struct s03_env env;

	start(S03_PART_BP2, O_ZERO);
	run(&sw);
	env = env_guarded();
	tchk_emmc_boot(&env, &sw, now_us, &b);
	CHECK(b.run && b.sectors[0] == B / S03_SECTOR && b.sectors[1] == B / S03_SECTOR);
	CHECK(b.errors[0] == 0u && b.errors[1] == 0u);
	CHECK(b.mine_checked && b.mine_ok);
	CHECK(b.bytes == 2u * B && b.us != 0u);
	CHECK(fe.sel == S03_PART_USER && unchanged());

	/* the suite's image damaged, and a read that fails */
	start(S03_PART_BP2, O_ZERO);
	g_fe_bp2[S03_SECTOR + 5u] ^= 1u;
	memcpy(snap2, g_fe_bp2, B);
	run(&sw);
	inj.kind = 'r';
	inj.part = S03_PART_BP1;
	inj.sector = 16;
	env = env_guarded();
	tchk_emmc_boot(&env, &sw, NULL, &b);
	CHECK(b.mine_checked && !b.mine_ok);
	CHECK(b.errors[0] == 8u && b.errors[1] == 0u && b.first_bad[0] == 16u);
	CHECK(unchanged());

	/* the last run of the active partition fails, past the image: the
	 * image is still checked, and where the reads failed is kept */
	start(S03_PART_BP2, O_ZERO);
	run(&sw);
	inj.kind = 'r';
	inj.part = S03_PART_BP2;
	inj.sector = B / S03_SECTOR - 8u;
	env = env_guarded();
	tchk_emmc_boot(&env, &sw, NULL, &b);
	CHECK(b.mine_checked && b.mine_ok && b.unchecked == TCHK_UNCHECKED_NONE);
	CHECK(b.errors[1] == 8u && b.first_bad[1] == B / S03_SECTOR - 8u);
	CHECK(b.sectors[1] == B / S03_SECTOR - 8u);

	/* a read inside the image fails: not checked, and not called encrypted */
	start(S03_PART_BP2, O_ZERO);
	run(&sw);
	inj.kind = 'r';
	inj.part = S03_PART_BP2;
	inj.sector = 8u;
	env = env_guarded();
	tchk_emmc_boot(&env, &sw, NULL, &b);
	CHECK(!b.mine_checked && b.unchecked == TCHK_UNCHECKED_READ);
	CHECK(b.errors[1] == 8u && b.first_bad[1] == 8u);

	/* stored encrypted: not checked */
	start(S03_PART_BP2, O_ZERO);
	g_fe_bp2[S03_ENCRYPTED_OFF] = 1u;
	run(&sw);
	env = env_guarded();
	tchk_emmc_boot(&env, &sw, NULL, &b);
	CHECK(!b.mine_checked && b.unchecked == TCHK_UNCHECKED_ENCRYPTED);
}

/* --- emmc-file --- */

#define FF_MAX 0x20000u
static struct {
	bool exists;
	uint8_t data[FF_MAX];
	uint32_t len, pos;
	int fd;
	unsigned fail;		/* the step that fails, enum tchk_file_step */
	bool flip;
	bool unlink_keeps;
	unsigned opens;
} ff;

static int32_t ff_open(void *ctx, const char *path, uint32_t flags)
{
	(void)ctx;
	CHECK(strcmp(path, TCHK_FILE_PATH) == 0);
	ff.opens++;
	if ((flags & S09_O_CREAT) != 0u) {
		if (ff.fail == TCHK_FILE_CREATE)
			return -1;
		ff.exists = true;
		ff.len = 0;
	} else if (ff.fail == TCHK_FILE_OPEN || !ff.exists) {
		return -1;
	}
	ff.pos = 0;
	return ff.fd = 3;
}

static int32_t ff_write(void *ctx, int32_t fd, const uint8_t *p, uint32_t n)
{
	(void)ctx;
	CHECK(fd == 3);
	if (ff.fail == TCHK_FILE_WRITE && ff.pos >= 0x8000u)
		return -1;
	memcpy(ff.data + ff.pos, p, n);
	ff.pos += n;
	ff.len = ff.pos;
	return (int32_t)n;
}

static int32_t ff_read(void *ctx, int32_t fd, uint8_t *p, uint32_t n)
{
	(void)ctx;
	(void)fd;
	if (ff.fail == TCHK_FILE_READ)
		return -1;
	memcpy(p, ff.data + ff.pos, n);
	if (ff.flip && ff.pos == 0x3000u)
		p[7] ^= 1u;
	ff.pos += n;
	return (int32_t)n;
}

static int32_t ff_close(void *ctx, int32_t fd)
{
	(void)ctx;
	(void)fd;
	return ff.fail == TCHK_FILE_CLOSE ? -1 : 0;
}

static int32_t ff_unlink(void *ctx, const char *path)
{
	(void)ctx;
	(void)path;
	if (ff.fail == TCHK_FILE_UNLINK)
		return -1;
	if (!ff.unlink_keeps)
		ff.exists = false;
	return 0;
}

static int32_t ff_exists(void *ctx, const char *path)
{
	(void)ctx;
	(void)path;
	return ff.exists ? 1 : 0;
}

static const struct tchk_file_ops ff_ops = { NULL, ff_open, ff_write, ff_read, ff_close,
					      ff_unlink, ff_exists };

static void test_emmc_file(void)
{
	static uint8_t buf[TCHK_FILE_PIECE];
	struct tchk_file r;
	static const unsigned fails[] = { TCHK_FILE_CREATE, TCHK_FILE_WRITE, TCHK_FILE_CLOSE,
					  TCHK_FILE_OPEN, TCHK_FILE_READ, TCHK_FILE_UNLINK };
	unsigned i;

	memset(&ff, 0, sizeof ff);
	tchk_file_test(&ff_ops, buf, &r);
	CHECK(r.failed == TCHK_FILE_OK && !r.leftover);
	CHECK(r.written == TCHK_FILE_BYTES && r.compared == TCHK_FILE_BYTES);
	CHECK(!ff.exists);

	memset(&ff, 0, sizeof ff);
	ff.exists = true;		/* an earlier run's */
	tchk_file_test(&ff_ops, buf, &r);
	CHECK(r.failed == TCHK_FILE_OK && r.leftover && !ff.exists);

	memset(&ff, 0, sizeof ff);
	ff.flip = true;
	tchk_file_test(&ff_ops, buf, &r);
	CHECK(r.failed == TCHK_FILE_COMPARE && !ff.exists);

	memset(&ff, 0, sizeof ff);
	ff.unlink_keeps = true;
	tchk_file_test(&ff_ops, buf, &r);
	CHECK(r.failed == TCHK_FILE_GONE);

	for (i = 0; i < ARRAY_LEN(fails); i++) {
		memset(&ff, 0, sizeof ff);
		ff.fail = fails[i];
		tchk_file_test(&ff_ops, buf, &r);
		CHECK(r.failed == fails[i]);
		/* removed whatever failed, except when the unlink itself did */
		CHECK(fails[i] == TCHK_FILE_UNLINK || !ff.exists);
	}
}

/* --- verdicts and judges --- */

static struct tchk_facts facts;
static struct tchk_table table;

static void all_good(void)
{
	unsigned i;

	memset(&facts, 0, sizeof facts);
	facts.ran = 0xFFFFFFFFu;
	facts.slave = true;
	facts.sw.result = TCHK_SW_NO_OTHER_OS;
	facts.sw.look = TCHK_LOOK_ZERO;
	facts.sw.mine = 2;
	facts.regs[0].checked = 6;
	facts.regs[1].checked = 20;
	facts.regs[2].checked = 9;
	facts.flash_id_read = true;
	facts.jedec[0] = 0xef;
	facts.jedec[1] = 0x40;
	facts.jedec[2] = 0x14;
	facts.flash_sr_read = true;
	facts.flash_sr = 0x00;
	facts.flash_read = true;
	facts.meta_magic_ok = true;
	facts.sel_verified = 1;
	facts.slot_good[0] = facts.slot_good[1] = true;
	facts.fs.parsed = facts.fs.codes_ok = true;
	facts.flash_bytes = 0x100000u;
	facts.flash_us = 400000u;
	facts.psoc_greeted = facts.psoc_ping = facts.psoc_keys = facts.psoc_battery = true;
	facts.slave_echo = true;
	facts.slave_failed_row = -1;
	for (i = 0; i < OPXY_TESTOS_STEP_COUNT; i++)
		facts.slave_ms[i] = (uint16_t)(100u * i);
	facts.link_bytes = 0x800000u;
	facts.link_us = 6500000u;
	facts.ext_read = true;
	facts.sec_count = S03_USER_SECTORS;
	facts.erase_group = S03_GROUP_SECTORS;
	facts.ext33 = 0;	/* their power-up values (JESD84-B51) */
	facts.ext34 = 0;
	facts.ext163 = 2;
	facts.features = true;
	facts.ext183 = 2;
	facts.ext226 = 32;
	facts.ext265 = 4;
	facts.boot.run = true;
	facts.boot.sectors[0] = facts.boot.sectors[1] = 8192;
	facts.boot.mine_checked = facts.boot.mine_ok = true;
	facts.boot.bytes = 8u << 20;
	facts.boot.us = 7000000u;
	facts.mounted = facts.space_read = true;
	facts.free_bytes = 1500ull << 20;
	facts.file.written = facts.file.compared = TCHK_FILE_BYTES;
	facts.panel_up = facts.panel_id = facts.te_seen = facts.pattern_drawn = true;
	facts.te_us = 16700u;
	facts.rtc_read = facts.rtc_ticked = true;
	facts.rtc_us = 1001000u;
	facts.nordic_greeting = facts.nordic_info = true;
	memcpy(facts.nordic_reply, "mode:app;sw_version:1.0.1", 25);
	facts.nordic_reply_len = 25;
	memcpy(facts.psoc_info, "sw_version:1.0.7", 16);
	facts.psoc_info_len = 16;
	facts.held_ms = 7100;
	facts.keys = TCHK_G_PASSED;
	facts.keys_seen = 3u;
	facts.psoc_start_greeted = true;
	facts.nordic_start_mode = 1u;
}

static void judge(void)
{
	tchk_table_init(&table);
	tchk_judge(&facts, &table);
}

static void test_verdict(void)
{
	unsigned i;

	/* the OS gone is the expected outcome: the hardware still passes */
	all_good();
	judge();
	for (i = 0; i < TCHK_COUNT; i++)
		if (i != TCHK_OS_PARTITION && table.r[i].state != TCHK_PASS)
			printf("    %s: %s\n", tchk_tests[i].id, table.r[i].detail.s);
	CHECK(table.r[TCHK_OS_PARTITION].state == TCHK_PASS);
	CHECK(strcmp(table.r[TCHK_OS_PARTITION].detail.s,
		     "no OS header in the other partition") == 0);
	CHECK(strstr(table.r[TCHK_OS_PARTITION].more.s, "all zero") != NULL);
	CHECK(tchk_verdict(&table) == TCHK_VERDICT_PASS);
	/* the OS found and checked, made active at the end */
	facts.sw.result = TCHK_SW_READY;
	judge();
	CHECK(table.r[TCHK_OS_PARTITION].state == TCHK_PASS);
	CHECK(strcmp(table.r[TCHK_OS_PARTITION].detail.s,
		     "an OS, checked: made active at the end") == 0);
	facts.sw.unverified = true;
	judge();
	CHECK(strcmp(table.r[TCHK_OS_PARTITION].detail.s,
		     "an image stored encrypted: made active at the end") == 0);
	CHECK(strstr(table.r[TCHK_OS_PARTITION].more.s, "stored encrypted") != NULL);
	facts.sw.unverified = false;
	/* what the other partition holds, or a look that could not read it, is
	 * no failure */
	{
		static const uint8_t calm[] = {
			TCHK_SW_NO_OTHER_OS, TCHK_SW_OTHER_BAD, TCHK_SW_NOT_ACTIVE, TCHK_SW_IO,
			TCHK_SW_NO_SLAVE, TCHK_SW_RESTORED,
		};

		for (i = 0; i < ARRAY_LEN(calm); i++) {
			facts.sw.result = calm[i];
			judge();
			CHECK(table.r[TCHK_OS_PARTITION].state != TCHK_FAIL &&
			      table.r[TCHK_OS_PARTITION].state != TCHK_NOT_PASSED);
		}
	}
	/* nor does a failed restore count */
	facts.sw.result = TCHK_SW_WRITE_FAILED;
	judge();
	CHECK(table.r[TCHK_OS_PARTITION].state == TCHK_FAIL);
	CHECK(tchk_verdict(&table) == TCHK_VERDICT_PASS);

	/* the key check ended with 8 leaves no gap; the installer locks on it */
	all_good();
	facts.keys = TCHK_G_ENDED;
	facts.keys_seen = 1u;
	judge();
	CHECK(table.r[TCHK_KEYS].state == TCHK_NOT_TESTED);
	CHECK(strcmp(table.r[TCHK_KEYS].detail.s, "ended with 8; com pressed, M4 not pressed") == 0);
	CHECK(tchk_verdict(&table) == TCHK_VERDICT_PASS);
	/* back after a trial start: not asked again */
	all_good();
	facts.keys = TCHK_G_EARLIER;
	facts.keys_seen = 0u;
	judge();
	CHECK(table.r[TCHK_KEYS].state == TCHK_PASS);
	CHECK(strcmp(table.r[TCHK_KEYS].detail.s, "passed before the trial, not asked again") == 0);
	all_good();
	judge();
	CHECK(strcmp(table.r[TCHK_KEYS].detail.s, "com and M4 pressed") == 0);

	/* the PSoC: a greeting missed at start-up, answered at the test */
	all_good();
	facts.psoc_start_greeted = false;
	facts.psoc_greeted = false;
	facts.psoc_asked = true;
	facts.psoc_greet_failed = 2u;
	judge();
	CHECK(table.r[TCHK_PSOC].state == TCHK_PASS);
	CHECK(strcmp(table.r[TCHK_PSOC].more.s, "version 1.0.7; start-up greeting not answered; "
		     "greetings not answered since: 2; greeted again at the test") == 0);
	facts.psoc_asked = false;
	judge();
	CHECK(table.r[TCHK_PSOC].state == TCHK_FAIL);
	CHECK(strcmp(table.r[TCHK_PSOC].detail.s, "no answer: greeting") == 0);
	CHECK(strstr(table.r[TCHK_PSOC].more.s, "the test's greeting not answered") != NULL);

	/* the flash read one byte late is named */
	all_good();
	facts.meta_magic_ok = false;
	facts.flash_late = true;
	facts.slot_good[0] = facts.slot_good[1] = false;
	judge();
	CHECK(table.r[TCHK_FLASH_LAYOUT].state == TCHK_FAIL);
	CHECK(strncmp(table.r[TCHK_FLASH_LAYOUT].more.s, "reads one byte late; ", 21) == 0);

	/* an automatic test not tested is a gap; no slave makes several */
	all_good();
	facts.slave = false;
	judge();
	CHECK(table.r[TCHK_SLAVE].state == TCHK_FAIL);
	CHECK(tchk_verdict(&table) == TCHK_VERDICT_FAIL);
	CHECK(table.r[TCHK_EMMC_FILE].state == TCHK_NOT_TESTED);
	CHECK(strcmp(table.r[TCHK_EMMC_FILE].detail.s, "no slave") == 0);
	all_good();
	facts.ran &= ~(1u << TCHK_RTC);
	judge();
	CHECK(tchk_verdict(&table) == TCHK_VERDICT_GAPS);

	/* any failure is FAIL, a guided one included */
	all_good();
	facts.regs[1].differ = 1;
	facts.regs[1].first = &(const struct opxy_testos_reg){ 0x2000200Cu, 31, 0, OPXY_TESTOS_RULE_EQ,
							       5, 0, "DMC0_CFG", "SDRSIZE", "S-13a.012" };
	facts.regs[1].first_value = 4;
	judge();
	CHECK(table.r[TCHK_DDR].state == TCHK_FAIL);
	CHECK(strstr(table.r[TCHK_DDR].more.s, "DMC0_CFG SDRSIZE 4, wanted 5 (S-13a.012)") != NULL);
	CHECK(tchk_verdict(&table) == TCHK_VERDICT_FAIL);
	all_good();
	facts.nordic_reply_len = (uint16_t)strlen(strcpy((char *)facts.nordic_reply,
							  "mode:bootloader;bl_version:1000.0.10"));
	judge();
	CHECK(table.r[TCHK_NORDIC].state == TCHK_FAIL);
	CHECK(strcmp(table.r[TCHK_NORDIC].detail.s, "it is in its bootloader (DFU)") == 0);
	CHECK(strcmp(table.r[TCHK_NORDIC].more.s,
		     "in its application at the suite's start; frames held back by the gate: 0") == 0);
	facts.nordic_start_mode = 2u;
	judge();
	CHECK(strncmp(table.r[TCHK_NORDIC].more.s, "in its bootloader at the suite's start; ", 40) == 0);
	/* told to leave and did: a pass that says so */
	facts.nordic_left = 1u;
	facts.nordic_reply_len = (uint16_t)strlen(strcpy((char *)facts.nordic_reply,
							  "mode:app;sw_version:1.0.1"));
	judge();
	CHECK(table.r[TCHK_NORDIC].state == TCHK_PASS);
	CHECK(strcmp(table.r[TCHK_NORDIC].more.s, "in its bootloader at the suite's start; "
		     "told to leave its bootloader: 1; frames held back by the gate: 0") == 0);
	facts.nordic_left = 0u;
	facts.nordic_start_mode = 0u;
	judge();
	CHECK(strncmp(table.r[TCHK_NORDIC].more.s, "no answer at the suite's start; ", 32) == 0);
	all_good();
	facts.jedec[0] = facts.jedec[1] = facts.jedec[2] = 0xFF;
	judge();
	CHECK(table.r[TCHK_FLASH_ID].state == TCHK_FAIL);
	/* the status register's block-protect bits, reported, judged by nothing */
	all_good();
	facts.flash_sr = 0x3C;		/* BP3-BP0 1111: none protected */
	judge();
	CHECK(strcmp(table.r[TCHK_FLASH_ID].detail.s,
		     "JEDEC id ef 40 14; status 3c, no block protected") == 0);
	facts.flash_sr = 0x04;		/* BP0 */
	judge();
	CHECK(table.r[TCHK_FLASH_ID].state == TCHK_PASS);
	CHECK(strcmp(table.r[TCHK_FLASH_ID].detail.s,
		     "JEDEC id ef 40 14; status 04, block protect bits set") == 0);
	facts.flash_sr_read = false;
	judge();
	CHECK(strcmp(table.r[TCHK_FLASH_ID].detail.s, "JEDEC id ef 40 14") == 0);
	/* S-03.009's switches: judged by the slave's record, not the bring-up's
	 * copy of EXT_CSD, which predates them */
	all_good();
	judge();
	CHECK(table.r[TCHK_EMMC_CONFIG].state == TCHK_PASS);
	CHECK(strstr(table.r[TCHK_EMMC_CONFIG].more.s, "[33] 00 [34] 00") != NULL);
	CHECK(strstr(table.r[TCHK_EMMC_CONFIG].more.s, "features set since") != NULL);
	facts.ext163 = 0;
	judge();
	CHECK(table.r[TCHK_EMMC_CONFIG].state == TCHK_PASS);
	facts.features = false;
	judge();
	CHECK(table.r[TCHK_EMMC_CONFIG].state == TCHK_FAIL);
	CHECK(strcmp(table.r[TCHK_EMMC_CONFIG].detail.s, "S-03.009's feature switches not made") == 0);
	all_good();
	facts.ext183 = 0;
	judge();
	CHECK(strcmp(table.r[TCHK_EMMC_CONFIG].detail.s, "BUS_WIDTH [183] differs from S-03") == 0);

	/* S-13a.080's USB PLL: a difference is noted, not failed */
	all_good();
	facts.regs[TCHK_REGS_USB_PLL].checked = 3;
	facts.regs[TCHK_REGS_USB_PLL].differ = 1;
	facts.regs[TCHK_REGS_USB_PLL].first =
		&(const struct opxy_testos_reg){ 0x200D0398u, 6, 1, OPXY_TESTOS_RULE_EQ, 10, 0,
						  "USB0_PLL_OSC", "PLLM", "S-13a.080" };
	facts.regs[TCHK_REGS_USB_PLL].first_value = 0;
	judge();
	CHECK(table.r[TCHK_CORE_STATE].state == TCHK_PASS);
	CHECK(strcmp(table.r[TCHK_CORE_STATE].detail.s,
		     "9 fields as S-13a states; USB PLL differs, noted") == 0);
	CHECK(strcmp(table.r[TCHK_CORE_STATE].more.s,
		     "USB0_PLL_OSC PLLM 0, reset value a (S-13a.080, not judged)") == 0);
	CHECK(tchk_verdict(&table) == TCHK_VERDICT_PASS);
	facts.regs[TCHK_REGS_CORE].differ = 1;
	facts.regs[TCHK_REGS_CORE].first = facts.regs[TCHK_REGS_USB_PLL].first;
	judge();
	CHECK(table.r[TCHK_CORE_STATE].state == TCHK_FAIL && !table.r[TCHK_CORE_STATE].detail.cut);

	/* emmc-boot says why the image was not checked */
	all_good();
	facts.boot.mine_checked = facts.boot.mine_ok = false;
	facts.boot.unchecked = TCHK_UNCHECKED_READ;
	facts.boot.sectors[0] = facts.boot.sectors[1] = 16256;
	facts.boot.errors[0] = facts.boot.errors[1] = 128;
	facts.boot.first_bad[0] = facts.boot.first_bad[1] = 16256;
	judge();
	CHECK(table.r[TCHK_EMMC_BOOT].state == TCHK_FAIL);
	CHECK(strcmp(table.r[TCHK_EMMC_BOOT].detail.s, "256 reads failed; image not checked") == 0);
	CHECK(strstr(table.r[TCHK_EMMC_BOOT].more.s,
		     "BP1 16256 read, 128 failed from 16256; BP2 16256 read, 128 failed from 16256") != NULL);
	CHECK(strstr(table.r[TCHK_EMMC_BOOT].more.s, "a read inside it failed") != NULL);
	facts.boot.unchecked = TCHK_UNCHECKED_ENCRYPTED;
	judge();
	CHECK(strstr(table.r[TCHK_EMMC_BOOT].detail.s, "; image stored encrypted") != NULL);
	all_good();
	facts.link_bad = 1;
	judge();
	CHECK(table.r[TCHK_BRIDGE].state == TCHK_FAIL);
	all_good();
	facts.sel_index = 0xFF;
	facts.slot_good[1] = false;
	judge();
	CHECK(table.r[TCHK_FLASH_LAYOUT].state == TCHK_FAIL);
	all_good();
	facts.slot_good[1] = false;	/* the other slot: reported, not failed */
	judge();
	CHECK(table.r[TCHK_FLASH_LAYOUT].state == TCHK_PASS);
	CHECK(strstr(table.r[TCHK_FLASH_LAYOUT].detail.s, "other not good") != NULL);
	all_good();
	facts.rtc_us = 1500000u;
	judge();
	CHECK(table.r[TCHK_RTC].state == TCHK_FAIL);
}

static void test_regs_groups(void)
{
	static const struct opxy_testos_reg rows[] = {
		{ 0x20005000u, 31, 0, OPXY_TESTOS_RULE_EQ, 0x20002000u, 0, "CGU0_CTL", "ALL", "S-13a.002" },
		{ 0x20005004u, 3, 0, OPXY_TESTOS_RULE_IN, 1, 4, "CGU0_DIV", "CSEL", "S-13a.003" },
		{ 0x20006000u, 0, 0, OPXY_TESTOS_RULE_EQ, 1, 0, "DMC0_STAT", "INITDONE", "S-13a.017" },
		{ 0x20007000u, 7, 0, OPXY_TESTOS_RULE_ANY, 0, 0, "X", "Y", "S-13a.010" },
		{ 0x20008000u, 7, 0, OPXY_TESTOS_RULE_EQ, 0xAD, 0, "WDOG0_CTL", "WDEN", "S-13a.060" },
	};
	const uint32_t vals[] = { 0x20002000u, 7u, 1u, 9u, 0x10u };
	struct tchk_regs g[TCHK_REGS_GROUPS];

	CHECK(tchk_reg_group("S-13a.002") == TCHK_REGS_CLOCKS);
	CHECK(tchk_reg_group("S-13a.006") == TCHK_REGS_CLOCKS);
	CHECK(tchk_reg_group("S-13a.008") == TCHK_REGS_DDR);
	CHECK(tchk_reg_group("S-13a.017") == TCHK_REGS_DDR);
	CHECK(tchk_reg_group("S-13a.021") == TCHK_REGS_CORE);
	CHECK(tchk_reg_group("S-13a.080") == TCHK_REGS_USB_PLL);
	CHECK(tchk_reg_group("S-13a.081") == TCHK_REGS_CORE);
	tchk_regs_judge(rows, ARRAY_LEN(rows), vals, g);
	CHECK(g[TCHK_REGS_CLOCKS].checked == 2u && g[TCHK_REGS_CLOCKS].differ == 1u);
	CHECK(g[TCHK_REGS_CLOCKS].first == &rows[1] && g[TCHK_REGS_CLOCKS].first_value == 7u);
	CHECK(g[TCHK_REGS_DDR].checked == 1u && g[TCHK_REGS_DDR].differ == 0u);
	CHECK(g[TCHK_REGS_CORE].checked == 1u && g[TCHK_REGS_CORE].differ == 1u);
}

/* --- the Nordic gate --- */

static uint8_t crc8(const uint8_t *d, size_t n)
{
	uint8_t c = 0;

	while (n--)
		c ^= *d++;
	return c;
}

static bool allowed(uint8_t id, const uint8_t *p, size_t n)
{
	uint8_t wire[S06_MAX_WIRE];
	const size_t w = s06_frame_encode(0x13, 7, id, p, n, crc8, wire);
	unsigned got;
	const bool ok = tchk_nordic_allowed(wire, w, &got);

	CHECK(got == id);
	return ok;
}

static void test_nordic_gate(void)
{
	static const uint8_t hello[2] = { 0x01, 0x03 }, ok40 = 0x40, enter = 0xEE, exit18 = 0x18;
	static const uint8_t chunk[8] = { 0x03, 0, 0, 0, 0, 0, 0, 0 };
	static const uint8_t junk[5] = { 1, 2, 0, 3, 0 };
	unsigned id;

	CHECK(allowed(0x05, hello, 2));
	CHECK(allowed(0x06, &ok40, 1));
	CHECK(allowed(0x09, NULL, 0));
	CHECK(allowed(0x02, hello, 2));
	CHECK(!allowed(0x0D, &enter, 1));	/* DFU enter */
	CHECK(allowed(0x0D, &exit18, 1));	/* leave the bootloader: state 0's answer */
	CHECK(!allowed(0x0D, chunk, sizeof chunk));
	CHECK(!allowed(0x0D, hello, 2));
	CHECK(!allowed(0x0D, NULL, 0));
	CHECK(!allowed(0x06, &exit18, 1));
	CHECK(!allowed(0x05, &ok40, 1));
	CHECK(!allowed(0x51, &ok40, 1));
	CHECK(!tchk_nordic_allowed(junk, sizeof junk, &id) && id == 0xFFFFu);
	CHECK(!tchk_nordic_allowed(junk, 0, &id));
	/* S-08.011 in state 0: the bootloader's 0xBB is told to leave, every
	 * other greeting 0x06 {40}; both answers pass the gate */
	{
		static const uint8_t flags[] = { 0x00, 0xAA, 0xBB, 0x01 };
		unsigned i;

		for (i = 0; i < ARRAY_LEN(flags); i++) {
			uint8_t aid = 0, byte = 0;

			CHECK(s08_answer_greeting(TCHK_NORDIC_STATE, &flags[i], 1, &aid, &byte));
			if (flags[i] == 0xBBu)
				CHECK(aid == 0x0Du && byte == 0x18u);
			else
				CHECK(aid == 0x06u && byte == 0x40u);
			CHECK(allowed(aid, &byte, 1));
		}
	}
}

/* --- pages and the report --- */

/* Columns a line takes on screen (wiki/Screen-Style-Guide.md). */
static size_t columns(const struct tchk_line *l)
{
	const char *tab = strchr(l->s, '\t');
	size_t n = 0;
	const char *p;

	if (l->style >= S15_ST_STEP_RUN && l->style <= S15_ST_STEP_FAIL) {
		CHECK(tab != NULL && (size_t)(tab - l->s) <= S15_STEP_LABEL_MAX);
		return tab == NULL ? strlen(l->s) : S15_STEP_COL + strlen(tab + 1);
	}
	if (l->style == S15_ST_KV)
		return tab == NULL ? strlen(l->s) : 14u + strlen(tab + 1);
	CHECK(tab == NULL);
	for (p = l->s; *p != '\0'; p++)
		if (!(l->style == S15_ST_KEYS && *p == '`'))
			n++;
	if (l->style == S15_ST_OK || l->style == S15_ST_WARN || l->style == S15_ST_ERROR)
		n += 3u;
	return n;
}

static void check_pages(const struct tchk_switch *sw)
{
	unsigned p, i;

	for (p = 0; p < TCHK_PAGES; p++) {
		struct tchk_line l[S15_ROWS];
		const unsigned n = tchk_page(&table, sw, p, l);

		CHECK(n == S15_ROWS);
		for (i = 0; i < n; i++) {
			if (columns(&l[i]) > S15_COLS)
				printf("    page %u row %u: %s\n", p, i, l[i].s);
			CHECK(columns(&l[i]) <= S15_COLS);
		}
	}
}

static void test_pages_fit(void)
{
	static const uint8_t results[] = {
		TCHK_SW_RESTORED, TCHK_SW_NO_OTHER_OS, TCHK_SW_OTHER_BAD, TCHK_SW_WRITE_FAILED,
		TCHK_SW_IO, TCHK_SW_NOT_ACTIVE, TCHK_SW_NO_SLAVE, TCHK_SW_READY, TCHK_SW_READY,
	};
	unsigned i, k;

	for (i = 0; i < ARRAY_LEN(results); i++) {
		all_good();
		facts.sw.result = results[i];
		facts.sw.unverified = i == 0u || i + 1u == ARRAY_LEN(results);
		judge();
		check_pages(&facts.sw);
	}
	/* the longest details, every one whole */
	all_good();
	facts.slave = false;
	facts.psoc_greeted = facts.psoc_ping = facts.psoc_keys = facts.psoc_battery = false;
	facts.te_misses = 123456;
	facts.te_us = 99999;
	facts.file.failed = TCHK_FILE_LEFTOVER;
	facts.keys = TCHK_G_ENDED;
	facts.keys_seen = 0u;
	facts.psoc_start_greeted = false;
	facts.psoc_greet_failed = 255u;
	judge();
	check_pages(&facts.sw);
	all_good();
	facts.link_bad = 4000000000u;
	facts.link_err = 4000000000u;
	facts.boot.errors[0] = 4000000000u;
	facts.file.failed = TCHK_FILE_GONE;
	facts.features = false;
	facts.boot.unchecked = TCHK_UNCHECKED_ENCRYPTED;
	facts.boot.mine_checked = false;
	judge();
	check_pages(&facts.sw);
	for (k = 0; k < TCHK_COUNT; k++) {
		CHECK(!table.r[k].detail.cut);
		CHECK(strlen(tchk_tests[k].name) <= S15_STEP_LABEL_MAX);
	}
	/* the first screen's lines fit too, and say nothing of the OS */
	for (k = 0; k < TCHK_FIRST_LINES; k++)
		CHECK(strlen(tchk_first_lines[k]) <= S15_COLS &&
		      strstr(tchk_first_lines[k], "OS") == NULL);
}

static char report[TCHK_REPORT_MAX];

static bool has(const char *needle)
{
	return strstr(report, needle) != NULL;
}

/* Values that are unit-unique or not this project's, fed in as facts: none may
 * reach the report or a page. */
static void test_report_redaction(void)
{
	static const char *const secret[] = {
		"WB7Q4K2Z", "NRF99X1", "SN12345678", "1a2b3c4d", "1A2B3C4D", "439041101",
		"5e6f7", "386807", "c0ffee11", "C0FFEE11", "7a5b3", "501171",
		"d00dfeed", "86c0", "34496", "beefcafe",
	};
	unsigned i, p;
	size_t n;

	all_good();
	facts.slot_crc[0] = 0x1A2B3C4Du;
	facts.slot_crc[1] = 0xBEEFCAFEu;
	facts.slot_len[0] = facts.slot_len[1] = 0x5E6F7u;
	facts.fs.len = 0x86C0u;
	facts.fs.blocks = 0x3Du;
	facts.sw.h[0].crc = 0xC0FFEE11u;
	facts.sw.h[0].length = 0x7A5B3u;
	facts.sw.h[1].crc = 0xD00DFEEDu;
	facts.psoc_info_len = (uint16_t)strlen(strcpy((char *)facts.psoc_info,
						       "serial:WB7Q4K2Z;sw_version:1.0.7"));
	facts.nordic_reply_len = (uint16_t)strlen(strcpy((char *)facts.nordic_reply,
							  "serial:NRF99X1;mode:app;sw_version:SN12345678"));
	judge();
	n = tchk_report(&table, &facts.sw, "1.0.11", report, sizeof report);
	CHECK(n > 0u && n < sizeof report - 1u);
	for (i = 0; i < ARRAY_LEN(secret); i++) {
		if (has(secret[i]))
			printf("    report holds %s\n", secret[i]);
		CHECK(!has(secret[i]));
	}
	for (p = 0; p < TCHK_PAGES; p++) {
		struct tchk_line l[S15_ROWS];
		const unsigned k = tchk_page(&table, &facts.sw, p, l);
		unsigned j;

		for (j = 0; j < k; j++)
			for (i = 0; i < ARRAY_LEN(secret); i++)
				CHECK(strstr(l[j].s, secret[i]) == NULL);
	}
	/* what may be there is */
	CHECK(has("waffle-boot installer, checks v1.0.11\n"));
	CHECK(has("version 1.0.7"));
	CHECK(has("version unread"));
	CHECK(has("JEDEC id ef 40 14; status 00, no block protected"));
	CHECK(has("Hardware: PASS"));
	CHECK(!has("Your OS") && !has("active again") && !has("written again"));
	CHECK(!has("cold") && !has("removes"));
	CHECK(has("other partition's header: all zero"));
	CHECK(has("os-partition  pass        no OS header in the other partition"));
	CHECK(has("- waffle-boot's own earliest start-up"));
	/* the report ends with its last line whole */
	CHECK(n > 1u && report[n - 1u] == '\n' && report[n] == '\0');
	CHECK(has("the install is then not offered.\n"));
	CHECK(!has("returns"));
	/* every detail written whole */
	for (i = 0; i < TCHK_COUNT; i++)
		CHECK(!table.r[i].detail.cut && !table.r[i].more.cut);

	/* an OS found: only its row says it is made active at the end */
	facts.sw.result = TCHK_SW_READY;
	judge();
	n = tchk_report(&table, &facts.sw, "1.0.11", report, sizeof report);
	CHECK(n == strlen(report));
	CHECK(has("os-partition  pass        an OS, checked: made active at the end\n"));
	CHECK(!has("Your OS"));
}

/* check.txt keeps one length however its text changes, ending in a newline. */
static void test_report_pad(void)
{
	size_t a, b, n;

	all_good();
	facts.keys = TCHK_G_NOT_RUN;
	judge();
	n = tchk_report(&table, &facts.sw, "1.0.11", report, sizeof report);
	CHECK(n + 1u < TCHK_REPORT_LEN);
	a = tchk_report_pad(report, n, sizeof report);
	CHECK(a == TCHK_REPORT_LEN && report[a - 1u] == '\n' && report[n] == ' ' &&
	      report[a - 2u] == ' ');
	CHECK(report[n - 1u] == '\n');
	facts.keys = TCHK_G_PASSED;
	facts.sw.result = TCHK_SW_RESTORED;
	facts.nordic_refused = 4000000000u;
	judge();
	n = tchk_report(&table, &facts.sw, "1.0.11", report, sizeof report);
	b = tchk_report_pad(report, n, sizeof report);
	CHECK(a == b);
	/* no room: written as it is */
	CHECK(tchk_report_pad(report, TCHK_REPORT_LEN - 1u, sizeof report) == TCHK_REPORT_LEN - 1u);
	CHECK(tchk_report_pad(report, 10u, TCHK_REPORT_LEN - 1u) == 10u);
}

static void test_formatters(void)
{
	struct tchk_text t;

	tchk_text_init(&t, 56);
	tchk_put_rate(&t, 1300000u, 1000000u);
	CHECK(strcmp(t.s, "1.3 MB/s") == 0);
	tchk_text_init(&t, 56);
	tchk_put_ms(&t, 16700u);
	CHECK(strcmp(t.s, "16.7 ms") == 0);
	tchk_text_init(&t, 56);
	tchk_put_ms(&t, 1234567u);
	CHECK(strcmp(t.s, "1234 ms") == 0);
	tchk_text_init(&t, 56);
	tchk_put_hex(&t, 0x5u, 2u);
	tchk_put_str(&t, " ");
	tchk_put_hex(&t, 0x1CF8000u, 1u);
	CHECK(strcmp(t.s, "05 1cf8000") == 0);
	tchk_text_init(&t, 4);
	tchk_put_str(&t, "abcdef");
	CHECK(strcmp(t.s, "abcd") == 0 && t.cut);
}

/* --- the first stage --- */

static uint8_t flash[0x10000];

static int flash_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (off + len > sizeof flash)
		return -1;
	memcpy(buf, flash + off, len);
	return 0;
}

/* A block header at off with its checksum byte set (S-05 Terms). */
static uint32_t block(uint32_t off, uint32_t flags_bcode, uint32_t count)
{
	uint8_t *h = flash + off;
	uint8_t x = 0;
	unsigned i;

	memset(h, 0, 16);
	h[0] = (uint8_t)flags_bcode;
	h[1] = (uint8_t)(flags_bcode >> 8);
	h[3] = 0xAD;
	h[4] = 0x00;
	h[7] = 0x08;
	h[8] = (uint8_t)count;
	h[9] = (uint8_t)(count >> 8);
	for (i = 0; i < 16u; i++)
		x ^= h[i];
	h[2] = x;
	return off + 16u + ((flags_bcode & 0x100u) != 0u ? 0u : count);
}

static void test_first_stage(void)
{
	struct tchk_stage st;
	uint32_t off;

	memset(flash, 0xFF, sizeof flash);
	off = block(0, 0x4004u, 0);			/* FIRST|IGNORE */
	off = block(off, 0x0004u, 0x1000u);
	off = block(off, 0x0104u, 0x2000u);		/* FILL: no payload */
	off = block(off, 0x8004u, 0);
	tchk_first_stage(flash_read, NULL, &st);
	CHECK(st.parsed && st.codes_ok && st.blocks == 4u && st.len == off);

	block(16, 0x0001u, 0x1000u);			/* a BCODE other than 4 */
	tchk_first_stage(flash_read, NULL, &st);
	CHECK(st.parsed && !st.codes_ok);

	flash[16 + 3] = 0xAC;				/* no signature */
	tchk_first_stage(flash_read, NULL, &st);
	CHECK(!st.parsed);

	memset(flash, 0xFF, sizeof flash);
	off = block(0, 0x4004u, 0);
	while (off < 0xB000u - 16u)
		off = block(off, 0x0004u, 0x800u);	/* no FINAL within 0xB000 */
	tchk_first_stage(flash_read, NULL, &st);
	CHECK(!st.parsed);
}

int main(void)
{
	RUN(test_first_stage);
	RUN(test_other_erased);
	RUN(test_other_intact);
	RUN(test_other_bad_or_encrypted);
	RUN(test_both_and_neither);
	RUN(test_second_run_after_a);
	RUN(test_io_failures);
	RUN(test_write_failures);
	RUN(test_guard);
	RUN(test_emmc_boot);
	RUN(test_emmc_file);
	RUN(test_formatters);
	RUN(test_regs_groups);
	RUN(test_verdict);
	RUN(test_nordic_gate);
	RUN(test_pages_fit);
	RUN(test_report_redaction);
	RUN(test_report_pad);
	return check_summary("board_testos_check");
}

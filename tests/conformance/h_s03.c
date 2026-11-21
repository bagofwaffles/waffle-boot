/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Conformance host program for S-03 (src/s03_emmc.c), driven by
 * test_s03.py.  It holds an eMMC with two boot partitions of B bytes that
 * erase to 0 (images.emmc.Emmc), an operation log and fault injection.  Each
 * result is followed by " |" and the eMMC operations, in images.emmc's
 * notation: s:PART, r:PART:FIRST:COUNT, e:PART:GROUP, w:PART:FIRST:COUNT,
 * x:INDEX:VALUE (a CMD6 byte switch), u (the unmount), with ":error" on an
 * operation that failed; the retries of a failed operation show once.
 *
 *   bsize B                       -> ok      (B bytes per boot partition)
 *   load P PATH | save P PATH     -> ok      (boot partition P, B bytes)
 *   zero P                        -> ok
 *   poke P OFF HEX                -> ok
 *   pokefile P OFF PATH           -> ok      (the file's bytes at OFF)
 *   peek P OFF LEN                -> HEX
 *   fault none                    -> ok      (faults stay until cleared)
 *   fault fail OP PART FIRST NTH STORE
 *                                 -> ok      the NTH s/r/e/w/x op with this
 *                                            part and first sector (group for
 *                                            e, EXT_CSD index and part 0 for
 *                                            x) fails with its retries; a
 *                                            write stores in full when STORE
 *   fault corrupt PART SECTOR BYTE NTH
 *                                 -> ok      the NTH write covering SECTOR
 *                                            stores that sector's BYTE inverted
 *   fault cut K                   -> ok      power cut after the K-th erase/write
 *   fault readfail PART SECTOR    -> ok      every read covering it fails
 *   boot                          -> RESULT PART LENGTH SEL0FAIL CRC
 *   write PATH                    -> RESULT TARGET SEL0FAIL   (or cut)
 *   supported SEC GROUP OPT       -> 0|1
 *   region SECTOR                 -> yaffs2|unused|fat|none
 *   fatmap SECTOR                 -> USER_SECTOR
 *   features                      -> STATUS
 *   shutdown UNMOUNT_FAILS [INDEX] -> STATUS ATTEMPTS  (the switch of EXT_CSD[INDEX] fails on
 *                                            every attempt; ATTEMPTS: how many were made)
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "waffle/s03_emmc.h"
#include "hlib.h"

#define MAX_B (8u << 20)

static uint8_t boot[2][MAX_B];
static uint32_t bsize = 4u << 20;
static unsigned selected;
static uint8_t staging[S03_OS_MAX + 2u * S03_SECTOR];
static uint8_t scratch[16u * S03_SECTOR];

/* --- faults --- */

static struct {
	char op;		/* 0: none */
	unsigned part;
	uint32_t first;
	unsigned long nth;
	int store;
	unsigned long seen;
	unsigned active;	/* attempts failed in the current run of retries */
} ffail;

static struct {
	unsigned part;		/* 0: none */
	uint32_t sector, byte;
	unsigned long nth, seen;
} fcorrupt;

static unsigned long fcut, muts;
static struct {
	unsigned part;		/* 0: none */
	uint32_t sector;
} freadfail;

static jmp_buf cut;

/* --- the log --- */

static char oplog[1u << 16];
static size_t oplen;
static char last[48];
static int last_failed;

/* Logs op; 0 when it is a retry of the failed operation just logged. */
static int logop(const char *op, int failed)
{
	int n;

	/* The retries of a failed operation show once. */
	if (failed && last_failed && strcmp(op, last) == 0)
		return 0;
	n = snprintf(oplog + oplen, sizeof oplog - oplen, " %s%s", op, failed ? ":error" : "");
	if (n > 0 && (size_t)n < sizeof oplog - oplen)
		oplen += (size_t)n;
	snprintf(last, sizeof last, "%s", op);
	last_failed = failed;
	return 1;
}

/*
 * Whether this operation is the one fault fail names, or one of its
 * S03_RETRIES attempts (S-03.010); the same operation after them is a new one.
 */
static int fails(char op, unsigned part, uint32_t first)
{
	if (ffail.op != op || ffail.part != part || ffail.first != first) {
		ffail.active = 0;
		return 0;
	}
	if (ffail.active > 0u) {
		if (ffail.active < S03_RETRIES) {
			ffail.active++;
			return 1;
		}
		ffail.active = 0;
		return 0;
	}
	if (++ffail.seen == ffail.nth) {
		ffail.active = 1;
		return 1;
	}
	return 0;
}

/*
 * The cut counts erases and writes as images.emmc does: a failed one once with
 * its retries, and a write that continues the write just before it (the same
 * partition, the next sector) as part of it, since S-03.055 leaves the split
 * into transfers free.  Once K of them are done, the next operation that does
 * not continue the last write is cut before it starts.
 */
static struct {
	char kind;
	unsigned part;
	uint32_t end;
} prev;

static int begin(char kind, uint32_t sector)
{
	int cont = kind == 'w' && prev.kind == 'w' && prev.part == selected && prev.end == sector;

	if (fcut != 0u && muts >= fcut && !cont)
		longjmp(cut, 1);
	return cont;
}

static void done(char kind, uint32_t end, int counted, int cont)
{
	if ((kind == 'w' || kind == 'e') && counted && !cont)
		muts++;
	prev.kind = kind;
	prev.part = selected;
	prev.end = end;
}

/* --- the eMMC --- */

static int c_select(void *ctx, unsigned part)
{
	char b[16];
	int f;

	(void)ctx;
	begin('s', 0);
	f = fails('s', part, 0);
	snprintf(b, sizeof b, "s:%u", part);
	logop(b, f);
	done('s', 0, 0, 0);
	if (f)
		return -1;
	selected = part;
	return 0;
}

static int in_range(uint32_t sector, uint32_t count)
{
	return selected >= 1u && selected <= 2u && count <= bsize / S03_SECTOR &&
	       sector <= bsize / S03_SECTOR - count;
}

static int c_read(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf)
{
	char b[48];
	int f;

	(void)ctx;
	begin('r', sector);
	done('r', 0, 0, 0);
	f = fails('r', selected, sector);
	if (freadfail.part == selected && freadfail.sector >= sector &&
	    freadfail.sector - sector < count)
		f = 1;
	snprintf(b, sizeof b, "r:%u:%lu:%lu", selected, (unsigned long)sector, (unsigned long)count);
	logop(b, f);
	if (f)
		return -1;
	if (selected == S03_PART_USER) {
		memset(buf, 0, (size_t)count * S03_SECTOR);
		return 0;
	}
	if (!in_range(sector, count))
		return -1;
	memcpy(buf, boot[selected - 1u] + (size_t)sector * S03_SECTOR, (size_t)count * S03_SECTOR);
	return 0;
}

static int c_write(void *ctx, uint32_t sector, uint32_t count, const uint8_t *buf)
{
	char b[48];
	int f, counted, cont;
	uint8_t *dst;

	(void)ctx;
	cont = begin('w', sector);
	f = fails('w', selected, sector);
	snprintf(b, sizeof b, "w:%u:%lu:%lu", selected, (unsigned long)sector, (unsigned long)count);
	counted = logop(b, f);
	if (selected == S03_PART_USER || !in_range(sector, count))
		return -1;
	if (f && !ffail.store) {
		done('w', sector + count, counted, cont);
		return -1;
	}
	dst = boot[selected - 1u] + (size_t)sector * S03_SECTOR;
	memcpy(dst, buf, (size_t)count * S03_SECTOR);
	if (fcorrupt.part == selected && fcorrupt.sector >= sector &&
	    fcorrupt.sector - sector < count && ++fcorrupt.seen == fcorrupt.nth)
		dst[(size_t)(fcorrupt.sector - sector) * S03_SECTOR + fcorrupt.byte] ^= 0xFFu;
	done('w', sector + count, counted, cont);
	return f ? -1 : 0;
}

static int c_erase(void *ctx, uint32_t first)
{
	char b[48];
	uint32_t group = first / S03_GROUP_SECTORS;
	int f, counted;
	size_t a, e;

	(void)ctx;
	begin('e', 0);
	f = fails('e', selected, group);
	snprintf(b, sizeof b, "e:%u:%lu", selected, (unsigned long)group);
	counted = logop(b, f);
	if (f) {
		done('e', 0, counted, 0);
		return -1;
	}
	if (selected < 1u || selected > 2u || first % S03_GROUP_SECTORS != 0u)
		return -1;
	a = (size_t)first * S03_SECTOR;
	e = a + (size_t)S03_GROUP_SECTORS * S03_SECTOR;
	if (a < bsize)
		memset(boot[selected - 1u] + a, 0, (e < bsize ? e : bsize) - a);
	done('e', 0, counted, 0);
	return 0;
}

static int switch_fail = -1;	/* an EXT_CSD index whose switch always fails */
static unsigned long switch_attempts;

static int c_switch(void *ctx, uint8_t index, uint8_t value)
{
	char b[24];
	int f = (int)index == switch_fail || fails('x', 0, index);

	(void)ctx;
	snprintf(b, sizeof b, "x:%u:%u", index, value);
	logop(b, f);
	switch_attempts += (unsigned long)f;
	return f ? -1 : 0;
}

static const struct s03_emmc_ops emmc_ops = { NULL, c_select, c_read, c_write, c_erase, c_switch };

static struct s03_env env = { &emmc_ops, h_crc32, 0, scratch, sizeof scratch, 0, NULL };

static int unmount_fails;

static int unmount(void *ctx)
{
	(void)ctx;
	logop("u", unmount_fails);
	return unmount_fails ? -1 : 0;
}

/* --- files --- */

static unsigned part_arg(const struct h_cmd *c, int i)
{
	unsigned long p = h_num(c, i);

	if (p < 1u || p > 2u) {
		h_error(c, "partition");
		return 1;
	}
	return (unsigned)p;
}

static uint8_t *load_file(const char *path, size_t *n)
{
	FILE *f = path ? fopen(path, "rb") : NULL;
	uint8_t *buf;
	long len;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(len > 0 ? (size_t)len : 1u);
	if (buf && len >= 0 && fread(buf, 1, (size_t)len, f) == (size_t)len) {
		fclose(f);
		*n = (size_t)len;
		return buf;
	}
	fclose(f);
	free(buf);
	return NULL;
}

static void cmd_fault(const struct h_cmd *c)
{
	const char *k = h_str(c, 1);

	if (!k) {
		h_error(c, "fault kind");
	} else if (strcmp(k, "none") == 0) {
		memset(&ffail, 0, sizeof ffail);
		memset(&fcorrupt, 0, sizeof fcorrupt);
		memset(&freadfail, 0, sizeof freadfail);
		fcut = 0;
	} else if (strcmp(k, "fail") == 0) {
		const char *op = h_str(c, 2);

		ffail.op = op ? op[0] : 0;
		ffail.part = (unsigned)h_num(c, 3);
		ffail.first = (uint32_t)h_num(c, 4);
		ffail.nth = h_num(c, 5);
		ffail.store = h_num(c, 6) != 0;
		ffail.seen = 0;
		ffail.active = 0;
	} else if (strcmp(k, "corrupt") == 0) {
		fcorrupt.part = part_arg(c, 2);
		fcorrupt.sector = (uint32_t)h_num(c, 3);
		fcorrupt.byte = (uint32_t)h_num(c, 4) % S03_SECTOR;
		fcorrupt.nth = h_num(c, 5);
		fcorrupt.seen = 0;
	} else if (strcmp(k, "cut") == 0) {
		fcut = h_num(c, 2);
	} else if (strcmp(k, "readfail") == 0) {
		freadfail.part = (unsigned)h_num(c, 2);
		freadfail.sector = (uint32_t)h_num(c, 3);
	} else {
		h_error(c, "fault kind");
	}
	h_put("ok");
}

static const char *region_name(enum s03_region r)
{
	switch (r) {
	case S03_REGION_YAFFS2:
		return "yaffs2";
	case S03_REGION_UNUSED:
		return "unused";
	case S03_REGION_FAT:
		return "fat";
	case S03_REGION_NONE:
	default:
		return "none";
	}
}

/* The commands that use the eMMC; run under the cut's setjmp. */
static void emmc_command(const struct h_cmd *c)
{
	const char *k = c->tok[0];

	if (strcmp(k, "boot") == 0) {
		struct s03_load o;

		s03_load_os(&env, staging, sizeof staging, &o);
		h_putu((unsigned long)o.result);
		h_putu(o.part);
		h_putu(o.length);
		h_putu(o.select0_failed);
		h_putx(o.result == S03_LOAD_OK ? h_crc32(0, staging, o.length) : 0u);
	} else if (strcmp(k, "write") == 0) {
		struct s03_write o;
		size_t n = 0;
		uint8_t *img = load_file(h_str(c, 1), &n);

		if (!img) {
			h_error(c, "cannot read image");
			return;
		}
		s03_write_os(&env, img, (uint32_t)n, &o);
		free(img);
		h_putu((unsigned long)o.result);
		h_putu(o.target);
		h_putu(o.select0_failed);
	} else if (strcmp(k, "features") == 0) {
		h_putu(s03_emmc_features_on(&env) != 0);
	} else if (strcmp(k, "shutdown") == 0) {
		unmount_fails = h_num(c, 1) != 0;
		switch_fail = c->n > 2 ? (int)h_num(c, 2) : -1;
		switch_attempts = 0;
		h_putu(s03_emmc_shutdown(&env, unmount, NULL) != 0);
		h_putu(switch_attempts);
		switch_fail = -1;
	} else {
		h_error(c, "unknown command");
	}
}

static void command(const struct h_cmd *c)
{
	const char *k = c->tok[0];

	if (strcmp(k, "bsize") == 0) {
		unsigned long b = h_num(c, 1);

		if (b == 0u || b > MAX_B || b % S03_SECTOR != 0u)
			h_error(c, "size");
		else
			bsize = (uint32_t)b;
		env.boot_size = bsize;
		h_put("ok");
	} else if (strcmp(k, "load") == 0 || strcmp(k, "save") == 0) {
		unsigned p = part_arg(c, 1);
		const char *path = h_str(c, 2);
		size_t n = 0;

		if (k[0] == 'l') {
			uint8_t *buf = load_file(path, &n);

			if (!buf || n != bsize)
				h_error(c, "partition image");
			else
				memcpy(boot[p - 1u], buf, n);
			free(buf);
		} else {
			FILE *f = path ? fopen(path, "wb") : NULL;

			if (!f || fwrite(boot[p - 1u], 1, bsize, f) != bsize)
				h_error(c, "cannot write");
			if (f)
				fclose(f);
		}
		h_put("ok");
	} else if (strcmp(k, "zero") == 0) {
		memset(boot[part_arg(c, 1) - 1u], 0, MAX_B);
		h_put("ok");
	} else if (strcmp(k, "poke") == 0) {
		unsigned p = part_arg(c, 1);
		unsigned long off = h_num(c, 2);
		uint8_t buf[S03_SECTOR];
		size_t n = h_hex(c, 3, buf, sizeof buf);

		if (off > bsize || n > bsize - off)
			h_error(c, "range");
		else
			memcpy(boot[p - 1u] + off, buf, n);
		h_put("ok");
	} else if (strcmp(k, "pokefile") == 0) {
		unsigned p = part_arg(c, 1);
		unsigned long off = h_num(c, 2);
		size_t n = 0;
		uint8_t *buf = load_file(h_str(c, 3), &n);

		if (!buf || off > bsize || n > bsize - off)
			h_error(c, "range");
		else
			memcpy(boot[p - 1u] + off, buf, n);
		free(buf);
		h_put("ok");
	} else if (strcmp(k, "peek") == 0) {
		unsigned p = part_arg(c, 1);
		unsigned long off = h_num(c, 2), n = h_num(c, 3);

		if (off > bsize || n > bsize - off)
			h_error(c, "range");
		else
			h_puthex(boot[p - 1u] + off, n);
	} else if (strcmp(k, "fault") == 0) {
		cmd_fault(c);
	} else if (strcmp(k, "supported") == 0) {
		h_putu(s03_emmc_supported(&wb_device_opxy, (uint32_t)h_num(c, 1), (uint32_t)h_num(c, 2),
					  (uint32_t)h_num(c, 3)));
	} else if (strcmp(k, "region") == 0) {
		h_put(region_name(s03_user_region((uint32_t)h_num(c, 1))));
	} else if (strcmp(k, "fatmap") == 0) {
		h_putx(s03_fat_volume_to_user((uint32_t)h_num(c, 1)));
	} else {
		oplen = 0;
		oplog[0] = '\0';
		last[0] = '\0';
		last_failed = 0;
		muts = 0;
		memset(&prev, 0, sizeof prev);
		ffail.seen = 0;
		ffail.active = 0;
		fcorrupt.seen = 0;
		if (setjmp(cut) == 0)
			emmc_command(c);
		else
			h_put("cut");
		h_put("|");
		fputs(oplog, stdout);
	}
	h_end();
}

int main(void)
{
	struct h_cmd c;

	env.boot_size = bsize;
	while (h_next(&c))
		command(&c);
	return h_status();
}

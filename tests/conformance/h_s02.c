/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Conformance host program for S-02 (src/s02_flash.c, src/s02_write.c),
 * driven by test_s02.py.  It holds one 1 MiB NOR flash with S-02.019's
 * semantics and an operation log, with the fault and power-cut injection of
 * images.spiflash.FlashModel.  Each result is followed by " |" and the
 * operations the command made: r:OFF:LEN, e:OFF, p:OFF:LEN (hex).
 *
 *   load PATH | save PATH          -> ok           (the whole flash image)
 *   poke OFF HEX                   -> ok
 *   peek OFF LEN                   -> HEX
 *   fault none                     -> ok           (for the next command only, up to four,
 *                                                  each at its own op; none clears them:)
 *   fault after W                  -> ok           power cut before write op W+1
 *   fault fail W                   -> ok           write op W fails, does nothing
 *   fault tear W MASK              -> ok           write op W torn by MASK, then a cut
 *   fault stuck W                  -> ok           write op W changes nothing and reports success
 *   fault late W                   -> ok           write op W takes effect, then reports failure
 *   readfail OFF LEN ...           -> ok           from now on (until load or readfail -), reads
 *                                                  that touch these ranges fail (S-02.036)
 *   readfail                       -> N            failed reads since the last query
 *   readfail once N OFF LEN        -> ok           in the next command only, the N-th read
 *                                                  that touches the range fails, once
 *   fs NOWRITES                    -> BOOT BASE WROTE SEL CHECKED GOOD STATUS
 *   metawrite REC                  -> STATUS
 *   restore                        -> STATUS RESTORED
 *   writesel SLOT VERIFIED         -> STATUS
 *   upgrade OPTION PATH            -> STATUS TARGET    (the image from a file)
 *   recover RUNNING PATH [eager] [during] [leave] [leaverx] [swap AT FLASH]
 *                                  -> REPLIES PHASE RESTART HOLD ACCEPT FIRST  (S-02.084, S-02.093:
 *                                                  recovery's protocol, src/front/, fed the
 *                                                  host's bytes from PATH; RUNNING 0 or 1, 2
 *                                                  for not established; REPLIES the bytes
 *                                                  it sent, hex; RESTART 1 when a reset was
 *                                                  asked for; HOLD fr_proto_hold(). The host
 *                                                  waits for a write, and the countdown
 *                                                  before it, to end (1 ms a poll), or with eager
 *                                                  sends one request a poll but in the
 *                                                  countdown; with during, in it too; with
 *                                                  leave the host leaves (fr_proto_drop())
 *                                                  once, when a countdown begins; with
 *                                                  leaverx once, when DATA has come in a
 *                                                  transfer. ACCEPT and
 *                                                  FIRST: the harness's time in ms when a
 *                                                  write or action was last accepted (the
 *                                                  phase countdown or writing begun) and of
 *                                                  the first erase or program, or "-". swap:
 *                                                  the flash becomes the image FLASH once the
 *                                                  host has sent AT bytes, a frame boundary)
 *   startlog                       -> STEP STATE SLOT NEXT UNFINISHED  (S-02.095:
 *                                                  fr_log_start(); STEP go, marked or
 *                                                  recovery; STATE ok, none, odd, full or io)
 *   storeread SLOT EST GOOD [REC]  -> VALID FLAGS      (S-02.097: s02_store_read() for running
 *                                                  slot SLOT, established EST, its image good
 *                                                  GOOD, with the record REC (12 bytes, hex) as
 *                                                  the start read it, or the one the flash holds
 *                                                  now; FLAGS the settings read as the payload's
 *                                                  flag bits, VALID whether a record gave them)
 *   storewrite SLOT EST GOOD FLAGS [REC] -> RC         (S-02.098: s02_store_write(), enum s02_store_rc)
 *   storeclear SLOT EST GOOD [REC] -> RC               (S-02.105: s02_store_clear())
 *
 * Write ops are erases and programs, counted from 1 in each command; a cut
 * prints "cut" in place of the result.  STATUS is the enum s02_status value.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "front/fr_proto.h"
#include "front/fr_startlog.h"
#include "waffle/s01_memmap.h"
#include "waffle/s02_flash.h"
#include "waffle/s02_store.h"
#include "hlib.h"

static uint8_t flash[S02_FLASH_SIZE];
static uint8_t scratch[S02_SECTOR_SIZE];

enum fault { F_NONE = 0, F_AFTER, F_FAIL, F_TEAR, F_STUCK, F_LATE };

/* What write_begin() asks of the op. */
enum op_fate { OP_DO = 0, OP_FAIL, OP_STUCK, OP_LATE };

/* The faults for the next command, each at its own write op. */
#define MAX_FAULT 4
static enum fault fault_kind[MAX_FAULT];
static unsigned long fault_at[MAX_FAULT];
static unsigned fault_n;
static uint8_t tear_mask[S02_SECTOR_SIZE];
static size_t tear_len;
static unsigned long writes;
static uint64_t rec_now;	/* recover: the harness's clock, 1 ms a poll */
static uint64_t rec_first = UINT64_MAX;	/* recover: the first erase or program */
static jmp_buf cut;

/* Read faults (S-02.036): ranges whose reads fail. */
#define MAX_READFAIL 8
static unsigned long rf_off[MAX_READFAIL], rf_len[MAX_READFAIL];
static unsigned rf_n;
static unsigned long rf_count;

/* A one-shot read fault for the next command: the once_n-th read that
 * touches [once_off, once_off + once_len) fails; 0: none. */
static unsigned long once_n, once_off, once_len, once_seen;

static int read_fails(uint32_t off, uint32_t len)
{
	if (once_n != 0u && off < once_off + once_len && once_off < (unsigned long)off + len &&
	    ++once_seen == once_n)
		return 1;
	for (unsigned i = 0; i < rf_n; i++)
		if (off < rf_off[i] + rf_len[i] && rf_off[i] < (unsigned long)off + len)
			return 1;
	return 0;
}

/* The operation log, printed after the result. */
static char oplog[1u << 20];
static size_t oplen;

/* One entry: kind r, e or p, the offset, and the length unless an erase. */
static void logop(char kind, unsigned long off, unsigned long len)
{
	int n;

	if (oplen + 32u > sizeof oplog)
		return;
	if (kind == 'e')
		n = snprintf(oplog + oplen, sizeof oplog - oplen, " e:%lx", off);
	else
		n = snprintf(oplog + oplen, sizeof oplog - oplen, " %c:%lx:%lx", kind, off, len);
	if (n > 0)
		oplen += (size_t)n;
}

/* Starts write op number ++writes: what it does, or a cut. */
static enum op_fate write_begin(void)
{
	writes++;
	if (rec_first == UINT64_MAX)
		rec_first = rec_now;
	for (unsigned i = 0; i < fault_n; i++) {
		if (fault_kind[i] == F_AFTER && writes > fault_at[i])
			longjmp(cut, 1);
		if (writes != fault_at[i])
			continue;
		switch (fault_kind[i]) {
		case F_FAIL:
			return OP_FAIL;
		case F_STUCK:
			return OP_STUCK;
		case F_LATE:
			return OP_LATE;
		case F_NONE:
		case F_AFTER:
		case F_TEAR:
		default:
			break;
		}
	}
	return OP_DO;
}

static int torn(void)
{
	for (unsigned i = 0; i < fault_n; i++)
		if (fault_kind[i] == F_TEAR && writes == fault_at[i])
			return 1;
	return 0;
}

static uint8_t mask_at(size_t i)
{
	return i < tear_len ? tear_mask[i] : 0u;
}

static int f_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (off > S02_FLASH_SIZE || len > S02_FLASH_SIZE - off)
		return -1;
	logop('r', off, len);
	if (read_fails(off, len)) {
		rf_count++;
		return -1;
	}
	memcpy(buf, flash + off, len);
	return 0;
}

static int f_erase(void *ctx, uint32_t off)
{
	enum op_fate fate;

	(void)ctx;
	if (off % S02_SECTOR_SIZE != 0u || off >= S02_FLASH_SIZE)
		return -1;
	fate = write_begin();
	logop('e', off, 0);
	if (fate == OP_FAIL)
		return -1;
	if (fate == OP_STUCK)
		return 0;
	if (torn()) {
		size_t i;

		for (i = 0; i < S02_SECTOR_SIZE; i++)
			flash[off + i] |= mask_at(i);	/* each bit old or 1 */
		longjmp(cut, 1);
	}
	memset(flash + off, S02_ERASED, S02_SECTOR_SIZE);
	return fate == OP_LATE ? -1 : 0;
}

static int f_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	size_t i;
	enum op_fate fate;

	(void)ctx;
	if (off > S02_FLASH_SIZE || len > S02_FLASH_SIZE - off ||
	    (len > 0u && off / S02_PAGE_SIZE != (off + len - 1u) / S02_PAGE_SIZE))
		return -1;	/* S-02.002: within one page */
	fate = write_begin();
	logop('p', off, len);
	if (fate == OP_FAIL)
		return -1;
	if (fate == OP_STUCK)
		return 0;
	if (torn()) {
		for (i = 0; i < len; i++) {
			uint8_t clear = (uint8_t)(flash[off + i] & ~buf[i]);

			flash[off + i] &= (uint8_t)~(clear & mask_at(i));
		}
		longjmp(cut, 1);
	}
	for (i = 0; i < len; i++)
		flash[off + i] &= buf[i];	/* S-02.019 */
	return fate == OP_LATE ? -1 : 0;
}

static const struct s02_flash_ops flash_ops = { NULL, f_read, f_erase, f_program };
static const struct s02_env env = { &flash_ops, h_crc32, scratch, sizeof scratch, NULL };

/* --- files --- */

static uint8_t *load_file(const struct h_cmd *c, int i, size_t *n)
{
	const char *path = h_str(c, i);
	FILE *f = path ? fopen(path, "rb") : NULL;
	uint8_t *buf;
	long len;

	if (!f) {
		h_error(c, "cannot open file");
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(len > 0 ? (size_t)len : 1u);
	if (!buf || len < 0 || fread(buf, 1, (size_t)len, f) != (size_t)len) {
		fclose(f);
		free(buf);
		h_error(c, "cannot read file");
		return NULL;
	}
	fclose(f);
	*n = (size_t)len;
	return buf;
}

/* Recovery's serial port (S-02.093): the host's bytes in, its replies out. */
static uint8_t rec_staging[FR_SLOT_CAP];
static const uint8_t *rec_in;
static size_t rec_in_n, rec_in_at;
static uint8_t rec_out[1u << 20];
static size_t rec_out_n;

static const struct fr_proto *rec_p;
static bool rec_during;		/* the host sends in the countdown too */
static bool rec_eager;		/* the host does not wait for a write to end */
static bool rec_given;		/* eager: this poll had its request */
static const uint8_t *rec_swap;	/* a whole flash image that replaces the flash */
static size_t rec_swap_at;	/* once the host has sent this many bytes */

/* A host that sends one request at a time, the next only once a write,
 * with the countdown before it, has ended (eager: one a poll, a write or
 * not, but none in the countdown): at most up to the end of the frame that
 * starts here. */
static size_t rio_read(void *ctx, uint8_t *buf, size_t max)
{
	size_t k = rec_in_n - rec_in_at < max ? rec_in_n - rec_in_at : max;

	(void)ctx;
	if (rec_p != NULL && rec_p->phase == FR_PH_COUNTDOWN && !rec_during)
		return 0;
	if (rec_eager) {
		if (rec_given)
			return 0;
		rec_given = true;
	} else if (rec_p != NULL && rec_p->phase == FR_PH_WRITING) {
		return 0;
	}
	if (rec_swap != NULL && rec_in_at >= rec_swap_at) {
		memcpy(flash, rec_swap, sizeof flash);	/* the flash changes under recovery */
		rec_swap = NULL;
	}
	if (rec_swap != NULL && rec_swap_at - rec_in_at < k)
		k = rec_swap_at - rec_in_at;
	if (rec_in_n - rec_in_at >= FR_PROTO_HEAD && rec_in[rec_in_at] == 'W' &&
	    rec_in[rec_in_at + 1u] == 'R') {
		size_t frame = FR_PROTO_HEAD + 4u + (size_t)(rec_in[rec_in_at + 4u] |
							     rec_in[rec_in_at + 5u] << 8);

		if (frame < k)
			k = frame;
	}
	memcpy(buf, rec_in + rec_in_at, k);
	rec_in_at += k;
	return k;
}

static size_t rio_write(void *ctx, const uint8_t *buf, size_t n)
{
	size_t k = sizeof rec_out - rec_out_n < n ? sizeof rec_out - rec_out_n : n;

	(void)ctx;
	memcpy(rec_out + rec_out_n, buf, k);
	rec_out_n += k;
	return k;
}

static bool rio_idle(void *ctx)
{
	(void)ctx;
	return true;
}

/* S-02.084: recovery's start-up survey from the flash, then its protocol
 * until every host byte is answered and no write runs. */
static void cmd_recover(const struct h_cmd *c)
{
	static const struct fr_flash ff = { f_read, f_erase, f_program, NULL };
	static const struct fr_bytes io = { rio_read, rio_write, rio_idle, NULL };
	static struct fr_rec_status st;
	static struct fr_proto p;
	unsigned long run = h_num(c, 1), i;
	uint64_t accept = UINT64_MAX;
	bool leave = false, leave_rx = false, left = false;
	size_t n, swap_n = 0;
	uint8_t *in = load_file(c, 2, &n), *swap = NULL;
	int a;

	if (!in)
		return;
	rec_eager = false;
	rec_swap = NULL;
	for (a = 3; a < c->n; a++) {
		if (strcmp(h_str(c, a), "eager") == 0) {
			rec_eager = true;
		} else if (strcmp(h_str(c, a), "during") == 0) {
			rec_during = true;
		} else if (strcmp(h_str(c, a), "leave") == 0) {
			leave = true;
		} else if (strcmp(h_str(c, a), "leaverx") == 0) {
			leave_rx = true;
		} else if (strcmp(h_str(c, a), "swap") == 0 && a + 2 < c->n && swap == NULL) {
			rec_swap_at = h_num(c, a + 1);
			swap = load_file(c, a + 2, &swap_n);
			if (swap == NULL || swap_n != sizeof flash) {
				h_error(c, "swap image");
				free(swap);
				free(in);
				return;
			}
			rec_swap = swap;
			a += 2;
		} else {
			h_error(c, "recover option");
		}
	}
	memset(&st, 0, sizeof st);
	st.why = FR_WHY_KEYS;
	st.running.established = run < 2u;
	st.running.slot = (uint8_t)(run < 2u ? run : 0u);
	fr_rec_survey(&st, &ff);	/* as fr_recovery.c's start */
	rec_in = in;
	rec_in_n = n;
	rec_in_at = 0;
	rec_out_n = 0;
	rec_p = &p;
	fr_proto_init(&p, &io, &st, &ff, rec_staging);
	rec_now = 0;
	rec_first = UINT64_MAX;
	fr_proto_clock(&p, rec_now);
	/* bounded: each poll answers a request or steps the writer; 1 ms a
	 * pass, so that a countdown runs out (S-02.093) */
	for (i = 0; i < 1000000ul; i++) {
		const uint8_t was = p.phase;

		rec_given = false;
		fr_proto_poll(&p);
		if ((p.phase == FR_PH_COUNTDOWN || p.phase == FR_PH_WRITING) &&
		    was != FR_PH_COUNTDOWN && was != FR_PH_WRITING)
			accept = rec_now;
		if (!left && ((leave && p.phase == FR_PH_COUNTDOWN) ||
			      (leave_rx && p.phase == FR_PH_RECEIVING && p.got > 0u))) {
			fr_proto_drop(&p);	/* the host leaves (S-02.084) */
			left = true;
		}
		rec_now += 1000u;
		fr_proto_clock(&p, rec_now);
		if (rec_in_at == rec_in_n && p.rx_n == 0 && p.tx_n == 0 &&
		    p.phase != FR_PH_WRITING && p.phase != FR_PH_COUNTDOWN)
			break;
	}
	free(in);
	free(swap);
	rec_eager = false;
	rec_during = false;
	rec_swap = NULL;
	h_puthex(rec_out, rec_out_n);
	h_putu(p.phase);
	h_putu(p.restart || p.restart_due);
	h_putu(fr_proto_hold(&p));
	if (accept == UINT64_MAX)
		h_put("-");
	else
		h_putu((unsigned long)(accept / 1000u));
	if (rec_first == UINT64_MAX)
		h_put("-");
	else
		h_putu((unsigned long)(rec_first / 1000u));
}

/* S-02.095: the front's step for the start log on the loaded flash. */
/* The running slot of a store command: SLOT EST GOOD in fields 1 to 3, and
 * the record the start read in field `rec`, or the flash's record now (read
 * from the array, so no operation is logged). */
static void store_run(const struct h_cmd *c, struct s02_store_slot *run, int rec)
{
	uint8_t raw[S02_RECORD_SIZE];

	run->slot = (unsigned)h_num(c, 1);
	run->established = h_num(c, 2) != 0;
	run->good = h_num(c, 3) != 0;
	if (h_str(c, rec) != NULL) {
		if (h_hex(c, rec, raw, sizeof raw) != sizeof raw)
			h_error(c, "bad record");
	} else {
		memcpy(raw, flash + (run->slot == 0u ? S02_RECORD0_OFF : S02_RECORD1_OFF), sizeof raw);
	}
	s02_record_decode(raw, &run->rec);
}

static unsigned long store_flags(const struct s02_store *s)
{
	return (s->dev_mode ? S02_STORE_FLAG_DEV : 0u) | (s->color ? S02_STORE_FLAG_COLOR : 0u) |
	       (s->mtp_macos ? S02_STORE_FLAG_MTP_MACOS : 0u) |
	       (s->start_menu ? S02_STORE_FLAG_START_MENU : 0u);
}

static void cmd_startlog(void)
{
	static const struct fr_flash ff = { f_read, f_erase, f_program, NULL };
	static const char *const states[] = { "ok", "none", "odd", "full", "io" };
	struct fr_log l;
	enum fr_log_step step = fr_log_start(&ff, &l);

	h_put(step == FR_LOG_RECOVERY ? "recovery" : step == FR_LOG_GO_MARKED ? "marked" : "go");
	h_put(l.state < sizeof states / sizeof states[0] ? states[l.state] : "?");
	h_putu(l.slot);
	h_putu(l.next);
	h_putu(l.unfinished);
}

static void cmd_load(const struct h_cmd *c)
{
	size_t n;
	uint8_t *buf = load_file(c, 1, &n);

	if (!buf)
		return;
	if (n != sizeof flash)
		h_error(c, "flash image size");
	else
		memcpy(flash, buf, n);
	free(buf);
	h_put("ok");
}

static void cmd_save(const struct h_cmd *c)
{
	const char *path = h_str(c, 1);
	FILE *f = path ? fopen(path, "wb") : NULL;

	if (!f || fwrite(flash, 1, sizeof flash, f) != sizeof flash)
		h_error(c, "cannot write file");
	if (f)
		fclose(f);
	h_put("ok");
}

static void cmd_fault(const struct h_cmd *c)
{
	const char *k = h_str(c, 1);
	enum fault f = F_NONE;

	if (k && strcmp(k, "after") == 0)
		f = F_AFTER;
	else if (k && strcmp(k, "fail") == 0)
		f = F_FAIL;
	else if (k && strcmp(k, "tear") == 0)
		f = F_TEAR;
	else if (k && strcmp(k, "stuck") == 0)
		f = F_STUCK;
	else if (k && strcmp(k, "late") == 0)
		f = F_LATE;
	else if (!k || strcmp(k, "none") != 0)
		h_error(c, "fault kind");
	if (f == F_NONE) {
		fault_n = 0;
	} else if (fault_n == MAX_FAULT) {
		h_error(c, "too many faults");
	} else {
		fault_kind[fault_n] = f;
		fault_at[fault_n++] = h_num(c, 2);
	}
	if (f == F_TEAR)
		tear_len = h_hex(c, 3, tear_mask, sizeof tear_mask);
	h_put("ok");
}

/* The commands that touch the flash; run under the cut's setjmp. */
static void flash_command(const struct h_cmd *c)
{
	const char *k = c->tok[0];

	if (strcmp(k, "fs") == 0) {
		struct s02_fs_outcome o;
		uint8_t sel[2];

		s02_first_stage(&env, h_num(c, 1) != 0, &o);
		sel[0] = o.written.index;
		sel[1] = o.written.verified;
		h_putu(o.boot_slot);
		h_putx(S01_SPI2_WINDOW + s02_slot_base(o.boot_slot));
		h_putu(o.wrote);
		h_puthex(sel, o.wrote ? 2u : 0u);
		h_putu(o.checked);
		h_putu(o.selected_good);
		h_putu((unsigned long)o.status);
	} else if (strcmp(k, "metawrite") == 0) {
		uint8_t rec[S02_META_SIZE];

		if (h_hex(c, 1, rec, sizeof rec) != sizeof rec) {
			h_error(c, "record size");
			return;
		}
		h_putu((unsigned long)s02_meta_write(&env, rec));
	} else if (strcmp(k, "restore") == 0) {
		bool restored;
		int rc = s02_meta_restore(&env, &restored);

		h_putu((unsigned long)rc);
		h_putu(restored);
	} else if (strcmp(k, "writesel") == 0) {
		h_putu((unsigned long)s02_write_selector(&env, (unsigned)h_num(c, 1),
							 (uint8_t)h_num(c, 2)));
	} else if (strcmp(k, "upgrade") == 0) {
		size_t n;
		uint8_t *img = load_file(c, 2, &n);
		unsigned target = 2;

		if (!img)
			return;
		h_putu((unsigned long)s02_slot_upgrade(&env, h_num(c, 1) != 0, img,
						       (uint32_t)n, &target));
		h_putu(target);
		free(img);
	} else if (strcmp(k, "recover") == 0) {
		cmd_recover(c);
	} else if (strcmp(k, "startlog") == 0) {
		cmd_startlog();
	} else if (strcmp(k, "storeread") == 0) {
		struct s02_store_slot run;
		struct s02_store s;
		bool valid;

		store_run(c, &run, 4);
		valid = s02_store_read(&env, &run, &s);
		h_putu(valid);
		h_putx(store_flags(&s));
	} else if (strcmp(k, "storewrite") == 0) {
		struct s02_store_slot run;
		struct s02_store s;
		const unsigned long f = h_num(c, 4);

		store_run(c, &run, 5);
		s.dev_mode = (f & S02_STORE_FLAG_DEV) != 0u;
		s.color = (f & S02_STORE_FLAG_COLOR) != 0u;
		s.mtp_macos = (f & S02_STORE_FLAG_MTP_MACOS) != 0u;
		s.start_menu = (f & S02_STORE_FLAG_START_MENU) != 0u;
		h_putu((unsigned long)s02_store_write(&env, &run, &s));
	} else if (strcmp(k, "storeclear") == 0) {
		struct s02_store_slot run;

		store_run(c, &run, 4);
		h_putu((unsigned long)s02_store_clear(&env, &run));
	} else {
		h_error(c, "unknown command");
	}
}

static void command(const struct h_cmd *c)
{
	const char *k = c->tok[0];

	if (strcmp(k, "load") == 0) {
		rf_n = 0;
		cmd_load(c);
	} else if (strcmp(k, "save") == 0) {
		cmd_save(c);
	} else if (strcmp(k, "poke") == 0) {
		uint8_t buf[4096];
		unsigned long off = h_num(c, 1);
		size_t n = h_hex(c, 2, buf, sizeof buf);

		if (off > sizeof flash || n > sizeof flash - off)
			h_error(c, "range");
		else
			memcpy(flash + off, buf, n);
		h_put("ok");
	} else if (strcmp(k, "peek") == 0) {
		unsigned long off = h_num(c, 1), n = h_num(c, 2);

		if (off > sizeof flash || n > sizeof flash - off) {
			h_error(c, "range");
			return;
		}
		h_puthex(flash + off, n);
	} else if (strcmp(k, "readfail") == 0) {
		if (c->n == 1) {
			h_putu(rf_count);
			rf_count = 0;
		} else if (c->n == 5 && strcmp(h_str(c, 1), "once") == 0) {
			once_n = h_num(c, 2);
			once_off = h_num(c, 3);
			once_len = h_num(c, 4);
			once_seen = 0;
			h_put("ok");
			h_end();
			return;		/* it stays for the next command */
		} else if (c->n == 2 && strcmp(h_str(c, 1), "-") == 0) {
			rf_n = 0;
			h_put("ok");
		} else if (c->n % 2 == 1 && (unsigned)(c->n - 1) / 2u <= MAX_READFAIL) {
			rf_n = 0;
			for (int i = 1; i + 1 < c->n; i += 2) {
				rf_off[rf_n] = h_num(c, i);
				rf_len[rf_n++] = h_num(c, i + 1);
			}
			rf_count = 0;
			h_put("ok");
		} else {
			h_error(c, "readfail ranges");
		}
		h_end();
		return;
	} else if (strcmp(k, "fault") == 0) {
		cmd_fault(c);
		h_end();
		return;		/* the fault stays for the next command */
	} else {
		writes = 0;
		oplen = 0;
		oplog[0] = '\0';
		if (setjmp(cut) == 0)
			flash_command(c);
		else
			h_put("cut");
		h_put("|");
		fputs(oplog, stdout);
	}
	fault_n = 0;
	once_n = 0;
	h_end();
}

int main(void)
{
	struct h_cmd c;

	memset(flash, S02_ERASED, sizeof flash);
	while (h_next(&c))
		command(&c);
	return h_status();
}

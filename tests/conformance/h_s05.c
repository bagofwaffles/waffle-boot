/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for the S-05 hand-off logic (src/s05_handoff.c).
 *
 * Commands (one result line each):
 *   decide REASON STATE KEYS [SLAVE] -> BOOT_OS INTERRUPTED   (KEYS 0 not held, 1 held, 2 silent;
 *                                                             SLAVE 0 absent, 1 present, the default)
 *   check IMAGE                    -> refusal number (enum s05_refusal)
 *   walk IMAGE                     -> OFF:CODE:TARGET:COUNT:ARG per block, then "end" or "fail"
 *   cfg HEADER_BUFFER LOAD_FN      -> the 600-byte configuration block, hex
 *   load IMAGE HEADER_BUFFER       -> P:D:C per load-function call | T:C:CRC per range | entry E
 *   master STAGE IMAGE RELEASE NZ  -> the op log of s05_master_handoff
 *   slave STAGE IMAGE              -> the op log of s05_slave_release, then ret:0|1
 *   boot REASON STATE KEYS SLAVE STORE PROMPT STAGE IMAGE RELEASE NZ
 *                                  -> the op log of boot_master (src/boot_flow.c): store (the
 *                                     start in boot menu op, S-15.175), prompt (S-05.025),
 *                                     the hand-off's ops, held:LOADED:CAUSE:INTERRUPTED:ABSENT.
 *                                     STORE -1: no op, else its answer; PROMPT 0: no op, else
 *                                     the digit it returns (1 start, 2 held)
 *
 * IMAGE is a file path, or zero:N for N zero bytes.  STAGE is the stage
 * callback's result (enum s05_fail); 0 stages IMAGE.  RELEASE is the reply
 * byte, or "none" when no reply comes.  NZ is sw_version_nonzero.
 *
 * The kernel call runs a model of the boot ROM (glue, after
 * tests/oracle/S-05-os-handoff/kernel_model.py): it asks the implementation's load
 * function, s05_load_step, for each header and payload, does FILL blocks
 * itself, and leaves the FINAL block's target as RCU0_SVECT0.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hlib.h"
#include "waffle/boot.h"
#include "waffle/s01_memmap.h"
#include "waffle/s05_handoff.h"

#define HB_DEFAULT  0x88A34D5Cu	/* the header buffer the vectors use */
#define LOADFN      0x88A34E00u
#define CONFIG_ADDR 0x88A34A00u
#define MAX_RANGES  4096

/* One field, formatted. */
static void putf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void putf(const char *fmt, ...)
{
	char buf[96];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	h_put(buf);
}

static uint8_t staging[S01_STAGING_MAX + 64];
static uint32_t staged_len;

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Fills staging from IMAGE; false when it does not fit or cannot be read. */
static bool read_image(const char *arg)
{
	FILE *f;
	size_t n;

	if (arg == NULL)
		return false;
	if (strncmp(arg, "zero:", 5) == 0) {
		unsigned long v = strtoul(arg + 5, NULL, 0);

		if (v > sizeof staging)
			return false;
		memset(staging, 0, (size_t)v);
		staged_len = (uint32_t)v;
		return true;
	}
	f = fopen(arg, "rb");
	if (f == NULL)
		return false;
	n = fread(staging, 1, sizeof staging, f);
	staged_len = (uint32_t)n;
	fclose(f);
	return n < sizeof staging;
}

/* --- the boot ROM model --- */

struct rom {
	uint32_t hb;
	uint8_t hdr[S05_LDR_HEADER];
	bool bad;			/* the load function read outside the image */
	unsigned ncalls, nranges;
	uint32_t call[MAX_RANGES][3];
	uint32_t range[MAX_RANGES][3];	/* target, count, crc */
	uint32_t svect0;
};

static void copy(void *ctx, uint32_t dest, const uint8_t *src, uint32_t count)
{
	struct rom *r = ctx;
	size_t off = (size_t)(src - staging);

	if (off + count > staged_len) {
		r->bad = true;
		return;
	}
	if (dest == r->hb && count == S05_LDR_HEADER) {
		memcpy(r->hdr, src, S05_LDR_HEADER);
	} else if (r->nranges < MAX_RANGES) {
		r->range[r->nranges][0] = dest;
		r->range[r->nranges][1] = count;
		r->range[r->nranges][2] = h_crc32(0, src, count);
		r->nranges++;
	}
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void rom_call(struct rom *r, uint8_t *cfg, uint32_t dest, uint32_t count)
{
	put32(cfg + S05_CFG_PDEST, dest);
	put32(cfg + S05_CFG_COUNT, count);
	if (r->ncalls < MAX_RANGES) {
		r->call[r->ncalls][0] = le32(cfg + S05_CFG_PSOURCE);
		r->call[r->ncalls][1] = dest;
		r->call[r->ncalls][2] = count;
		r->ncalls++;
	}
	s05_load_step(cfg, staging, copy, r);
}

/* 0 at the FINAL block, -1 when the stream breaks the model. */
static int rom_kernel(struct rom *r, uint32_t hb)
{
	uint8_t cfg[S05_CONFIG_SIZE];
	unsigned blocks;

	memset(r, 0, sizeof *r);
	r->hb = hb;
	s05_config_build(cfg, hb, LOADFN);
	for (blocks = 0; blocks < MAX_RANGES; blocks++) {
		uint32_t code, target, count, arg;

		rom_call(r, cfg, hb, S05_LDR_HEADER);
		if (r->bad || r->hdr[3] != S05_LDR_SIGNATURE)
			return -1;
		code = le32(r->hdr);
		target = le32(r->hdr + 4);
		count = le32(r->hdr + 8);
		arg = le32(r->hdr + 12);
		if (code & S05_FLAG_FILL) {
			if (count != 0u && r->nranges < MAX_RANGES) {
				uint32_t crc = 0, i;
				uint8_t w[4];

				put32(w, arg);
				for (i = 0; i < count; i++)
					crc = h_crc32(crc, &w[i % 4u], 1);
				r->range[r->nranges][0] = target;
				r->range[r->nranges][1] = count;
				r->range[r->nranges][2] = crc;
				r->nranges++;
			}
		} else if (code & S05_FLAG_IGNORE) {
			if (count != 0u)
				return -1;
		} else if (count != 0u) {
			rom_call(r, cfg, target, count);
			if (r->bad)
				return -1;
		}
		if (code & S05_FLAG_FINAL) {
			r->svect0 = target;
			return 0;
		}
	}
	return -1;
}

static struct rom rom;

static void do_load(const struct h_cmd *c)
{
	unsigned i;
	int rc;

	if (!read_image(h_str(c, 1))) {
		h_error(c, "image");
		return;
	}
	rc = rom_kernel(&rom, c->n > 2 ? (uint32_t)h_num(c, 2) : HB_DEFAULT);
	for (i = 0; i < rom.ncalls; i++) {
		putf("0x%x:0x%x:0x%x", (unsigned)rom.call[i][0],
		       (unsigned)rom.call[i][1], (unsigned)rom.call[i][2]);
	}
	h_put("|");
	for (i = 0; i < rom.nranges; i++) {
		putf("0x%x:0x%x:0x%x", (unsigned)rom.range[i][0], (unsigned)rom.range[i][1],
		       (unsigned)rom.range[i][2]);
	}
	h_put("|");
	if (rc == 0) {
		h_put("entry");
		h_putx(rom.svect0);
	} else {
		h_put("fail");
	}
	h_end();
}

/* --- the sequences, with logged ops --- */

struct seq {
	unsigned long stage;
	const char *image;
	long reply;		/* -1: no reply */
	uint32_t svect0;
};

static struct seq cur;		/* the sequence the callbacks follow */

static void op(const char *s)
{
	h_put(s);
}

static void m_pre_load(void *ctx)
{
	(void)ctx;
	op("pre");
}

static enum s05_fail m_stage(void *ctx, uint32_t *length)
{
	const struct seq *s = &cur;

	(void)ctx;
	op("stage");
	if (s->stage != 0u)
		return (enum s05_fail)s->stage;
	if (!read_image(s->image))
		return S05_FAIL_NO_OS;
	*length = staged_len;
	return S05_FAIL_NONE;
}

static int m_release(void *ctx, uint8_t *reply)
{
	const struct seq *s = &cur;

	(void)ctx;
	op("release");
	if (s->reply < 0)
		return -1;
	*reply = (uint8_t)s->reply;
	return 0;
}

static void m_teardown(void *ctx)
{
	(void)ctx;
	op("teardown");
}

static void m_kernel_call(void *ctx, uint32_t config_addr)
{
	(void)ctx;
	cur.svect0 = rom_kernel(&rom, HB_DEFAULT) == 0 ? rom.svect0 : 0xFFFFFFFFu;
	putf("kernel:0x%x:%u", (unsigned)config_addr, rom.ncalls);
}

static uint32_t m_read_svect0(void *ctx)
{
	const struct seq *s = &cur;

	(void)ctx;
	op("svect");
	return s->svect0;
}

static void m_jump(void *ctx, uint32_t entry)
{
	(void)ctx;
	putf("jump:0x%x", (unsigned)entry);
}

static void m_clear(void *ctx)
{
	(void)ctx;
	op("clear");
}

static void m_held(void *ctx, enum s05_fail cause)
{
	(void)ctx;
	putf("held:%u", (unsigned)cause);
}

static void m_reply(void *ctx, uint8_t value)
{
	(void)ctx;
	putf("reply:%u", (unsigned)value);
}

static void m_stop_server(void *ctx)
{
	(void)ctx;
	op("stop");
}

static void parse_seq(const struct h_cmd *c, struct seq *s)
{
	memset(s, 0, sizeof *s);
	s->stage = h_num(c, 1);
	s->image = h_str(c, 2);
	s->reply = -1;
	if (c->n > 3 && strcmp(c->tok[3], "none") != 0)
		s->reply = (long)h_num(c, 3);
}

static void do_master(const struct h_cmd *c)
{
	struct s05_master_ops ops;
	struct s05_master m;

	if (c->n != 5) {
		h_error(c, "fields");
		return;
	}
	parse_seq(c, &cur);
	memset(&ops, 0, sizeof ops);
	ops.ctx = &cur;
	ops.pre_load = m_pre_load;
	ops.stage = m_stage;
	ops.release = m_release;
	ops.teardown = m_teardown;
	ops.kernel_call = m_kernel_call;
	ops.read_svect0 = m_read_svect0;
	ops.jump = m_jump;
	ops.clear_sw_version = m_clear;
	ops.held = m_held;
	m.staging = staging;
	m.config_addr = CONFIG_ADDR;
	m.sw_version_nonzero = h_num(c, 4) != 0u;
	m.file = false;
	m.dev = &wb_device_opxy;
	h_put("begin");
	s05_master_handoff(&ops, &m);
	h_end();
}

static void do_slave(const struct h_cmd *c)
{
	struct s05_slave_ops ops;
	bool ok;

	if (c->n != 3) {
		h_error(c, "fields");
		return;
	}
	parse_seq(c, &cur);
	memset(&ops, 0, sizeof ops);
	ops.ctx = &cur;
	ops.stage = m_stage;
	ops.reply = m_reply;
	ops.stop_server = m_stop_server;
	ops.teardown = m_teardown;
	ops.kernel_call = m_kernel_call;
	ops.read_svect0 = m_read_svect0;
	ops.jump = m_jump;
	h_put("begin");
	ok = s05_slave_release(&ops, staging, CONFIG_ADDR);
	h_put(ok ? "ret:1" : "ret:0");
	h_end();
}

/* --- the boot flow (src/boot_flow.c), with logged ops --- */

static struct {
	uint16_t reason;
	uint32_t state;
	enum s05_keys keys;
	bool present, nonzero;
	long store;		/* -1: no op */
	int prompt;		/* 0: no op */
} bt;

static struct s05_master_ops boot_handoff;
static struct boot_master_ops boot_ops;

static void b_bringup(void *ctx)
{
	(void)ctx;
}

static bool b_ddr_check(void *ctx)
{
	(void)ctx;
	return true;
}

static void b_ddr_fatal(void *ctx)
{
	(void)ctx;
	op("fatal");
}

static void b_metadata(void *ctx, uint32_t *state, bool *nonzero)
{
	(void)ctx;
	*state = bt.state;
	*nonzero = bt.nonzero;
}

static uint16_t b_reason(void *ctx)
{
	(void)ctx;
	return bt.reason;
}

static enum s05_keys b_psoc_start(void *ctx)
{
	(void)ctx;
	return bt.keys;
}

static bool b_slave_start(void *ctx)
{
	(void)ctx;
	return bt.present;
}

static void b_held(void *ctx, const struct boot_held *h)
{
	(void)ctx;
	putf("held:%u:%u:%u:%u", (unsigned)h->load_attempted, (unsigned)h->cause,
	     (unsigned)h->interrupted, (unsigned)h->slave_absent);
}

static int b_prompt(void *ctx)
{
	(void)ctx;
	op("prompt");
	return bt.prompt;
}

static bool b_store(void *ctx)
{
	(void)ctx;
	op("store");
	return bt.store > 0;
}

/* The board's held op of the hand-off: the boot's state, then held mode. */
static void b_load_failed(void *ctx, enum s05_fail cause)
{
	(void)ctx;
	boot_master_load_failed(&boot_ops, cause, NULL);
}

static void do_boot(const struct h_cmd *c)
{
	if (c->n != 11) {
		h_error(c, "fields");
		return;
	}
	memset(&bt, 0, sizeof bt);
	bt.reason = (uint16_t)h_num(c, 1);
	bt.state = (uint32_t)h_num(c, 2);
	bt.keys = (enum s05_keys)h_num(c, 3);
	bt.present = h_num(c, 4) != 0u;
	bt.store = strcmp(h_str(c, 5), "-1") == 0 ? -1 : (long)h_num(c, 5);
	bt.prompt = (int)h_num(c, 6);
	bt.nonzero = h_num(c, 10) != 0u;
	memset(&cur, 0, sizeof cur);
	cur.stage = h_num(c, 7);
	cur.image = h_str(c, 8);
	cur.reply = strcmp(h_str(c, 9), "none") == 0 ? -1 : (long)h_num(c, 9);

	memset(&boot_handoff, 0, sizeof boot_handoff);
	boot_handoff.ctx = &cur;
	boot_handoff.pre_load = m_pre_load;
	boot_handoff.stage = m_stage;
	boot_handoff.release = m_release;
	boot_handoff.teardown = m_teardown;
	boot_handoff.kernel_call = m_kernel_call;
	boot_handoff.read_svect0 = m_read_svect0;
	boot_handoff.jump = m_jump;
	boot_handoff.clear_sw_version = m_clear;
	boot_handoff.held = b_load_failed;

	memset(&boot_ops, 0, sizeof boot_ops);
	boot_ops.bringup = b_bringup;
	boot_ops.ddr_check = b_ddr_check;
	boot_ops.ddr_fatal = b_ddr_fatal;
	boot_ops.metadata = b_metadata;
	boot_ops.reboot_reason = b_reason;
	boot_ops.psoc_start = b_psoc_start;
	boot_ops.slave_start = b_slave_start;
	boot_ops.dev = &wb_device_opxy;
	boot_ops.handoff = &boot_handoff;
	boot_ops.staging = staging;
	boot_ops.config_addr = CONFIG_ADDR;
	boot_ops.held = b_held;
	boot_ops.boot_prompt = bt.prompt != 0 ? b_prompt : NULL;
	boot_ops.start_in_menu = bt.store >= 0 ? b_store : NULL;
	h_put("begin");
	boot_master(&boot_ops);
	h_end();
}

static void do_walk(const struct h_cmd *c)
{
	struct s05_block b;
	uint32_t off = 0;
	unsigned n;

	if (!read_image(h_str(c, 1))) {
		h_error(c, "image");
		return;
	}
	for (n = 0; n < MAX_RANGES; n++) {
		if (s05_ldr_next(staging, staged_len, &off, &b) != 0) {
			h_put("fail");
			h_end();
			return;
		}
		putf("0x%x:0x%x:0x%x:0x%x:0x%x", (unsigned)b.offset, (unsigned)b.code,
		       (unsigned)b.target, (unsigned)b.count, (unsigned)b.argument);
		if (b.flags & S05_FLAG_FINAL)
			break;
	}
	h_put("end");
	h_end();
}

int main(void)
{
	struct h_cmd c;

	while (h_next(&c)) {
		const char *k = c.tok[0];

		if (strcmp(k, "decide") == 0 && (c.n == 4 || c.n == 5)) {
			struct s05_decision d;

			s05_boot_decision((uint16_t)h_num(&c, 1), (uint32_t)h_num(&c, 2),
					  (enum s05_keys)h_num(&c, 3),
					  c.n == 4 || h_num(&c, 4) != 0, &d);
			h_putu(d.boot_os);
			h_putu(d.interrupted);
			h_end();
		} else if (strcmp(k, "check") == 0 && c.n == 2) {
			if (!read_image(h_str(&c, 1))) {
				h_error(&c, "image");
				continue;
			}
			h_putu((unsigned long)s05_check_image(staging, staged_len));
			h_end();
		} else if (strcmp(k, "walk") == 0 && c.n == 2) {
			do_walk(&c);
		} else if (strcmp(k, "cfg") == 0 && c.n == 3) {
			uint8_t cfg[S05_CONFIG_SIZE];

			memset(cfg, 0xA5, sizeof cfg);
			s05_config_build(cfg, (uint32_t)h_num(&c, 1), (uint32_t)h_num(&c, 2));
			h_puthex(cfg, sizeof cfg);
			h_end();
		} else if (strcmp(k, "load") == 0 && (c.n == 2 || c.n == 3)) {
			do_load(&c);
		} else if (strcmp(k, "master") == 0) {
			do_master(&c);
		} else if (strcmp(k, "slave") == 0) {
			do_slave(&c);
		} else if (strcmp(k, "boot") == 0) {
			do_boot(&c);
		} else {
			h_error(&c, "unknown command");
		}
	}
	return h_status();
}

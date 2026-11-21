/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Conformance host program for S-04 (src/s04_records.c), driven by
 * test_s04.py.  Commands (hlib.h notation; v is a version a.b.c.d):
 *
 *   consume REC8          -> REASON REC8      (the record after the read)
 *   write REC8 REASON     -> REC8
 *   effect REASON         -> BOOTS SCREEN CDC TRANSPORT
 *   hostmode PAYLOAD      -> REASON
 *   nordic REASON         -> enter|exit|none
 *   writer W ARG          -> REASON RESET NORDIC SHUTDOWN PSOC_RESET DELAY
 *   reboot W ARG          -> OP...            (the operations, in order)
 *   cdc NAME:REG...       -> 0|1              (NAME hex, REG 0|1)
 *   metaread RAW          -> SERIAL SKU FACTORY v v STATE CVGAIN CVZERO CV2A CV2B
 *   metabuild BEFORE SERIAL SKU FACTORY v v STATE FACTORYMODE -> RAW
 *   m6 v                  -> 0|1
 *   setmeta FM OPT SERIAL SKU FACTORY REQSERIAL REQSKU REQFACTORY
 *                         -> WRITES SERIAL SKU FACTORY
 *                            (request strings hex, "x" when absent)
 *   rtcenc Y M D h m s    -> ok CLK | refused
 *   rtcdec CLK            -> Y M D h m s
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "waffle/s04_records.h"
#include "hlib.h"

static const char *nordic_name(enum s04_nordic n)
{
	switch (n) {
	case S04_NORDIC_ENTER_DFU:
		return "enter";
	case S04_NORDIC_EXIT_DFU:
		return "exit";
	case S04_NORDIC_NONE:
	default:
		return "none";
	}
}

static int version(const struct h_cmd *c, int i, uint16_t v[4])
{
	const char *s = h_str(c, i);
	unsigned long x[4];
	char tail;
	int k;

	if (!s || sscanf(s, "%lu.%lu.%lu.%lu%c", &x[0], &x[1], &x[2], &x[3], &tail) != 4) {
		h_error(c, "bad version");
		return 0;
	}
	for (k = 0; k < 4; k++)
		v[k] = (uint16_t)x[k];
	return 1;
}

static void put_version(const uint16_t v[4])
{
	char b[32];

	snprintf(b, sizeof b, "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
	h_put(b);
}

/* --- reboot: the operations, recorded --- */

static void op_psoc_reset(void *ctx)
{
	(void)ctx;
	h_put("psoc_reset");
}

static void op_shutdown(void *ctx)
{
	(void)ctx;
	h_put("shutdown");
}

static void op_enter(void *ctx)
{
	(void)ctx;
	h_put("enter");
}

static void op_exit(void *ctx)
{
	(void)ctx;
	h_put("exit");
}

static void op_link_stop(void *ctx)
{
	(void)ctx;
	h_put("link_stop");
}

static void op_delay(void *ctx, uint32_t ms)
{
	char b[32];

	(void)ctx;
	snprintf(b, sizeof b, "delay:%lu", (unsigned long)ms);
	h_put(b);
}

/* S-13b.072's wait: no step S-04's vectors name. */
static void op_drain(void *ctx)
{
	(void)ctx;
}

static void op_write(void *ctx, uint16_t reason)
{
	char b[32];

	(void)ctx;
	snprintf(b, sizeof b, "write:%u", reason);
	h_put(b);
}

static void op_hard(void *ctx)
{
	(void)ctx;
	h_put("hard");
}

static void op_soft(void *ctx)
{
	(void)ctx;
	h_put("soft");
}

static const struct s04_reboot_ops reboot_ops = {
	NULL, op_psoc_reset, op_shutdown, op_enter, op_exit, op_link_stop,
	op_delay, op_drain, op_write, op_hard, op_soft,
};

/* --- metadata --- */

static void put_meta(const struct s04_meta *m)
{
	h_puthex((const uint8_t *)m->serial, strlen(m->serial));
	h_putu(m->sku);
	h_putu(m->factory);
	put_version(m->os_version);
	put_version(m->sw_version);
	h_putx(m->state);
	h_putx(m->cv_gain);
	h_putx(m->cv_zero);
	h_putx(m->cv_pair2[0]);
	h_putx(m->cv_pair2[1]);
}

/* A request string, hex or "x" for absent, into buf (NUL-terminated). */
static const char *req_str(const struct h_cmd *c, int i, char *buf, size_t size)
{
	const char *s = h_str(c, i);
	size_t n;

	if (s && strcmp(s, "x") == 0)
		return NULL;
	n = h_hex(c, i, (uint8_t *)buf, size - 1);
	buf[n] = '\0';
	return buf;
}

static void cmd_setmeta(const struct h_cmd *c)
{
	struct s04_meta m;
	struct s04_setmeta req;
	char serial[64], sku[64], factory[64];
	size_t n;
	bool w;

	memset(&m, 0, sizeof m);
	n = h_hex(c, 3, (uint8_t *)m.serial, 8);
	m.serial[n] = '\0';
	m.sku = (uint16_t)h_num(c, 4);
	m.factory = (uint16_t)h_num(c, 5);
	req.serial = req_str(c, 6, serial, sizeof serial);
	req.sku_variant = req_str(c, 7, sku, sizeof sku);
	req.factory = req_str(c, 8, factory, sizeof factory);
	w = s04_set_metadata(&wb_device_opxy, &m, h_num(c, 1) != 0, h_num(c, 2) != 0, &req);
	h_putu(w);
	h_puthex((const uint8_t *)m.serial, strlen(m.serial));
	h_putu(m.sku);
	h_putu(m.factory);
}

static void cmd_metabuild(const struct h_cmd *c)
{
	uint8_t before[S04_META_SIZE], out[S04_META_SIZE];
	struct s04_meta m;
	size_t n;

	memset(&m, 0, sizeof m);
	if (h_hex(c, 1, before, sizeof before) != sizeof before) {
		h_error(c, "record size");
		return;
	}
	n = h_hex(c, 2, (uint8_t *)m.serial, 8);
	m.serial[n] = '\0';
	m.sku = (uint16_t)h_num(c, 3);
	m.factory = (uint16_t)h_num(c, 4);
	if (!version(c, 5, m.os_version) || !version(c, 6, m.sw_version))
		return;
	m.state = (uint32_t)h_num(c, 7);
	/* Values the write must not store (S-04.043 keeps those bytes). */
	m.cv_gain = 0x13579BDFu;
	m.cv_zero = 0x2468u;
	m.cv_pair2[0] = 0x1122u;
	m.cv_pair2[1] = 0x3344u;
	s04_meta_build(&wb_device_opxy, before, &m, h_num(c, 8) != 0, out);
	h_puthex(out, sizeof out);
}

/* --- the commands --- */

static void command(const struct h_cmd *c)
{
	const char *k = c->tok[0];
	uint8_t buf[256];
	size_t n;

	if (strcmp(k, "consume") == 0 || strcmp(k, "write") == 0) {
		uint8_t rec[S04_REBOOT_SIZE];

		if (h_hex(c, 1, rec, sizeof rec) != sizeof rec) {
			h_error(c, "record size");
			return;
		}
		if (k[0] == 'c') {
			h_putu(s04_reboot_consume(rec));
		} else {
			s04_reboot_write(rec, (uint16_t)h_num(c, 2));
		}
		h_puthex(rec, sizeof rec);
	} else if (strcmp(k, "effect") == 0) {
		struct s04_reason_effect e;

		s04_reason_effect((uint16_t)h_num(c, 1), &e);
		h_putu(e.boot_os);
		h_put(e.screen == S04_SCREEN_UPGRADE ? "upgrade" : "main");
		h_putu(e.cdc);
		h_put(e.transport == S04_TRANSPORT_MSC ? "msc" :
		      e.transport == S04_TRANSPORT_MIDI ? "midi" : "-");
	} else if (strcmp(k, "hostmode") == 0) {
		n = h_hex(c, 1, buf, sizeof buf);
		h_putu(s04_host_mode_reason(buf, n));
	} else if (strcmp(k, "nordic") == 0) {
		h_put(nordic_name(s04_nordic_step((uint16_t)h_num(c, 1))));
	} else if (strcmp(k, "writer") == 0) {
		struct s04_writer_rule r;

		s04_writer_rule((enum s04_writer)h_num(c, 1), (uint16_t)h_num(c, 2), &r);
		h_putu(r.reason);
		h_put(r.reset == S04_RESET_SOFT ? "soft" : "hard");
		h_put(nordic_name(r.nordic));
		h_putu(r.shutdown);
		h_putu(r.psoc_reset_request);
		h_putu(r.min_delay_ms);
	} else if (strcmp(k, "reboot") == 0) {
		s04_reboot(&reboot_ops, (enum s04_writer)h_num(c, 1), (uint16_t)h_num(c, 2));
	} else if (strcmp(k, "cdc") == 0) {
		struct s04_cdc_scan s;
		char name[256];
		int i;

		s04_cdc_scan_init(&s);
		for (i = 1; i < c->n; i++) {
			char *colon = strchr(c->tok[i], ':');
			struct h_cmd one;

			if (!colon) {
				h_error(c, "entry");
				return;
			}
			*colon = '\0';
			one.n = 1;
			one.tok[0] = c->tok[i];
			n = h_hex(&one, 0, (uint8_t *)name, sizeof name - 1);
			name[n] = '\0';
			s04_cdc_scan_entry(&s, name, colon[1] == '1');
		}
		h_putu(s04_cdc_scan_result(&s));
	} else if (strcmp(k, "metaread") == 0) {
		uint8_t raw[S04_META_SIZE];
		struct s04_meta m;

		if (h_hex(c, 1, raw, sizeof raw) != sizeof raw) {
			h_error(c, "record size");
			return;
		}
		s04_meta_read(&wb_device_opxy, raw, &m);
		put_meta(&m);
	} else if (strcmp(k, "metabuild") == 0) {
		cmd_metabuild(c);
	} else if (strcmp(k, "m6") == 0) {
		uint16_t v[4];

		if (version(c, 1, v))
			h_putu(s04_m6_needed(v));
	} else if (strcmp(k, "setmeta") == 0) {
		cmd_setmeta(c);
	} else if (strcmp(k, "rtcenc") == 0) {
		uint32_t clk;

		if (s04_rtc_encode((unsigned)h_num(c, 1), (unsigned)h_num(c, 2),
				   (unsigned)h_num(c, 3), (unsigned)h_num(c, 4),
				   (unsigned)h_num(c, 5), (unsigned)h_num(c, 6), &clk)) {
			h_put("ok");
			h_putx(clk);
		} else {
			h_put("refused");
		}
	} else if (strcmp(k, "rtcdec") == 0) {
		struct s04_datetime d;

		s04_rtc_decode((uint32_t)h_num(c, 1), &d);
		h_putu(d.year);
		h_putu(d.month);
		h_putu(d.day);
		h_putu(d.hour);
		h_putu(d.minute);
		h_putu(d.second);
	} else {
		h_error(c, "unknown command");
		return;
	}
	h_end();
}

int main(void)
{
	struct h_cmd c;

	while (h_next(&c))
		command(&c);
	return h_status();
}

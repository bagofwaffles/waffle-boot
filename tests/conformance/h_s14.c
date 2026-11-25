/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for the S-14 logging logic (src/core/log/line.c, log/log.c,
 * log/export.c).
 *
 * Commands (one result line each):
 *   line TS NAME LINE_NO TEXT      -> s14_line_build; NAME hex, or x for no tag
 *   taglen BODY                    -> s14_tag_len
 *   namevalid NAME                 -> s14_tag_name_valid (NAME hex)
 * A log with a clock, PSoC and CDC sinks and a screen, each logged:
 *   log CORE                       -> ok (0 master, 1 slave)
 *   at US                          -> ok: the clock
 *   screen                         -> ok: s14_log_screen_start
 *   threshold N                    -> ok: sets the field
 *   cfg HEX                        -> THRESHOLD PSOC_ON CDC_ON after s14_log_config
 *   passes LEVEL                   -> 1|0
 *   emit LEVEL NAME LINE_NO TEXT   -> what the sinks got: psoc:ID:HEX cdc:ID:HEX screen:HEX
 *   relay SRC ID HEX               -> what the sinks got, then ret:0|1
 * Sink output is "SINK:ROUTE:ID:HEX" for links and "screen:HEX".
 *   export E... [/ FAIL...]        -> copy:NAME and discard:NAME in call order, then ret:N;
 *                                     E is NAME:REG:MTIME:SIZE with NAME hex
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hlib.h"
#include "waffle/log/export.h"
#include "waffle/log/log.h"

static uint8_t buf[H_MAXLINE / 2];

/* Field i as a NUL-terminated name, or NULL for "x". */
static const char *name_arg(const struct h_cmd *c, int i, char *out, size_t max)
{
	size_t n;

	if (strcmp(h_str(c, i), "x") == 0)
		return NULL;
	n = h_hex(c, i, (uint8_t *)out, max - 1);
	out[n] = '\0';
	return out;
}

/* --- the log --- */

static uint32_t clock_us;

static uint32_t c_now(void *ctx)
{
	(void)ctx;
	return clock_us;
}

/* cppcheck-suppress constParameterCallback ; s14_link_ops fixes the type */
static bool l_send(void *ctx, uint8_t route, uint8_t id, const uint8_t *payload, size_t len)
{
	char head[32];

	snprintf(head, sizeof head, "%s:0x%x:0x%x:", (const char *)ctx, route, id);
	h_put(head);
	if (len == 0)
		putchar('-');
	for (size_t i = 0; i < len; i++)
		printf("%02x", payload[i]);
	return true;
}

static void s_show(void *ctx, const uint8_t *body, size_t len)
{
	(void)ctx;
	h_put("screen:");
	if (len == 0)
		putchar('-');
	for (size_t i = 0; i < len; i++)
		printf("%02x", body[i]);
}

static char psoc_name[] = "psoc", cdc_name[] = "cdc";
static const struct s14_clock_ops clk = { c_now, NULL };
static const struct s14_link_ops psoc = { l_send, psoc_name };
static const struct s14_link_ops cdc = { l_send, cdc_name };
static const struct s14_screen_ops scr = { s_show, NULL };
static struct s14_log lg;

/* --- the export --- */

#define MAX_ENTRIES 64

struct ex {
	unsigned n, pos, nfail;
	char name[MAX_ENTRIES][S14_EXPORT_NAME_MAX + 1];
	bool regular[MAX_ENTRIES];
	uint32_t mtime[MAX_ENTRIES], size[MAX_ENTRIES];
	char fail[MAX_ENTRIES][S14_EXPORT_NAME_MAX + 1];
};

static struct ex ex;

static void e_rewind(void *ctx)
{
	struct ex *e = ctx;

	e->pos = 0;
}

static bool e_next(void *ctx, struct s14_export_entry *out)
{
	struct ex *e = ctx;

	if (e->pos >= e->n)
		return false;
	out->name = e->name[e->pos];
	out->regular = e->regular[e->pos];
	out->mtime = e->mtime[e->pos];
	out->size = e->size[e->pos];
	e->pos++;
	return true;
}

static void put_name(const char *what, const char *name)
{
	h_put(what);
	for (const char *p = name; *p; p++)
		printf("%02x", (unsigned char)*p);
}

/* cppcheck-suppress constParameterCallback ; s14_export_ops fixes the type */
static bool e_copy(void *ctx, const char *name, uint32_t mtime)
{
	const struct ex *e = ctx;

	(void)mtime;
	put_name("copy:", name);
	for (unsigned i = 0; i < e->nfail; i++)
		if (strcmp(e->fail[i], name) == 0)
			return false;
	return true;
}

/* cppcheck-suppress constParameterCallback ; s14_export_ops fixes the type */
static void e_discard(void *ctx, const char *name)
{
	(void)ctx;
	put_name("discard:", name);
}

static void do_export(const struct h_cmd *c)
{
	static const struct s14_export_ops ops = { e_rewind, e_next, e_copy, e_discard, &ex };
	bool fails = false;
	char field[H_MAXLINE];

	memset(&ex, 0, sizeof ex);
	for (int i = 1; i < c->n; i++) {
		const char *t = c->tok[i];
		unsigned reg = 0;
		unsigned long mtime = 0, size = 0;
		char *name;
		size_t n;

		if (strcmp(t, "/") == 0) {
			fails = true;
			continue;
		}
		if (ex.n >= MAX_ENTRIES || ex.nfail >= MAX_ENTRIES) {
			h_error(c, "entries");
			return;
		}
		name = fails ? ex.fail[ex.nfail] : ex.name[ex.n];
		snprintf(field, sizeof field, "%s", t);
		if (!fails) {
			char *colon = strchr(field, ':');

			if (colon == NULL || sscanf(colon + 1, "%u:%lu:%lu", &reg, &mtime, &size) != 3) {
				h_error(c, "entry");
				return;
			}
			*colon = '\0';
		}
		n = 0;
		if (strcmp(field, "-") != 0)
			for (; field[2 * n] && field[2 * n + 1] && n < S14_EXPORT_NAME_MAX; n++) {
				unsigned v;

				if (sscanf(field + 2 * n, "%2x", &v) != 1)
					break;
				name[n] = (char)v;
			}
		name[n] = '\0';
		if (fails) {
			ex.nfail++;
		} else {
			ex.regular[ex.n] = reg != 0;
			ex.mtime[ex.n] = (uint32_t)mtime;
			ex.size[ex.n] = (uint32_t)size;
			ex.n++;
		}
	}
	h_put("calls");
	snprintf(field, sizeof field, "ret:%lu", (unsigned long)s14_export_run(&ops));
	h_put(field);
	h_end();
}

int main(void)
{
	struct h_cmd c;
	uint8_t line[S14_LINE_MAX];
	char name[S14_NAME_MAX + 8];

	while (h_next(&c)) {
		const char *k = c.tok[0];
		size_t n;

		if ((strcmp(k, "line") == 0 || strcmp(k, "emit") == 0) && c.n == 5) {
			struct s14_tag tag;

			tag.name = name_arg(&c, 2, name, sizeof name);
			tag.line_no = (uint32_t)h_num(&c, 3);
			n = h_hex(&c, 4, buf, sizeof buf);
			if (k[0] == 'l') {
				size_t w = s14_line_build(line, (uint32_t)h_num(&c, 1), tag, buf, n);

				h_puthex(line, w);
			} else {
				h_put("sinks");
				s14_log_emit(&lg, (enum s14_level)h_num(&c, 1), tag, buf, n);
			}
			h_end();
		} else if (strcmp(k, "taglen") == 0 && c.n == 2) {
			n = h_hex(&c, 1, buf, sizeof buf);
			h_putu(s14_tag_len(buf, n));
			h_end();
		} else if (strcmp(k, "namevalid") == 0 && c.n == 2) {
			n = h_hex(&c, 1, buf, sizeof buf - 1);
			buf[n] = 0;
			h_putu(s14_tag_name_valid((const char *)buf));
			h_end();
		} else if (strcmp(k, "log") == 0 && c.n == 2) {
			s14_log_init(&lg, h_num(&c, 1) ? S14_SLAVE : S14_MASTER, &clk, &psoc, &cdc, &scr);
			h_put("ok");
			h_end();
		} else if (strcmp(k, "at") == 0 && c.n == 2) {
			clock_us = (uint32_t)h_num(&c, 1);
			h_put("ok");
			h_end();
		} else if (strcmp(k, "screen") == 0) {
			s14_log_screen_start(&lg);
			h_put("ok");
			h_end();
		} else if (strcmp(k, "threshold") == 0 && c.n == 2) {
			lg.threshold = (uint8_t)h_num(&c, 1);
			h_put("ok");
			h_end();
		} else if (strcmp(k, "cfg") == 0 && c.n == 2) {
			n = h_hex(&c, 1, buf, sizeof buf);
			s14_log_config(&lg, buf, n);
			h_putu(lg.threshold);
			h_putu(lg.psoc_on);
			h_putu(lg.cdc_on);
			h_end();
		} else if (strcmp(k, "passes") == 0 && c.n == 2) {
			h_putu(s14_log_passes(&lg, (enum s14_level)h_num(&c, 1)));
			h_end();
		} else if (strcmp(k, "relay") == 0 && c.n == 4) {
			bool r;

			n = h_hex(&c, 3, buf, sizeof buf);
			h_put("sinks");
			r = s14_log_relay(&lg, (uint8_t)h_num(&c, 1), (uint8_t)h_num(&c, 2), buf, n);
			h_put(r ? "ret:1" : "ret:0");
			h_end();
		} else if (strcmp(k, "export") == 0) {
			do_export(&c);
		} else {
			h_error(&c, "unknown command");
		}
	}
	return h_status();
}

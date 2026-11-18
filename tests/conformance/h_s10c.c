/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for S-10c's codecs and texts (src/s10c_sysex.c, s10c_text.c).
 * Bytes are hex ("-": empty).
 *
 * Commands (one result line each):
 *   pack DATA            -> the packed bytes (s10c_pack7)
 *   unpack PACKED        -> the data (s10c_unpack7)
 *   send MSG             -> the event packets, concatenated (s10c_usbmidi_send)
 *   recv PACKETS         -> each complete message, a fresh receiver (s10c_midi_rx_packet), or "-"
 *   classify MSG         -> ignored | te | identity | carrier (s10c_classify)
 *   carrier STREAM       -> the carrier message (s10c_carrier_build)
 *   percent DONE TOTAL   -> s10c_percent
 *   tecode WORD          -> the TE code (s10c_te_code)
 *   progress DONE TOTAL LABEL -> the S-06 report's payload (s10c_progress_s06)
 *   progress_te DONE TOTAL OUTCOME LABEL -> the TE reply's payload (s10c_progress_te); OUTCOME 1
 *                        for S-10c.106's outcome report
 *   label STEP           -> the step's label (s10c_step_label), or "-"
 *   skiplabel STEP WHY   -> the label after a step's Skipped (s10c_skip_label); WHY option_off |
 *                        not_confirmed | switched_off
 *
 * The command server (src/s10c_host.c), one device across commands:
 *   dev HELD SLAVE BUSY  -> ok: a fresh server (s10c_host_init) on a device
 *                        in held mode or not (S-10c.045: not held, the
 *                        continue op is NULL), the slave present or not
 *                        (S-09.008), a scan, run or OS write under way or not
 *   frame LINK ROUTE ID PAYLOAD -> what the server does with one delivered
 *                        frame (s10c_host_frame), in order: each frame sent,
 *                        LINK:ROUTE:ID:PAYLOAD (hex), and "restart:LINK:W" when
 *                        Continue to the OS asks the board to restart, W the
 *                        writer the board takes (W11, or W2 without the slave,
 *                        S-04.020); "-" for nothing. LINK: 0 CDC, 1 MIDI, 2 the
 *                        PSoC's, 3 the Nordic's
 * The device's other ops are stubs: the CDC port open, the option off, a
 * transfer's storage that takes every byte.
 */
#include <stdio.h>
#include <string.h>

#include "hlib.h"
#include "waffle/s10c_host.h"
#include "waffle/s10c_sysex.h"

static uint8_t in[H_MAXLINE / 2];
static uint8_t out[H_MAXLINE / 2 + H_MAXLINE / 14 + 16];
static size_t out_len;
static struct s10c_midi_rx rx;

/* cppcheck-suppress constParameterCallback ; s10c_packet_fn fixes the type */
static void collect(void *ctx, const uint8_t packet[4])
{
	(void)ctx;
	if (out_len + 4 <= sizeof out) {
		memcpy(out + out_len, packet, 4);
		out_len += 4;
	}
}

static void put_text(const char *s, size_t n)
{
	char t[S10C_TEXT_MAX + 1];

	if (n == 0 || n >= sizeof t) {
		h_put("-");
		return;
	}
	memcpy(t, s, n);
	t[n] = '\0';
	h_puthex((const uint8_t *)t, n);
}

/* ---- the command server's device ---- */

static struct {
	bool held, slave, busy;
	unsigned out;		/* fields printed for the current frame */
} dv;

static struct s10c_host host;

static void d_send_frame(void *ctx, enum s10c_link link, uint8_t route,
			 uint8_t id, const uint8_t *payload, size_t len)
{
	char s[16 + 2 * 1024];
	int k;
	size_t i;

	(void)ctx;
	k = snprintf(s, sizeof s, "%u:%02x:%02x:%s", (unsigned)link, route, id, len ? "" : "-");
	for (i = 0; i < len && k > 0 && (size_t)k + 3u < sizeof s; i++)
		k += snprintf(s + k, sizeof s - (size_t)k, "%02x", payload[i]);
	dv.out++;
	h_put(s);
}

static void d_note(const char *s)
{
	dv.out++;
	h_put(s);
}

static void d_send_sysex(void *ctx, const uint8_t *msg, size_t n)
{
	(void)ctx;
	(void)msg;
	(void)n;
	d_note("sysex");
}

static bool d_true(void *ctx)
{
	(void)ctx;
	return true;
}

static bool d_false(void *ctx)
{
	(void)ctx;
	return false;
}

static void d_none(void *ctx)
{
	(void)ctx;
}

static void d_rx_feed(void *ctx, const uint8_t *stream, size_t n)
{
	(void)ctx;
	(void)stream;
	(void)n;
}

static void d_read_info(void *ctx, struct s10c_info *v)
{
	(void)ctx;
	memset(v, 0, sizeof *v);
}

static bool d_write_metadata(void *ctx, const struct s10c_setmeta *m)
{
	(void)ctx;
	(void)m;
	return false;
}

static bool d_read_fsn(void *ctx, uint32_t words[4])
{
	(void)ctx;
	memset(words, 0, 4 * sizeof words[0]);
	return false;
}

static void d_payload(void *ctx, const uint8_t *payload, size_t len)
{
	(void)ctx;
	(void)payload;
	(void)len;
}

static void d_log_relay(void *ctx, uint8_t id, const uint8_t *payload, size_t len)
{
	(void)ctx;
	(void)id;
	(void)payload;
	(void)len;
}

static void d_reboot_os(void *ctx)
{
	(void)ctx;
	d_note("reboot");
}

static void d_reboot_mode(void *ctx, bool has_mode, uint8_t mode)
{
	(void)ctx;
	(void)has_mode;
	(void)mode;
	d_note("reboot");
}

static void d_screen(void *ctx, uint32_t total)
{
	(void)ctx;
	(void)total;
}

static bool d_append(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	(void)p;
	(void)n;
	return true;
}

static void d_submit(void *ctx, enum s10c_source source, struct s11_outcome *o)
{
	(void)ctx;
	(void)source;
	memset(o, 0, sizeof *o);
}

/* S-10c.076: what the board's menu answers (s15_menu_continue_check()). */
static enum s10c_continue d_continue_check(void *ctx)
{
	(void)ctx;
	return dv.busy ? S10C_CONT_BUSY : S10C_CONT_GO;
}

/* The board's restart (env_continue_os(): W11, or W2 without the slave). */
static void d_continue_os(void *ctx, enum s10c_link link)
{
	char s[24];

	(void)ctx;
	snprintf(s, sizeof s, "restart:%u:%s", (unsigned)link, dv.slave ? "W11" : "W2");
	d_note(s);
}

static struct s10c_ops dev_ops = {
	.send_frame = d_send_frame,
	.send_sysex = d_send_sysex,
	.cdc_open = d_true,
	.cdc_rx_restart = d_none,
	.midi_rx_restart = d_none,
	.midi_rx_feed = d_rx_feed,
	.option_on = d_false,
	.read_info = d_read_info,
	.write_metadata = d_write_metadata,
	.read_fsn = d_read_fsn,
	.log_config = d_payload,
	.log_relay = d_log_relay,
	.reboot_os = d_reboot_os,
	.reboot_mode = d_reboot_mode,
	.reboot_after_dfu = d_none,
	.upgrade_screen = d_screen,
	.transfer_start = d_true,
	.transfer_append = d_append,
	.transfer_discard = d_none,
	.transfer_submit = d_submit,
	.ramdisk_offer = d_none,
	.continue_check = d_continue_check,
	.continue_os = d_continue_os,
};

static void serve(const struct h_cmd *c)
{
	uint8_t p[H_MAXLINE / 2];
	size_t n;

	if (c->n != 5 || h_num(c, 1) > (unsigned long)S10C_LINK_NORDIC) {
		h_error(c, "usage: frame LINK ROUTE ID PAYLOAD");
		return;
	}
	n = h_hex(c, 4, p, sizeof p);
	dv.out = 0;
	(void)s10c_host_frame(&host, (enum s10c_link)h_num(c, 1), (uint8_t)h_num(c, 2),
			      (uint8_t)h_num(c, 3), p, n);
	if (dv.out == 0)
		h_put("-");
	h_end();
}

int main(void)
{
	struct h_cmd c;
	static const char *const class_name[] = { "ignored", "te", "identity", "carrier" };

	while (h_next(&c)) {
		const char *k = c.tok[0];

		if (strcmp(k, "pack") == 0 && c.n == 2) {
			size_t n = h_hex(&c, 1, in, sizeof in);

			h_puthex(out, s10c_pack7(in, n, out));
			h_end();
		} else if (strcmp(k, "unpack") == 0 && c.n == 2) {
			size_t n = h_hex(&c, 1, in, sizeof in);

			h_puthex(out, s10c_unpack7(in, n, out));
			h_end();
		} else if (strcmp(k, "send") == 0 && c.n == 2) {
			size_t n = h_hex(&c, 1, in, sizeof in);

			out_len = 0;
			s10c_usbmidi_send(in, n, collect, NULL);
			h_puthex(out, out_len);
			h_end();
		} else if (strcmp(k, "recv") == 0 && c.n == 2) {
			size_t n = h_hex(&c, 1, in, sizeof in);
			bool any = false;

			s10c_midi_rx_init(&rx);
			for (size_t i = 0; i + 4 <= n; i += 4) {
				size_t m = s10c_midi_rx_packet(&rx, in + i);

				if (m) {
					h_puthex(rx.buf, m);
					any = true;
				}
			}
			if (!any)
				h_put("-");
			h_end();
		} else if (strcmp(k, "classify") == 0 && c.n == 2) {
			size_t n = h_hex(&c, 1, in, sizeof in);

			h_put(class_name[s10c_classify(in, n)]);
			h_end();
		} else if (strcmp(k, "carrier") == 0 && c.n == 2) {
			size_t n = h_hex(&c, 1, in, sizeof in);

			if (n > S10C_CARRIER_MAX_STREAM) {
				h_error(&c, "stream too long");
				continue;
			}
			h_puthex(out, s10c_carrier_build(in, n, out));
			h_end();
		} else if (strcmp(k, "percent") == 0 && c.n == 3) {
			h_putu(s10c_percent((uint32_t)h_num(&c, 1), (uint32_t)h_num(&c, 2)));
			h_end();
		} else if (strcmp(k, "tecode") == 0 && c.n == 2) {
			char code[10];

			s10c_te_code((uint32_t)h_num(&c, 1), code);
			put_text(code, sizeof code);
			h_end();
		} else if (strcmp(k, "progress") == 0 && c.n == 4) {
			char label[S10C_TEXT_MAX + 1];
			size_t n = h_hex(&c, 3, (uint8_t *)label, S10C_TEXT_MAX);

			label[n] = '\0';
			h_puthex(out, s10c_progress_s06((uint32_t)h_num(&c, 1), (uint32_t)h_num(&c, 2), label, out));
			h_end();
		} else if (strcmp(k, "progress_te") == 0 && c.n == 5) {
			char label[S10C_TEXT_MAX + 1];
			size_t n = h_hex(&c, 4, (uint8_t *)label, S10C_TEXT_MAX);
			const uint32_t done = (uint32_t)h_num(&c, 1), total = (uint32_t)h_num(&c, 2);

			label[n] = '\0';
			n = s10c_progress_te(done, total, h_num(&c, 3) != 0, label, out);
			h_puthex(out, n);
			h_end();
		} else if (strcmp(k, "label") == 0 && c.n == 2) {
			const char *s = s10c_step_label((unsigned)h_num(&c, 1));

			put_text(s, strlen(s));
			h_end();
		} else if (strcmp(k, "skiplabel") == 0 && c.n == 3) {
			const char *w = c.tok[2];
			char label[S10C_SKIP_LABEL_MAX + 1];
			size_t n = s10c_skip_label((unsigned)h_num(&c, 1),
				strcmp(w, "option_off") == 0 ? S11_SKIP_OPTION_OFF :
				strcmp(w, "not_confirmed") == 0 ? S11_SKIP_NOT_CONFIRMED :
				strcmp(w, "already_written") == 0 ?
				S11_SKIP_ALREADY_WRITTEN : S11_SKIP_SWITCHED_OFF, label);

			put_text(label, n);
			h_end();
		} else if (strcmp(k, "dev") == 0 && c.n == 4) {
			dv.held = h_num(&c, 1) != 0;
			dv.slave = h_num(&c, 2) != 0;
			dv.busy = h_num(&c, 3) != 0;
			dev_ops.continue_check = dv.held ? d_continue_check : NULL;
			s10c_host_init(&host, &dev_ops, NULL);
			h_put("ok");
			h_end();
		} else if (strcmp(k, "frame") == 0) {
			serve(&c);
		} else {
			h_error(&c, "unknown command");
		}
	}
	return h_status();
}

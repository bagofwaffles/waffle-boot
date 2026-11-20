/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10c host commands. Checks S-10c.001, S-10c.002, S-10c.003, S-10c.004,
 * S-10c.005, S-10c.006, S-10c.007, S-10c.010, S-10c.011, S-10c.020,
 * S-10c.021, S-10c.022, S-10c.031, S-10c.032, S-10c.033, S-10c.040,
 * S-10c.041, S-10c.042, S-10c.050, S-10c.051, S-10c.052, S-10c.053,
 * S-10c.054, S-10c.055, S-10c.056, S-10c.057, S-10c.059, S-10c.060,
 * S-10c.061, S-10c.070, S-10c.071, S-10c.072, S-10c.073,
 * S-10c.074, S-10c.075, S-10c.076, S-10c.080, S-10c.081, S-10c.082, S-10c.083,
 * S-10c.084, S-10c.085, S-10c.086, S-10c.087, S-10c.088, S-10c.089,
 * S-10c.090, S-10c.091, S-10c.092, S-10c.100, S-10c.101, S-10c.102,
 * S-10c.103, S-10c.104, S-10c.105, S-10c.106, S-10c.107; and S-11.050,
 * S-11.051 from the host side (forcing by source, the reported outcome);
 * S-17.033 (the info text carries no coprocessor value).
 */
#include <stdint.h>
#include <string.h>

#include "waffle/s10c_host.h"
#include "waffle/s10c_sysex.h"

#include "s11_check.h"
#include "vectors/s10c_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define B(r) (s10cv_blob + (r).off)

static int bytes_eq(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
	return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

/* The next entry of a u16be length-prefixed list. */
static int list_next(const uint8_t **p, const uint8_t *end,
		     const uint8_t **item, size_t *len)
{
	if (*p >= end)
		return 0;
	*len = (size_t)((*p)[0] << 8 | (*p)[1]);
	*item = *p + 2;
	*p += 2 + *len;
	return 1;
}

/* ---- the fake device behind the ops ---- */

struct frame {
	enum s10c_link link;
	uint8_t route, id;
	size_t len;
	uint8_t p[256];
};

struct msg {
	size_t len;
	uint8_t m[S10C_SYSEX_MAX_SENT];
};

struct fake {
	struct s10c_host host;
	bool option, fsn_ok, cdc_open;
	char serial[8];
	uint32_t sku, factory;
	struct frame frames[64];
	size_t nframes, next_frame;
	struct msg msgs[64];
	size_t nmsgs;
	unsigned log_configs, log_relays, reboots_os, reboots_mode, reboots_dfu,
		 offers, starts, discards, submits;
	bool mode_given;
	uint8_t mode;
	size_t appended;
	bool append_ok, start_ok;
	enum s10c_source source;
	struct s11_outcome outcome;
	unsigned restarts, cdc_restarts;
	uint8_t fed[1024];
	size_t nfed;
	/* order of events: 'P' progress, 'R' final reply */
	char order[16];
	size_t norder;
	/* the upgrade screen: opened, refusals, screens open at the last */
	unsigned screens, refusals, screens_at_refusal;
	enum s11_reason refused_reason;
	struct msg last_progress;	/* the last TE progress reply */
	/* the S-06 progress reports: how many, and where the last went */
	unsigned reports;
	enum s10c_link report_link;
	unsigned report_route;
	/* S-10c.076: the board's answer, and the sequence once started */
	enum s10c_continue cont;
	unsigned continued;
	enum s10c_link cont_link;
	size_t frames_at_cont;		/* the reply was out before it */
};

static struct fake F;

static void f_send_frame(void *ctx, enum s10c_link link, uint8_t route,
			 uint8_t id, const uint8_t *p, size_t n)
{
	struct fake *f = ctx;
	struct frame *fr;

	if (id == 0x0E && n > 0 && f->norder < sizeof(f->order))
		f->order[f->norder++] = p[0] == 0x92 ? 'P' : 'R';
	if (id == 0x0E && n > 0 && p[0] == 0x92) {
		f->reports++;
		f->report_link = link;
		f->report_route = route;
	}
	/* the runner ignores log lines and progress reports */
	if (id == 0x03 || id == 0x04 || (id == 0x0E && n > 0 && p[0] == 0x92))
		return;
	if (f->nframes == N(f->frames) || n > sizeof(fr->p))
		return;
	fr = &f->frames[f->nframes++];
	fr->link = link;
	fr->route = route;
	fr->id = id;
	fr->len = n;
	memcpy(fr->p, p, n);
}

static void f_send_sysex(void *ctx, const uint8_t *m, size_t n)
{
	struct fake *f = ctx;

	CHECK(n <= S10C_SYSEX_MAX_SENT, "S-10c.005: a message of %zu bytes", n);
	if (n >= 10 && m[9] == 0x40 && (m[6] & 0x40) == 0) {
		if (f->norder < sizeof(f->order))
			f->order[f->norder++] = 'P';
		f->last_progress.len = n;
		memcpy(f->last_progress.m, m, n);
		return;	/* a progress reply: left out by the runner */
	}
	if (n >= 10 && f->norder < sizeof(f->order))
		f->order[f->norder++] = 'R';
	if (f->nmsgs == N(f->msgs) || n > S10C_SYSEX_MAX_SENT)
		return;
	f->msgs[f->nmsgs].len = n;
	memcpy(f->msgs[f->nmsgs++].m, m, n);
}

static bool f_cdc_open(void *ctx)
{
	return ((struct fake *)ctx)->cdc_open;
}

static void f_cdc_restart(void *ctx)
{
	((struct fake *)ctx)->cdc_restarts++;
}

static void f_rx_restart(void *ctx)
{
	((struct fake *)ctx)->restarts++;
}

static void f_rx_feed(void *ctx, const uint8_t *p, size_t n)
{
	struct fake *f = ctx;

	if (f->nfed + n <= sizeof(f->fed)) {
		memcpy(f->fed + f->nfed, p, n);
		f->nfed += n;
	}
}

static bool f_option(void *ctx)
{
	return ((struct fake *)ctx)->option;
}

static void f_read_info(void *ctx, struct s10c_info *v)
{
	static const char dsp[] = "ZZ0123456789ABCDEF";
	const struct fake *f = &F;	/* the ops signature fixes ctx */

	(void)ctx;
	memset(v, 0, sizeof(*v));
	v->serial_len = 8;
	memcpy(v->serial, f->serial, 8);
	v->dsp_ok = true;
	memcpy(v->dsp_otp, dsp, 18);
	v->hw_rev = 255;
	v->sku = f->sku;
	v->sw = (struct s11_version){ 1, 2, 3, 0 };
	v->bl = (struct s11_version){ 1, 0, 0, 0 };
	v->os = (struct s11_version){ 1, 2, 3, 45 };
	v->factory = f->factory;
}

/* S-04.046 as the S-10c model applies it */
static bool f_write_metadata(void *ctx, const struct s10c_setmeta *m)
{
	struct fake *f = ctx;

	if (m->has_serial) {
		if (m->serial_len != 8)
			return false;
		memcpy(f->serial, m->serial, 8);
	}
	if (m->has_sku)
		f->sku = m->sku <= 999 ? m->sku : 0;
	if (m->has_factory)
		f->factory = m->factory & 0xFFFFu;
	return true;
}

/* commands.json's device: the FSN words in OTP */
static bool f_read_fsn(void *ctx, uint32_t words[4])
{
	memcpy(words, s10cv_fsn_words, sizeof(s10cv_fsn_words));
	return ((struct fake *)ctx)->fsn_ok;
}

static void f_log_config(void *ctx, const uint8_t *p, size_t n)
{
	(void)p;
	(void)n;
	((struct fake *)ctx)->log_configs++;
}

static void f_log_relay(void *ctx, uint8_t id, const uint8_t *p, size_t n)
{
	(void)id;
	(void)p;
	(void)n;
	((struct fake *)ctx)->log_relays++;
}

static void f_reboot_os(void *ctx)
{
	((struct fake *)ctx)->reboots_os++;
}

static void f_reboot_mode(void *ctx, bool has, uint8_t mode)
{
	struct fake *f = ctx;

	f->reboots_mode++;
	f->mode_given = has;
	f->mode = mode;
}

static void f_reboot_dfu(void *ctx)
{
	((struct fake *)ctx)->reboots_dfu++;
}

static void f_screen(void *ctx, uint32_t total)
{
	(void)total;
	((struct fake *)ctx)->screens++;
}

static void f_refused(void *ctx, enum s11_reason reason)
{
	struct fake *f = ctx;

	f->refusals++;
	f->screens_at_refusal = f->screens;
	f->refused_reason = reason;
}

static bool f_start(void *ctx)
{
	struct fake *f = ctx;

	f->starts++;
	f->appended = 0;
	return f->start_ok;
}

static bool f_append(void *ctx, const uint8_t *p, size_t n)
{
	struct fake *f = ctx;

	(void)p;
	f->appended += n;
	return f->append_ok;
}

static void f_discard(void *ctx)
{
	((struct fake *)ctx)->discards++;
}

static void f_submit(void *ctx, enum s10c_source source, struct s11_outcome *o)
{
	struct fake *f = ctx;

	f->submits++;
	f->source = source;
	/* the package pipeline reports progress meanwhile */
	s10c_host_progress(&f->host, 1, 2, "[main dsp]");
	s10c_host_progress(&f->host, 2, 2, "[main dsp]");
	*o = f->outcome;
}

static void f_offer(void *ctx)
{
	((struct fake *)ctx)->offers++;
}

static enum s10c_continue f_cont_check(void *ctx)
{
	return ((struct fake *)ctx)->cont;
}

static void f_continue(void *ctx, enum s10c_link link)
{
	struct fake *f = ctx;

	f->continued++;
	f->cont_link = link;
	f->frames_at_cont = f->nframes;
}

static const struct s10c_ops fake_ops = {
	f_send_frame, f_send_sysex, f_cdc_open, f_cdc_restart, f_rx_restart,
	f_rx_feed,
	f_option, f_read_info, f_write_metadata, f_read_fsn, f_log_config,
	f_log_relay, f_reboot_os, f_reboot_mode, f_reboot_dfu,
	f_screen, f_refused, f_start, f_append, f_discard, f_submit, f_offer,
	f_cont_check, f_continue,
};

static void fake_boot(bool option, bool fsn_ok)
{
	memset(&F, 0, sizeof(F));
	F.option = option;
	F.fsn_ok = fsn_ok;
	F.cdc_open = true;
	memcpy(F.serial, "AB123456", 8);
	F.sku = 7;
	F.factory = 1;
	F.append_ok = F.start_ok = true;
	F.outcome.kind = S11_OUTCOME_DONE;
	s10c_host_init(&F.host, &fake_ops, &F);
}

/* ---- S-10c.001-S-10c.006 ---- */

static void test_pack(void)
{
	uint8_t out[8192];

	for (size_t i = 0; i < N(s10cv_pack); i++) {
		const struct s10cv_pair *v = &s10cv_pack[i];
		size_t n = s10c_pack7(B(v->a), v->a.len, out);

		CHECK(n == s10c_packed_len(v->a.len) &&
		      bytes_eq(out, n, B(v->b), v->b.len), "%s", v->name);
	}
	for (size_t i = 0; i < N(s10cv_unpack); i++) {
		const struct s10cv_pair *v = &s10cv_unpack[i];
		size_t n = s10c_unpack7(B(v->b), v->b.len, out);

		CHECK(n == s10c_unpacked_len(v->b.len) &&
		      bytes_eq(out, n, B(v->a), v->a.len), "%s", v->name);
	}
}

struct packets {
	uint8_t p[1024];
	size_t n;
};

static void collect_packet(void *ctx, const uint8_t pk[4])
{
	struct packets *c = ctx;

	if (c->n + 4 <= sizeof(c->p))
		memcpy(c->p + c->n, pk, 4);
	c->n += 4;
}

static void test_usbmidi(void)
{
	static struct s10c_midi_rx rx;

	for (size_t i = 0; i < N(s10cv_usb_send); i++) {
		const struct s10cv_pair *v = &s10cv_usb_send[i];
		struct packets c = { { 0 }, 0 };

		s10c_usbmidi_send(B(v->a), v->a.len, collect_packet, &c);
		CHECK(bytes_eq(c.p, c.n, B(v->b), v->b.len), "%s", v->name);
	}
	for (size_t i = 0; i < N(s10cv_usb_receive); i++) {
		const struct s10cv_pair *v = &s10cv_usb_receive[i];
		const uint8_t *want = B(v->b), *end = want + v->b.len, *item;
		size_t len, got = 0, expected = 0;
		int ok = 1;

		s10c_midi_rx_init(&rx);
		for (size_t k = 0; k + 4 <= v->a.len; k += 4) {
			size_t n = s10c_midi_rx_packet(&rx, B(v->a) + k);

			if (n == 0)
				continue;
			got++;
			if (!list_next(&want, end, &item, &len) ||
			    !bytes_eq(rx.buf, n, item, len))
				ok = 0;
		}
		for (const uint8_t *p = B(v->b); list_next(&p, end, &item, &len);)
			expected++;
		CHECK(ok && got == expected, "%s: %zu of %zu messages", v->name,
		      got, expected);
	}
}

/* ---- S-10c.007, S-10c.020-S-10c.022 ---- */

static void test_classify(void)
{
	for (size_t i = 0; i < N(s10cv_classify); i++) {
		const struct s10cv_class *v = &s10cv_classify[i];

		CHECK((int)s10c_classify(B(v->msg), v->msg.len) == v->cls, "%s",
		      v->name);
	}
}

struct msgs {
	uint8_t data[4096];
	size_t n, count;
	int too_long;
};

static void collect_msg(void *ctx, const uint8_t *m, size_t n)
{
	struct msgs *c = ctx;

	if (n > S10C_SYSEX_MAX_SENT)
		c->too_long = 1;
	if (c->n + 2 + n <= sizeof(c->data)) {
		c->data[c->n] = (uint8_t)(n >> 8);
		c->data[c->n + 1] = (uint8_t)n;
		memcpy(c->data + c->n + 2, m, n);
	}
	c->n += 2 + n;
	c->count++;
}

static void test_carrier(void)
{
	uint8_t out[S10C_SYSEX_MAX_SENT];

	for (size_t i = 0; i < N(s10cv_carrier_encode); i++) {
		const struct s10cv_pair *v = &s10cv_carrier_encode[i];
		size_t n;

		if (v->a.len > S10C_CARRIER_MAX_STREAM)
			continue;	/* longer streams go through send_frame */
		n = s10c_carrier_build(B(v->a), v->a.len, out);
		CHECK(bytes_eq(out, n, B(v->b), v->b.len), "%s", v->name);
	}
	for (size_t i = 0; i < N(s10cv_carrier_tx); i++) {
		const struct s10cv_pair *v = &s10cv_carrier_tx[i];
		static struct msgs c;

		memset(&c, 0, sizeof(c));
		s10c_carrier_send_frame(B(v->a), v->a.len, collect_msg, &c);
		CHECK(!c.too_long && bytes_eq(c.data, c.n, B(v->b), v->b.len),
		      "%s", v->name);
	}
	/* S-10c.021: each message restarts the receiver; > 512 is ignored */
	for (size_t i = 0; i < N(s10cv_carrier_rx); i++) {
		const struct s10cv_pair *v = &s10cv_carrier_rx[i];

		fake_boot(false, true);
		s10c_host_midi_message(&F.host, B(v->a), v->a.len);
		if (v->b.present)
			CHECK(F.restarts == 1 &&
			      bytes_eq(F.fed, F.nfed, B(v->b), v->b.len), "%s",
			      v->name);
		else
			CHECK(F.restarts == 0 && F.nfed == 0, "%s ignored", v->name);
	}
}

/* ---- S-10c.010, S-10c.011, S-10c.080-S-10c.091 ---- */

static void test_sysex(void)
{
	for (size_t i = 0; i < N(s10cv_sysex); i++) {
		const struct s10cv_step *v = &s10cv_sysex[i];
		const uint8_t *p = B(v->replies), *end = p + v->replies.len, *item;
		size_t len, k = 0;
		int ok = 1;

		if (v->first)
			fake_boot(false, true);
		F.nmsgs = 0;
		s10c_host_midi_message(&F.host, B(v->host), v->host.len);
		while (list_next(&p, end, &item, &len)) {
			if (k >= F.nmsgs || !bytes_eq(F.msgs[k].m, F.msgs[k].len,
						      item, len))
				ok = 0;
			k++;
		}
		CHECK(ok && k == F.nmsgs, "%s: step %zu (%zu replies, %zu wanted)",
		      v->name, i, F.nmsgs, k);
	}
}

/* ---- S-10c.040-S-10c.075 ---- */

static void test_commands(void)
{
	for (size_t i = 0; i < N(s10cv_commands); i++) {
		const struct s10cv_cmd *v = &s10cv_commands[i];
		static const enum s10c_link links[4] = {
			S10C_LINK_CDC, S10C_LINK_MIDI, S10C_LINK_PSOC,
			S10C_LINK_NORDIC,
		};
		enum s10c_link link = links[v->link];
		const struct frame *fr;

		if (v->first) {
			fake_boot(v->option, v->fsn_readable);
			/* the board's boot ops before held mode (S-10c.045) */
			F.start_ok = v->held;
			F.cont = !v->held ? S10C_CONT_NOT_HELD :
				 S10C_CONT_GO;
		}
		/* S-10c.076: the board restarts, and serves nothing */
		if (v->inject && F.continued != 0u)
			continue;
		if (v->inject) {
			s10c_host_frame(&F.host, link, (uint8_t)v->route,
					(uint8_t)v->id, B(v->payload),
					v->payload.len);
			continue;
		}
		if (F.next_frame >= F.nframes) {
			CHECK(0, "%s: no frame for id %02x", v->name, v->id);
			continue;
		}
		fr = &F.frames[F.next_frame++];
		CHECK(fr->link == link && fr->route == v->route && fr->id == v->id &&
		      (v->prefix ? fr->len >= v->payload.len &&
				   memcmp(fr->p, B(v->payload), v->payload.len) == 0 :
				   bytes_eq(fr->p, fr->len, B(v->payload),
					    v->payload.len)),
		      "%s: frame %zu: id %02x route %02x len %zu", v->name,
		      F.next_frame, fr->id, fr->route, fr->len);
	}
}

/* S-10c.041 (no reply apart from those listed), S-10c.059, S-10c.060 effects,
 * S-10c.074 reboots, S-10c.031 */
static void test_effects(void)
{
	static const uint8_t ee[2] = { 0xEE, 0x04 }, os[1] = { 0x18 };
	static const uint8_t ping[1] = { 0x55 };

	fake_boot(false, true);
	CHECK(!s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0xAC, NULL, 0),
	      "AC not served on CDC");
	CHECK(!s10c_host_frame(&F.host, S10C_LINK_MIDI, 0x21, 0xAC, NULL, 0),
	      "AC not served on MIDI");
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0xB0, NULL, 0);
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x03, NULL, 0);
	s10c_host_frame(&F.host, S10C_LINK_MIDI, 0x21, 0xB0, NULL, 0);
	s10c_host_frame(&F.host, S10C_LINK_MIDI, 0x21, 0x04, NULL, 0);
	CHECK(F.log_configs == 1 && F.log_relays == 1 && F.nframes == 0,
	      "silent commands, MIDI log commands unserved");
	s10c_host_frame(&F.host, S10C_LINK_MIDI, 0x21, 0x0D, ee, 2);
	CHECK(F.reboots_mode == 1 && F.mode_given && F.mode == 4 &&
	      F.nframes == 0, "0D EE: the mode, no reply");
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, ee, 1);
	CHECK(F.reboots_mode == 2 && !F.mode_given, "0D EE without a mode");
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, os, 1);
	CHECK(F.reboots_os == 1 && F.nframes == 0, "0D 18: no reply");
	for (unsigned id = 0; id < 256; id++) {
		size_t before = F.nframes;

		s10c_host_frame(&F.host, S10C_LINK_MIDI, 0x21, (uint8_t)id, ping, 1);
		if (id == 0x01 || id == 0x09 || id == 0x0D || id == 0x1F)
			CHECK(F.nframes == before + 1, "MIDI id %02x answered", id);
		else
			CHECK(F.nframes == before, "MIDI id %02x silent", id);
	}
	F.cdc_open = false;
	F.nframes = 0;
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x01, ping, 1);
	CHECK(F.nframes == 0, "S-10c.031: nothing sent on a closed port");
}

/* S-10c.053, S-10c.061: the option is tested at each request */
static void test_factory_gate(void)
{
	static const uint8_t unlock[4] = { 0xCA, 0xFE, 0xBA, 0xBE };
	static const uint8_t meta[] = "serial:CD654321";

	fake_boot(true, true);
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x07, unlock, 4);
	F.option = false;
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0B, meta, sizeof(meta) - 1);
	CHECK(F.nframes == 2 && F.frames[0].p[0] == 0x03 &&
	      F.frames[1].p[0] == 0x04 && memcmp(F.serial, "AB123456", 8) == 0,
	      "unlocked, then the option off: set metadata refused");
	F.option = true;
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0B, meta, sizeof(meta) - 1);
	CHECK(F.frames[2].p[0] == 0x03 && memcmp(F.serial, "CD654321", 8) == 0,
	      "the option back on: applied");
}

/* S-10c.056 parsing details */
static void test_setmeta(void)
{
	static const struct {
		const char *text;
		int serial, sku;
		uint32_t sku_v;
	} t[] = {
		{ "serial:AB;sku_variant:5", 1, 1, 5 },
		{ ";serial:CD654321", 1, 0, 0 },
		{ "a:1;;serial:CD654321", 1, 0, 0 },
		{ "x;serial:CD654321", 1, 0, 0 },
		{ "serial:A:B", 0, 0, 0 },
		{ "serial:", 0, 0, 0 },
		{ "serial:;sku_variant:-1", 1, 1, 0xFFFFFFFFu },
		{ "sku_variant:  +12x", 0, 1, 12 },
		{ "sku_variant:4294967296", 0, 1, 0xFFFFFFFFu },
		{ "sku_variant:- 5", 0, 1, 0 },
		{ "sku_variantx:3;sku_variant:4", 0, 1, 4 },
	};

	for (size_t i = 0; i < N(t); i++) {
		struct s10c_setmeta m;

		s10c_setmeta_parse((const uint8_t *)t[i].text, strlen(t[i].text), &m);
		CHECK(m.has_serial == t[i].serial && m.has_sku == t[i].sku &&
		      (!m.has_sku || m.sku == t[i].sku_v), "'%s'", t[i].text);
	}
}

/* ---- S-10c.054, S-10c.055, S-10c.088 ---- */

static void test_info(void)
{
	for (size_t i = 0; i < N(s10cv_info); i++) {
		const struct s10cv_info *v = &s10cv_info[i];
		struct s10c_info in;
		char text[S10C_INFO_TEXT_MAX];
		size_t n;

		memset(&in, 0, sizeof(in));
		in.serial_len = (uint8_t)strlen(v->serial);
		memcpy(in.serial, v->serial, in.serial_len);
		in.dsp_ok = v->dsp_ok;
		if (v->dsp_ok)
			memcpy(in.dsp_otp, B(v->dsp_otp), 18);
		in.hw_rev = (uint8_t)v->hw_rev;
		in.sku = v->sku;
		in.sw = (struct s11_version){ (uint16_t)v->sw[0], (uint16_t)v->sw[1],
					      (uint16_t)v->sw[2], (uint16_t)v->sw[3] };
		in.bl = (struct s11_version){ (uint16_t)v->bl[0], (uint16_t)v->bl[1],
					      (uint16_t)v->bl[2], (uint16_t)v->bl[3] };
		in.os = (struct s11_version){ (uint16_t)v->os[0], (uint16_t)v->os[1],
					      (uint16_t)v->os[2], (uint16_t)v->os[3] };
		in.factory = v->factory;
		n = s10c_info_text(&in, text);
		CHECK(s11_text_eq(text, n, v->text), "info %s: %.*s", v->name,
		      (int)n, text);
	}
	for (size_t i = 0; i < N(s10cv_te_code); i++) {
		const struct s10cv_te_code *v = &s10cv_te_code[i];
		char code[10];

		s10c_te_code(v->word, code);
		CHECK(s11_text_eq(code, 10, v->code) &&
		      s11_product_ok(v->word) == (v->passes != 0),
		      "TE code %08x", (unsigned)v->word);
	}
}

/* ---- S-10c.100-S-10c.107 ---- */

static void test_progress_texts(void)
{
	uint8_t out[1 + S10C_TEXT_MAX];

	for (size_t i = 0; i < N(s10cv_percent); i++) {
		const struct s10cv_percent *v = &s10cv_percent[i];

		CHECK(s10c_percent(v->done, v->total) == v->percent,
		      "percent %u/%u", (unsigned)v->done, (unsigned)v->total);
		CHECK(s10c_percent_te(v->done, v->total) == v->te_percent,
		      "TE percent %u/%u", (unsigned)v->done, (unsigned)v->total);
	}
	for (size_t i = 0; i < N(s10cv_progress_s06); i++) {
		const struct s10cv_progress *v = &s10cv_progress_s06[i];
		size_t n = s10c_progress_s06(v->done, v->total, v->label, out);

		CHECK(bytes_eq(out, n, B(v->out), v->out.len), "S-06 progress '%s'",
		      v->label);
	}
	for (size_t i = 0; i < N(s10cv_progress_te); i++) {
		const struct s10cv_progress *v = &s10cv_progress_te[i];
		struct s10c_te_req req = { 0x03, v->req_id >= 0,
					   (uint16_t)(v->req_id >= 0 ? v->req_id : 0),
					   NULL, 0, false };
		uint8_t msg[S10C_SYSEX_MAX_SENT];
		size_t n = s10c_progress_te(v->done, v->total, v->outcome != 0,
					    v->label, out);

		n = s10c_te_reply_build(&req, 0x40, out, n, msg);
		CHECK(bytes_eq(msg, n, B(v->out), v->out.len), "TE progress '%s'",
		      v->label);
	}
}

/* S-10c.104 labels; S-10c.033 the CDC write timeout; S-10c.032 */
static void test_labels_and_cdc(void)
{
	CHECK(strcmp(s10cv_labels[0], S10C_LABEL_DECRYPTING) == 0 &&
	      strcmp(s10cv_labels[1], S10C_LABEL_EXTRACTING) == 0, "file labels");
	for (unsigned k = 0; k < 6; k++)
		CHECK(strcmp(s10cv_labels[2 + k], s10c_step_label(k)) == 0,
		      "step %u label %s", k, s10c_step_label(k));
	/* S-11.057: the reserved step 6 has no label */
	CHECK(s10c_step_label(6)[0] == '\0' && s10c_step_label(7)[0] == '\0',
	      "no label from step 6 on");
	/* S-10c.106: the report after a step's Skipped */
	for (size_t i = 0; i < N(s10cv_skips); i++) {
		const struct s10cv_skip *v = &s10cv_skips[i];
		const enum s11_skip why =
			strcmp(v->reason, "option off") == 0 ? S11_SKIP_OPTION_OFF :
			strcmp(v->reason, "not confirmed") == 0 ?
						S11_SKIP_NOT_CONFIRMED :
			strcmp(v->reason, "already written") == 0 ?
						S11_SKIP_ALREADY_WRITTEN :
						S11_SKIP_SWITCHED_OFF;
		char label[S10C_SKIP_LABEL_MAX + 1];
		size_t n = s10c_skip_label(v->step, why, label);

		CHECK(n == strlen(v->label) && strcmp(label, v->label) == 0,
		      "skip label %u %s: '%s', want '%s'", v->step, v->reason,
		      label, v->label);
	}
	CHECK(s10c_write_timeout_ms(0) == 110 &&
	      s10c_write_timeout_ms(249) == 134 &&
	      s10c_write_timeout_ms(1000) == 210, "len / 10 + 110 ms");
	fake_boot(false, true);
	s10c_host_cdc_line_state(&F.host);
	CHECK(F.cdc_restarts == 1, "SET_CONTROL_LINE_STATE restarts the receiver");
}

static void test_error_texts(void)
{
	char text[S10C_TEXT_MAX];

	for (size_t i = 0; i < N(s10cv_reasons); i++) {
		struct s11_outcome o = { S11_OUTCOME_REFUSED, S11_REASON_NONE, 0,
					 S11_OK };
		size_t n, k;

		for (int r = S11_REASON_SHORT; r <= S11_REASON_STREAM; r++) {
			if (strcmp(s11_reason_name((enum s11_reason)r),
				   s10cv_reasons[i]) == 0)
				o.reason = (enum s11_reason)r;
		}
		n = s10c_error_text(&o, text);
		k = strlen(s10cv_reasons[i]);
		CHECK(o.reason != S11_REASON_NONE && n >= k &&
		      memcmp(text, s10cv_reasons[i], k) == 0, "reason %s",
		      s10cv_reasons[i]);
	}
	{
		struct s11_outcome o = { S11_OUTCOME_FAILED, S11_REASON_NONE, 3,
					 S11_NOT_FOUND };
		size_t n = s10c_error_text(&o, text);

		CHECK(s11_text_eq(text, n, "step 3 result 2"), "failed step text");
	}
}

/* S-10c.073, S-10c.090, S-10c.100-S-10c.107: progress precedes the final
 * reply, on the link and in the form of the request; the outcome report. */
static void test_finish(void)
{
	static const uint8_t begin[14] = { 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0,
					   0x00, 0x08, 0x40, 0x00 };
	static const uint8_t chunk[3] = { 0x03, 0xAA, 0xBB }, finish[1] = { 0x05 };
	/* sysex.json dfu-begin-ok, request id 1 */
	static const uint8_t te_begin[] = {
		0xF0, 0x00, 0x20, 0x76, 0x21, 0x40, 0x60, 0x01, 0x03, 0x00,
		0x02, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x08, 0x40, 0x00, 0x08, 0x00, 0x00, 0x0F, 0x20,
		0xF7 };
	static const uint8_t te_chunk[] = { 0xF0, 0x00, 0x20, 0x76, 0x21, 0x40,
					    0x60, 0x02, 0x03, 0x00, 0x03, 0x00,
					    0x55, 0xF7 };
	static const uint8_t te_finish[] = { 0xF0, 0x00, 0x20, 0x76, 0x21, 0x40,
					     0x60, 0x03, 0x03, 0x00, 0x04, 0xF7 };

	fake_boot(false, true);
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, begin, 14);
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, chunk, 3);
	F.norder = 0;
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, finish, 1);
	CHECK(F.submits == 1 && F.source == S10C_SOURCE_FW_CDC && F.appended == 2,
	      "CDC finish submits the bytes, forced");
	CHECK(F.norder == 4 && memcmp(F.order, "PPPR", 4) == 0,
	      "two reports, [done], then the reply: %.*s", (int)F.norder, F.order);
	CHECK(F.offers == 1, "RAM disk offered again");

	/* a failed run: no outcome report before the reply; text step N */
	F.outcome = (struct s11_outcome){ S11_OUTCOME_FAILED, S11_REASON_NONE, 3,
					  S11_FAILURE };
	s10c_host_frame(&F.host, S10C_LINK_MIDI, 0x21, 0x0D, begin, 14);
	F.norder = 0;
	F.nframes = 0;
	s10c_host_frame(&F.host, S10C_LINK_MIDI, 0x21, 0x0D, finish, 1);
	CHECK(F.source == S10C_SOURCE_FW_MIDI && F.norder == 3 &&
	      memcmp(F.order, "PPR", 3) == 0, "failed run on MIDI");
	CHECK(F.nframes == 1 && F.frames[0].link == S10C_LINK_MIDI &&
	      F.frames[0].len == 16 && F.frames[0].p[0] == 0x84 &&
	      memcmp(F.frames[0].p + 1, "step 3 result 1", 15) == 0,
	      "84 and the failed step");

	/* the MIDI transfer: progress as status-40 replies to the finish */
	F.outcome.kind = S11_OUTCOME_NOTHING_TO_DO;
	s10c_host_midi_message(&F.host, te_begin, sizeof(te_begin));
	s10c_host_midi_message(&F.host, te_chunk, sizeof(te_chunk));
	F.norder = 0;
	F.nmsgs = 0;
	s10c_host_midi_message(&F.host, te_finish, sizeof(te_finish));
	CHECK(F.source == S10C_SOURCE_TE && F.norder == 4 &&
	      memcmp(F.order, "PPPR", 4) == 0, "TE finish: %.*s", (int)F.norder,
	      F.order);
	CHECK(F.nmsgs == 1 && F.msgs[0].len == 11 && F.msgs[0].m[7] == 0x03 &&
	      F.msgs[0].m[9] == 0x00, "TE final reply: status 00 to id 3");
	{
		/* S-10c.102, S-10c.106: the outcome report, no counts, percent
		 * 100 (packed: the high-bit byte, then 64), its label and no 00 */
		static const uint8_t tail[] = { 'o', ']', 0xF7 };
		const struct msg *lp = &F.last_progress;

		CHECK(lp->len > 14 && lp->m[11] == 100 && lp->m[12] == '[' &&
		      memcmp(lp->m + lp->len - 3, tail, 3) == 0,
		      "TE outcome report: 100, [nothing to do], no 00");
	}

	/* after a submission, progress goes back to CDC (S-10c.100) */
	F.nframes = 0;
	s10c_host_progress(&F.host, 0, 0, "[done]");
	CHECK(F.norder == 5 && F.order[4] == 'P', "a later report goes to CDC");
}

/* S-10c.071, S-10c.088 (quirk Q-C14): a begin whose transfer cannot start
 * is refused with an error text, the transfer in progress discarded. */
static void test_begin_cannot_start(void)
{
	static const uint8_t begin[14] = { 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0,
					   0x00, 0x08, 0x40, 0x00 };
	static const uint8_t chunk[3] = { 0x03, 0xAA, 0xBB };
	/* sysex.json dfu-begin-ok, request id 1 */
	static const uint8_t te_begin[] = {
		0xF0, 0x00, 0x20, 0x76, 0x21, 0x40, 0x60, 0x01, 0x03, 0x00,
		0x02, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x08, 0x40, 0x00, 0x08, 0x00, 0x00, 0x0F, 0x20,
		0xF7 };
	static const uint8_t te_chunk[] = { 0xF0, 0x00, 0x20, 0x76, 0x21, 0x40,
					    0x60, 0x02, 0x03, 0x00, 0x03, 0x00,
					    0x55, 0xF7 };

	fake_boot(false, true);
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, begin, 14);
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, chunk, 3);
	F.start_ok = false;
	F.nframes = 0;
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, begin, 14);
	CHECK(F.discards == 1 && F.starts == 2, "CDC: the transfer discarded");
	CHECK(F.nframes == 1 && F.frames[0].id == 0x0E && F.frames[0].len > 1 &&
	      F.frames[0].p[0] == 0x84, "CDC: 84 and an error text (S-10c.071)");

	fake_boot(false, true);
	s10c_host_midi_message(&F.host, te_begin, sizeof(te_begin));
	s10c_host_midi_message(&F.host, te_chunk, sizeof(te_chunk));
	F.start_ok = false;
	F.nmsgs = 0;
	s10c_host_midi_message(&F.host, te_begin, sizeof(te_begin));
	CHECK(F.discards == 1 && F.starts == 2, "MIDI: the transfer discarded");
	CHECK(F.nmsgs == 1 && F.msgs[0].len > 11 && F.msgs[0].m[7] == 0x01 &&
	      F.msgs[0].m[9] == 0x01, "MIDI: status 01 and an error text (S-10c.088)");
}

/* S-10c.073, S-10c.090 (Q-C15): a finish with no transfer is refused and
 * checks or installs nothing. */
static void test_finish_no_transfer(void)
{
	static const uint8_t finish[1] = { 0x05 };
	static const uint8_t te_finish[] = { 0xF0, 0x00, 0x20, 0x76, 0x21, 0x40,
					     0x60, 0x03, 0x03, 0x00, 0x04, 0xF7 };

	fake_boot(false, true);
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, finish, 1);
	CHECK(F.submits == 0 && F.nframes == 1 && F.frames[0].p[0] == 0x84 &&
	      F.frames[0].len == 23 &&
	      memcmp(F.frames[0].p + 1, "No update in progress!", 22) == 0,
	      "CDC: 84 No update in progress!, nothing submitted");
	CHECK(F.offers == 1, "CDC: the RAM disk offered again");
	s10c_host_midi_message(&F.host, te_finish, sizeof(te_finish));
	CHECK(F.submits == 0 && F.nmsgs == 1 && F.msgs[0].m[9] == 0x01,
	      "MIDI: status 01, nothing submitted");
	CHECK(F.offers == 2, "MIDI: the RAM disk offered again");
}

static const struct s10cv_step *sysex_step(const char *name)
{
	for (size_t i = 0; i < N(s10cv_sysex); i++)
		if (strcmp(s10cv_sysex[i].name, name) == 0)
			return &s10cv_sysex[i];
	return NULL;
}

/* S-10c.071, S-10c.088: a begin refused for its product word
 * opens the upgrade screen, then tells it the refusal and its reason; the
 * transfer in progress is left as it is. */
static void test_begin_refused(void)
{
	static const uint8_t ok[14] = { 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0,
					0x00, 0x08, 0x40, 0x00 };
	static const uint8_t bad[14] = { 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0,
					 0x00, 0x08, 0x30, 0x00 };
	static const uint8_t chunk[3] = { 0x03, 0xAA, 0xBB };
	static const char *const te_bad[] = {
		"dfu-begin-bad-product-34", "dfu-begin-bad-variant",
		"dfu-begin-bad-zero", "dfu-begin-bad-over-999",
		"dfu-begin-bad-variant-f",
	};
	const struct s10cv_step *v;

	fake_boot(false, true);
	F.nframes = 0;
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, bad, 14);
	CHECK(F.screens == 1 && F.refusals == 1 && F.screens_at_refusal == 1,
	      "CDC: the screen opens, then the refusal");
	CHECK(F.refused_reason == S11_REASON_PRODUCT, "CDC: the product reason");
	CHECK(F.starts == 0 && F.nframes == 1 && F.frames[0].p[0] == 0x84 &&
	      memcmp(F.frames[0].p + 1, "PRODUCT", 7) == 0,
	      "CDC: no transfer, 84 PRODUCT");
	/* shorter than 14 bytes: product word 0 */
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, ok, 13);
	CHECK(F.screens == 2 && F.refusals == 2, "CDC: a short begin refused");
	/* a passing begin is not refused; a later bad one keeps its transfer */
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, ok, 14);
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, chunk, 3);
	CHECK(F.screens == 3 && F.refusals == 2 && F.starts == 1,
	      "CDC: a good begin");
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, bad, 14);
	F.nframes = 0;
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, chunk, 3);
	CHECK(F.refusals == 3 && F.discards == 0 && F.appended == 4 &&
	      F.nframes == 1 && F.frames[0].p[0] == 0x54,
	      "CDC: the transfer in progress goes on");

	for (size_t i = 0; i < N(te_bad); i++) {
		v = sysex_step(te_bad[i]);
		CHECK(v != NULL, "vector %s", te_bad[i]);
		if (v == NULL)
			continue;
		fake_boot(false, true);
		s10c_host_midi_message(&F.host, B(v->host), v->host.len);
		CHECK(F.screens == 1 && F.refusals == 1 &&
		      F.screens_at_refusal == 1 &&
		      F.refused_reason == S11_REASON_PRODUCT && F.starts == 0,
		      "MIDI %s: the screen, then the refusal", te_bad[i]);
	}
	v = sysex_step("dfu-begin-ok");
	CHECK(v != NULL, "vector dfu-begin-ok");
	if (v != NULL) {
		fake_boot(false, true);
		s10c_host_midi_message(&F.host, B(v->host), v->host.len);
		CHECK(F.screens == 1 && F.refusals == 0 && F.starts == 1,
		      "MIDI: a good begin is not refused");
	}
	/* "dfu already in progress": nothing changes, no screen */
	v = sysex_step("dfu-begin-again-before-chunk");
	if (v != NULL) {
		fake_boot(false, true);
		s10c_host_midi_message(&F.host, B(v->host), v->host.len);
		s10c_host_midi_message(&F.host, B(v->host), v->host.len);
		CHECK(F.screens == 1 && F.refusals == 0, "MIDI: begin again");
	}

	/* without the hook nothing is reported, and nothing breaks */
	{
		struct s10c_ops bare = fake_ops;

		bare.begin_refused = NULL;
		fake_boot(false, true);
		s10c_host_init(&F.host, &bare, &F);
		F.nframes = 0;
		s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, bad, 14);
		CHECK(F.refusals == 0 && F.nframes == 1 && F.frames[0].p[0] == 0x84,
		      "no hook: the reply alone");
	}
}

/* S-07.076, S-08.004: the common command set on the coprocessor links */
static void test_coproc_links(void)
{
	static const enum s10c_link links[2] = { S10C_LINK_PSOC,
						 S10C_LINK_NORDIC };
	static const uint8_t ping[1] = { 0x55 };
	static const uint8_t begin[14] = { 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0,
					   0x00, 0x08, 0x40, 0x00 };
	static const uint8_t chunk[3] = { 0x03, 0xAA, 0xBB }, finish[1] = { 0x05 };

	static const unsigned to[2] = { 0x14, 0x13 };	/* S-10c.045 */

	for (unsigned l = 0; l < 2; l++) {
		enum s10c_link link = links[l];

		fake_boot(false, true);
		F.cdc_open = false;	/* S-10c.031 is CDC's alone */
		for (unsigned id = 0; id < 256; id++) {
			size_t before = F.nframes;
			bool answered = id == 0x01 || id == 0x05 ||
					id == 0x07 || id == 0x09 ||
					id == 0x0B || id == 0x0D || id == 0x1F;
			bool silent = id == 0x02 || id == 0x06 || id == 0xB0;
			bool r = s10c_host_frame(&F.host, link, 0x31,
						 (uint8_t)id, ping, 1);

			CHECK(r == (answered || silent),
			      "link %u id %02x served %d", l, id, r);
			CHECK(F.nframes == before + (answered ? 1u : 0u),
			      "link %u id %02x answered", l, id);
			if (answered && F.nframes == before + 1)
				CHECK(F.frames[before].link == link &&
				      F.frames[before].route ==
					      (id == 0x01 ? 0x13 : to[l]),
				      "link %u id %02x: on the link, route %02x",
				      l, id, F.frames[before].route);
		}
		CHECK(F.log_relays == 0 && F.log_configs == 1,
		      "link %u: log lines left to the caller", l);
		CHECK(F.frames[0].id == 0x02 && F.frames[0].len == 1 &&
		      F.frames[0].p[0] == 0x55, "link %u: the pong", l);
		CHECK(F.frames[1].id == 0x06 && F.frames[1].p[0] == 0x40,
		      "link %u: the handshake", l);

		/* a firmware run: forced; its reports, then the reply, on the
		 * link (S-10c.100, S-10c.045) */
		F.nframes = 0;
		(void)s10c_host_frame(&F.host, link, 0x31, 0x0D, begin, 14);
		(void)s10c_host_frame(&F.host, link, 0x31, 0x0D, chunk, 3);
		F.norder = 0;
		F.nframes = 0;
		(void)s10c_host_frame(&F.host, link, 0x31, 0x0D, finish, 1);
		CHECK(F.submits == 1 && F.source == S10C_SOURCE_FW_COPROC &&
		      F.appended == 2, "link %u: submitted, forced", l);
		/* the run's two reports and the outcome report (S-10c.106) */
		CHECK(F.norder == 4 && F.order[0] == 'P' && F.order[1] == 'P' &&
		      F.order[2] == 'P' && F.order[3] == 'R',
		      "link %u: the reports, then the reply: %.*s", l,
		      (int)F.norder, F.order);
		CHECK(F.reports >= 3 && F.report_link == link &&
		      F.report_route == s10cv_coproc_progress[l].id,
		      "link %u: the reports on the link, route %02x", l,
		      F.report_route);
		CHECK(F.nframes == 1 && F.frames[0].link == link &&
		      F.frames[0].route == to[l] && F.frames[0].id == 0x0E &&
		      F.frames[0].p[0] == 0x54, "link %u: 0E 54", l);
		/* progress outside a submission goes to CDC again */
		F.cdc_open = true;
		F.norder = 0;
		s10c_host_progress(&F.host, 1, 2, "[main dsp]");
		CHECK(F.norder == 1 && F.order[0] == 'P',
		      "link %u: the default sink restored", l);
	}
}

/* S-10c.045 service vectors: every id a coprocessor's document leaves to
 * the common set; 03 and 04 are its caller's log lines (S-14.040). */
static void test_coproc_service(void)
{
	static const uint8_t ping[1] = { 0x55 };

	for (size_t i = 0; i < N(s10cv_coproc_service); i++) {
		const struct s10cv_service *v = &s10cv_coproc_service[i];
		enum s10c_link link = v->link == 2 ? S10C_LINK_PSOC :
						     S10C_LINK_NORDIC;
		bool r;

		if (v->cls == 0)
			continue;
		fake_boot(false, true);
		r = s10c_host_frame(&F.host, link, 0x31, (uint8_t)v->id,
				    ping, 1);
		CHECK(F.nframes == (v->cls == 1 ? 1u : 0u),
		      "link %d id %02x: %zu frames", v->link, v->id, F.nframes);
		if (v->id != 0x03 && v->id != 0x04)
			CHECK(r == (v->cls != 3), "link %d id %02x served %d",
			      v->link, v->id, r);
	}
}
/* S-10c.076: continue to the OS on CDC, MIDI and a coprocessor link: the
 * reply 54 first, then the board's restart; 84 and a text with no effect
 * while busy (the board's, or a host transfer here) and before held mode. */
static void test_continue(void)
{
	static const uint8_t cont[3] = { 0x19, 0x01, 0x02 }, only[1] = { 0x19 };
	static const uint8_t begin[18] = { 0x02, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03,
		0x00, 0x04, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00, 0x0f, 0xa0 };
	static const enum s10c_link links[3] = { S10C_LINK_CDC, S10C_LINK_MIDI,
						 S10C_LINK_PSOC };
	static const uint8_t routes[3] = { 0x12, 0x12, 0x14 };
	struct s10c_ops bare = fake_ops;

	for (unsigned l = 0; l < 3u; l++) {
		fake_boot(false, true);
		F.cont = S10C_CONT_GO;
		s10c_host_frame(&F.host, links[l], l == 2u ? 0x41 : 0x21, 0x0D, cont,
				sizeof(cont));
		CHECK(F.continued == 1u && F.cont_link == links[l], "link %u: continued", l);
		CHECK(F.nframes == 1u && F.frames_at_cont == 1u &&
		      F.frames[0].link == links[l] && F.frames[0].route == routes[l] &&
		      F.frames[0].id == 0x0E && F.frames[0].len == 1u &&
		      F.frames[0].p[0] == 0x54, "link %u: the reply 54 first", l);
	}
	for (int c = S10C_CONT_BUSY; c <= S10C_CONT_NOT_HELD; c++) {
		fake_boot(false, true);
		F.cont = (enum s10c_continue)c;
		s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, only, 1u);
		CHECK(F.continued == 0u && F.nframes == 1u && F.frames[0].id == 0x0E &&
		      F.frames[0].len > 1u && F.frames[0].p[0] == 0x84,
		      "refused %d: 84 and a text", c);
	}
	/* S-10c.076: never forwarded. Whatever the answer, nothing leaves
	 * for it but the reply on the asking link: no 0D, nothing to the
	 * Nordic. */
	for (int c = S10C_CONT_GO; c <= S10C_CONT_NOT_HELD; c++) {
		for (unsigned l = 0; l < 3u; l++) {
			fake_boot(false, true);
			F.cont = (enum s10c_continue)c;
			s10c_host_frame(&F.host, links[l], l == 2u ? 0x41 : 0x21,
					0x0D, only, 1u);
			CHECK(F.nframes == 1u, "one frame (%d, %u)", c, l);
			for (unsigned i = 0; i < F.nframes; i++)
				CHECK(F.frames[i].id != 0x0D &&
				      F.frames[i].link == links[l] &&
				      F.frames[i].link != S10C_LINK_NORDIC,
				      "not forwarded (%d, %u)", c, l);
		}
	}
	/* a host transfer under way */
	fake_boot(false, true);
	F.cont = S10C_CONT_GO;
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, begin, sizeof(begin));
	s10c_host_frame(&F.host, S10C_LINK_CDC, 0x21, 0x0D, only, 1u);
	CHECK(F.continued == 0u && F.nframes == 2u && F.frames[1].p[0] == 0x84 &&
	      F.host.transfer, "busy: a transfer; it goes on");
	/* no op: before held mode */
	fake_boot(false, true);
	bare.continue_check = NULL;
	bare.continue_os = NULL;
	s10c_host_init(&F.host, &bare, &F);
	s10c_host_frame(&F.host, S10C_LINK_MIDI, 0x21, 0x0D, only, 1u);
	CHECK(F.continued == 0u && F.nframes == 1u && F.frames[0].p[0] == 0x84,
	      "before held mode: 84");
}

int main(void)
{
	test_pack();
	test_usbmidi();
	test_classify();
	test_carrier();
	test_sysex();
	test_commands();
	test_effects();
	test_coproc_service();
	test_factory_gate();
	test_setmeta();
	test_info();
	test_progress_texts();
	test_labels_and_cdc();
	test_error_texts();
	test_finish();
	test_begin_cannot_start();
	test_begin_refused();
	test_finish_no_transfer();
	test_coproc_links();
	test_continue();
	return s11_check_report("test_s10c_host");
}

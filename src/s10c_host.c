/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/s10c_host.h"

#include "waffle/bytes.h"

#define ROUTE_TO_HOST 0x12u	/* source 1, destination 2 (S-10c.042) */
#define ROUTE_TO_PSOC 0x14u	/* source 1, destination 4 (S-10c.045) */
#define ROUTE_TO_NORDIC 0x13u	/* source 1, destination 3 (S-10c.045) */

static const char no_update[] = "No update in progress!";

static bool coproc(enum s10c_link link)
{
	return link == S10C_LINK_PSOC || link == S10C_LINK_NORDIC;
}

static void default_sink(struct s10c_host *h)
{
	h->sink.link = S10C_LINK_CDC;	/* S-10c.100 */
	h->sink.te = false;
}

void s10c_host_init(struct s10c_host *h, const struct s10c_ops *ops,
		    void *ctx, const struct wb_device *dev,
		    enum wb_dev_how how)
{
	h->ops = ops;
	h->ctx = ctx;
	h->dev = dev;
	h->how = how;
	h->factory = false;		/* S-10c.053: off at each boot */
	h->transfer = false;
	h->got_chunk = false;
	h->expected_chunk = 0;
	default_sink(h);
}

/* ---- sending ---- */

/*
 * S-10c.031: a frame for CDC while the port is not open is dropped.
 * S-10c.045: on a coprocessor link a reply other than a pong goes from 1
 * to the coprocessor's address, not to the host's.
 */
static void send(struct s10c_host *h, enum s10c_link link, uint8_t route,
		 uint8_t id, const uint8_t *p, size_t n)
{
	if (link == S10C_LINK_CDC && !h->ops->cdc_open(h->ctx))
		return;
	if (route == ROUTE_TO_HOST && link == S10C_LINK_PSOC)
		route = ROUTE_TO_PSOC;
	else if (route == ROUTE_TO_HOST && link == S10C_LINK_NORDIC)
		route = ROUTE_TO_NORDIC;
	h->ops->send_frame(h->ctx, link, route, id, p, n);
}

static void reply1(struct s10c_host *h, enum s10c_link link, uint8_t id,
		   uint8_t b)
{
	send(h, link, ROUTE_TO_HOST, id, &b, 1);
}

/* A 0E reply: `status` (54 done, 84 error) then a text of at most 247
 * bytes (S-10c.070). */
static void reply_fw(struct s10c_host *h, enum s10c_link link, uint8_t status,
		     const char *text, size_t n)
{
	uint8_t p[1 + S10C_TEXT_MAX];

	if (n > S10C_TEXT_MAX)
		n = S10C_TEXT_MAX;
	p[0] = status;
	for (size_t i = 0; i < n; i++)
		p[1 + i] = (uint8_t)text[i];
	send(h, link, ROUTE_TO_HOST, 0x0E, p, 1 + n);
}

static void reply_fw_str(struct s10c_host *h, enum s10c_link link,
			 const char *text)
{
	reply_fw(h, link, 0x84, text, wb_strlen(text));
}

static void te_reply(struct s10c_host *h, const struct s10c_te_req *req,
		     uint8_t status, const uint8_t *p, size_t n)
{
	uint8_t msg[S10C_SYSEX_MAX_SENT];

	h->ops->send_sysex(h->ctx, msg, s10c_te_reply_build(h->dev, req, status,
							    p, n, msg));
}

static void te_reply_str(struct s10c_host *h, const struct s10c_te_req *req,
			 uint8_t status, const char *text)
{
	te_reply(h, req, status, (const uint8_t *)text, wb_strlen(text));
}

/* ---- progress (S-10c.100-S-10c.106) ---- */

static void report(struct s10c_host *h, uint32_t done, uint32_t total,
		   bool outcome, const char *label)
{
	uint8_t p[1 + S10C_TEXT_MAX];
	size_t n;

	if (h->sink.te) {	/* S-10c.102: a reply to the finish request */
		n = s10c_progress_te(done, total, outcome, label, p);
		te_reply(h, &h->sink.req, 0x40, p, n);
		return;
	}
	/* S-10c.100, S-10c.045: to the link the finish came on, a
	 * coprocessor's included, in S-10c.101's form */
	n = s10c_progress_s06(done, total, label, p);	/* S-10c.101 */
	send(h, h->sink.link, ROUTE_TO_HOST, 0x0E, p, n);
}

void s10c_host_progress(struct s10c_host *h, uint32_t done, uint32_t total,
			const char *label)
{
	report(h, done, total, false, label);
}

/* S-10c.106 */
void s10c_host_outcome(struct s10c_host *h, const struct s11_outcome *o)
{
	char text[9 + S10C_TEXT_MAX + 1];	/* "[failed] ", the text, NUL */
	size_t n;

	switch (o->kind) {
	case S11_OUTCOME_DONE:		/* percent 100 in the TE form */
		report(h, 0, 0, true, "[done]");
		break;
	case S11_OUTCOME_NOTHING_TO_DO:
		report(h, 0, 0, true, "[nothing to do]");
		break;
	case S11_OUTCOME_REFUSED:
	case S11_OUTCOME_FAILED:
	default:
		n = s10c_error_text(o, text + 9);
		for (size_t i = 0; i < 9; i++)
			text[i] = "[failed] "[i];
		text[9 + n] = '\0';
		s10c_host_progress(h, 0, 0, text);
		break;
	}
}

static bool completed(const struct s11_outcome *o)
{
	return o->kind == S11_OUTCOME_DONE ||
	       o->kind == S11_OUTCOME_NOTHING_TO_DO;
}

/* Submits the transfer (S-11.050) with progress to `sink`, then the outcome
 * report of a completed run (S-10c.106). Every report precedes the final
 * reply, which the caller sends after this returns (S-10c.107). */
static void submit(struct s10c_host *h, enum s10c_source source,
		   const struct s10c_progress_sink *sink, struct s11_outcome *o)
{
	h->transfer = false;
	h->sink = *sink;
	h->ops->transfer_submit(h->ctx, source, o);
	if (completed(o))
		s10c_host_outcome(h, o);
}

static void end_transfer(struct s10c_host *h)
{
	if (h->transfer)
		h->ops->transfer_discard(h->ctx);
	h->transfer = false;
}

/*
 * S-10c.071, S-10c.088: the begin opened the upgrade screen before its
 * product word was checked; the screen shows the refusal there.
 */
static void begin_refused(struct s10c_host *h)
{
	if (h->ops->begin_refused != NULL)
		h->ops->begin_refused(h->ctx, S11_REASON_PRODUCT);
}

/* ---- the host link's consent ---- */

static enum s10c_gate gate(struct s10c_host *h, enum s10c_link link)
{
	/* the PSoC is the unit's own; the Nordic may carry another host's */
	if (link == S10C_LINK_PSOC || h->ops->gate == NULL)
		return S10C_GATE_OK;
	return h->ops->gate(h->ctx, link);
}

static const char *gate_text(enum s10c_gate g)
{
	return g == S10C_GATE_OFF ? "host link off" :
	       g == S10C_GATE_BUSY ? "busy: a computer has the backplane" :
				     "not allowed: hold 1 on the unit";
}

/* A change not served: its refusal, as the firmware command's 84. */
static bool fw_gated(struct s10c_host *h, enum s10c_link link)
{
	const enum s10c_gate g = gate(h, link);

	if (g != S10C_GATE_OK)
		reply_fw_str(h, link, gate_text(g));
	return g != S10C_GATE_OK;
}

/* ---- the firmware command 0D (S-10c.070-S-10c.075) ---- */

static void fw_begin(struct s10c_host *h, enum s10c_link link,
		     const uint8_t *p, size_t n)
{
	/* S-10c.071: shorter than 14 bytes, the product word is 0 */
	uint32_t w = n >= 14u ? wb_get_be32(p + 10) : 0;

	h->ops->upgrade_screen(h->ctx, 0);
	if (!s11_product_ok(h->dev, w)) {
		begin_refused(h);
		reply_fw_str(h, link, "PRODUCT: incorrect sku");
		return;
	}
	end_transfer(h);
	if (!h->ops->transfer_start(h->ctx)) {
		reply_fw_str(h, link, "cannot prepare the transfer");
		return;
	}
	h->transfer = true;
	h->got_chunk = false;
	h->expected_chunk = 0;
	reply1(h, link, 0x0E, 0x54);
}

/* S-10c.072 */
static void fw_chunk(struct s10c_host *h, enum s10c_link link,
		     const uint8_t *p, size_t n)
{
	if (!h->transfer) {
		reply_fw_str(h, link, no_update);
		return;
	}
	if (!h->ops->transfer_append(h->ctx, p + 1, n - 1)) {
		end_transfer(h);
		reply_fw_str(h, link, "cannot store the chunk");
		return;
	}
	h->got_chunk = true;
	reply1(h, link, 0x0E, 0x54);
}

static void fw_finish(struct s10c_host *h, enum s10c_link link)
{
	struct s10c_progress_sink sink = { link, false, { 0 } };
	struct s11_outcome o;

	if (!h->transfer) {
		reply_fw_str(h, link, no_update);
	} else {
		/* S-10c.075: forced on either link */
		submit(h, link == S10C_LINK_CDC ? S10C_SOURCE_FW_CDC :
		       link == S10C_LINK_MIDI ? S10C_SOURCE_FW_MIDI :
						S10C_SOURCE_FW_COPROC,
		       &sink, &o);
		if (completed(&o)) {
			reply1(h, link, 0x0E, 0x54);
		} else {
			char text[S10C_TEXT_MAX];
			size_t n = s10c_error_text(&o, text);

			reply_fw(h, link, 0x84, text, n);
		}
		default_sink(h);
	}
	h->ops->ramdisk_offer(h->ctx);	/* S-10c.073 */
}

/*
 * S-10c.076: continue to the OS. Refused with 84 and a text while it would
 * not start, with no effect; otherwise the reply first, since the restart
 * does not return (S-04.063 lets it leave before the detach). Payload bytes
 * after the sub-command are ignored. A transfer in progress here is a host
 * transfer under way.
 */
static void fw_continue(struct s10c_host *h, enum s10c_link link)
{
	enum s10c_continue c = h->ops->continue_check != NULL ?
			       h->ops->continue_check(h->ctx) : S10C_CONT_NOT_HELD;

	if (c == S10C_CONT_GO && h->transfer)
		c = S10C_CONT_BUSY;
	switch (c) {
	case S10C_CONT_GO:
		reply1(h, link, 0x0E, 0x54);
		h->ops->continue_os(h->ctx, link);
		break;
	case S10C_CONT_BUSY:
		reply_fw_str(h, link, "busy: an install or OS write is under way");
		break;
	case S10C_CONT_NOT_HELD:
	default:
		reply_fw_str(h, link, "not in the boot menu");
		break;
	}
}

/* S-10c.070 */
static void firmware(struct s10c_host *h, enum s10c_link link,
		     const uint8_t *p, size_t n)
{
	const unsigned sub = n == 0 ? 0x100u : p[0];

	if ((sub == 0x02 || sub == 0x03 || sub == 0x05 || sub == 0x18 ||
	     sub == 0x19 || sub == 0xEE) && fw_gated(h, link))
		return;
	switch (sub) {
	case 0x02:
		fw_begin(h, link, p, n);
		break;
	case 0x03:
		fw_chunk(h, link, p, n);
		break;
	case 0x05:
		fw_finish(h, link);
		break;
	case 0x18:		/* S-10c.074: no reply */
		h->ops->reboot_os(h->ctx);
		break;
	case 0x19:
		/* S-10c.076: served here; nothing is sent for it but the
		 * reply on its own link, so it never reaches the Nordic. */
		fw_continue(h, link);
		break;
	case 0xEE:
		h->ops->reboot_mode(h->ctx, n >= 2, n >= 2 ? p[1] : 0);
		break;
	case 0xAA:		/* quirk S-10c Q-C5: the text alone */
		reply_fw_str(h, link, "compressed firmwares not implemented");
		break;
	case 0xAB:
		reply_fw_str(h, link, "signed firmwares not implemented");
		break;
	default:
		reply_fw_str(h, link, "unknown firmware sub-command");
		break;
	}
}

/* ---- S-06 commands (S-10c.040-S-10c.061) ---- */

/*
 * S-10c.041 (quirk Q-C9 for the MIDI set). A coprocessor link takes the
 * CDC set, S-07's "common command set" (S-07.076, S-08.004), but for the
 * log lines, which its caller relays by their source (S-14.040, S-14.041).
 * S-10c.045: every id the coprocessor's document does not claim is served
 * as on CDC, factory unlock and set metadata still behind S-10c.061.
 */
static bool served(enum s10c_link link, uint8_t id)
{
	switch (id) {
	case 0x01: case 0x02: case 0x09: case 0x0D: case 0x1F:
		return true;
	case 0x03: case 0x04:
		return link == S10C_LINK_CDC;
	case 0x05: case 0x06: case 0x07: case 0x0B: case 0xB0:
		return link == S10C_LINK_CDC || coproc(link);
	default:
		return false;
	}
}

static void info(struct s10c_host *h, char *text, size_t *n)
{
	struct s10c_info v;

	h->ops->read_info(h->ctx, &v);
	*n = s10c_info_text(h->dev, h->how, &v, text);
}

/* S-10c.042, S-10c.045: a pong's route is the ping's exchanged; every other
 * reply is 0x12, or on a coprocessor link from 1 to the coprocessor. */
bool s10c_host_frame(struct s10c_host *h, enum s10c_link link, uint8_t route,
		     uint8_t id, const uint8_t *p, size_t n)
{
	static const uint8_t unlock[4] = { 0xCA, 0xFE, 0xBA, 0xBE };
	char text[S10C_INFO_TEXT_MAX];
	uint8_t fsn[16];
	uint32_t words[4];
	struct s10c_setmeta m;
	size_t tn;
	bool ok;

	if (!served(link, id))
		return false;
	switch (id) {
	case 0x01:	/* S-10c.050, S-10c.042: the route's nibbles exchanged */
		send(h, link, (uint8_t)((route << 4) | (route >> 4)), 0x02, p, n);
		break;
	case 0x05:	/* S-10c.052 */
		reply1(h, link, 0x06, 0x40);
		break;
	case 0x07:	/* S-10c.053, S-10c.061 */
		ok = gate(h, link) == S10C_GATE_OK && h->ops->option_on(h->ctx) && n == 4;
		for (size_t i = 0; ok && i < 4; i++)
			ok = p[i] == unlock[i];
		h->factory = ok;
		reply1(h, link, 0x08, ok ? 0x03 : 0x04);
		break;
	case 0x09:	/* S-10c.054 */
		info(h, text, &tn);
		send(h, link, ROUTE_TO_HOST, 0x0A, (const uint8_t *)text, tn);
		break;
	case 0x0B:	/* S-10c.056, S-10c.061 */
		ok = h->factory && h->ops->option_on(h->ctx) &&
		     gate(h, link) == S10C_GATE_OK;
		if (ok) {
			s10c_setmeta_parse(p, n, &m);
			/* S-10c.056: the factory key only with factory-word */
			if (!wb_has(h->dev, WB_F_FACTORY_WORD))
				m.has_factory = false;
			ok = h->ops->write_metadata(h->ctx, &m);
		}
		reply1(h, link, 0x0C, ok ? 0x03 : 0x04);
		break;
	case 0x0D:
		firmware(h, link, p, n);
		break;
	case 0x1F:	/* S-10c.057; quirk Q-C4: empty when unreadable */
		ok = h->ops->read_fsn(h->ctx, words);
		for (unsigned i = 0; i < 16; i++)	/* each word LSB first */
			fsn[i] = (uint8_t)(words[i / 4] >> (8 * (i % 4)));
		send(h, link, ROUTE_TO_HOST, 0x20, fsn, ok ? sizeof(fsn) : 0);
		break;
	case 0xB0:	/* S-10c.059, S-10c.128: the log to a host, with consent */
		if (gate(h, link) == S10C_GATE_OK)
			h->ops->log_config(h->ctx, p, n);
		break;
	case 0x03:
	case 0x04:	/* S-10c.060 */
		h->ops->log_relay(h->ctx, id, p, n);
		break;
	case 0x02:
	case 0x06:	/* S-10c.051: no effect */
	default:
		break;
	}
	return true;
}

/* ---- the MIDI firmware transfer (S-10c.080-S-10c.091) ---- */

static void te_begin(struct s10c_host *h, const struct s10c_te_req *req,
		     const uint8_t *p)
{
	uint32_t w = wb_get_be32(p + 10);

	if (h->transfer && !h->got_chunk) {
		te_reply_str(h, req, 0x01, "dfu already in progress");
		return;
	}
	h->ops->upgrade_screen(h->ctx, wb_get_be32(p + 14));
	if (!s11_product_ok(h->dev, w)) {
		char text[32] = "incorrect sku: ";

		begin_refused(h);
		s10c_te_code(w, text + 15);
		te_reply(h, req, 0x01, (const uint8_t *)text, 25);
		return;
	}
	end_transfer(h);
	if (!h->ops->transfer_start(h->ctx)) {
		/* S-10c.088 (quirk Q-C14): status 01 with an error text, the
		 * transfer in progress already discarded, as S-10c.071. */
		te_reply_str(h, req, 0x01, "cannot prepare the transfer");
		return;
	}
	h->transfer = true;
	h->got_chunk = false;
	h->expected_chunk = 0;
	te_reply(h, req, 0x00, (const uint8_t *)"\x10\x00", 2);
}

static void put_u8_text(char *buf, size_t *n, uint8_t v)
{
	*n += s11_u32_text(v, buf + *n, 3);
}

/* S-10c.089 */
static void te_chunk(struct s10c_host *h, const struct s10c_te_req *req,
		     const uint8_t *p, size_t n)
{
	static const char err[] = "unexpected error when handling chunk";

	if (p[1] != h->expected_chunk) {
		char text[40] = "unexpected chunk ";
		size_t tn = 17;

		put_u8_text(text, &tn, p[1]);
		for (const char *s = " (exp "; *s != '\0'; s++)
			text[tn++] = *s;
		put_u8_text(text, &tn, h->expected_chunk);
		text[tn++] = ')';
		te_reply(h, req, 0x01, (const uint8_t *)text, tn);
		return;
	}
	if (!h->transfer) {
		te_reply_str(h, req, 0x01, err);
		return;
	}
	if (n > 2 && !h->ops->transfer_append(h->ctx, p + 2, n - 2)) {
		end_transfer(h);
		te_reply_str(h, req, 0x01, err);
		return;
	}
	h->expected_chunk = (uint8_t)(p[1] + 1u);
	h->got_chunk = true;
	te_reply(h, req, 0x00, NULL, 0);
}

/* S-10c.090 */
static void te_finish(struct s10c_host *h, const struct s10c_te_req *req)
{
	static const char head[] = "error when doing dfu: '";
	struct s10c_progress_sink sink = { S10C_LINK_MIDI, true, *req };
	struct s11_outcome o;
	char text[sizeof(head) + S10C_TEXT_MAX + 1];
	size_t n = sizeof(head) - 1, en;

	for (size_t i = 0; i < n; i++)
		text[i] = head[i];
	if (!h->transfer) {
		en = wb_strlen(no_update);
		for (size_t i = 0; i < en; i++)
			text[n + i] = no_update[i];
	} else {
		submit(h, S10C_SOURCE_TE, &sink, &o);	/* S-10c.092 */
		default_sink(h);
		if (completed(&o)) {
			te_reply(h, req, 0x00, NULL, 0);
			h->ops->ramdisk_offer(h->ctx);
			return;
		}
		en = s10c_error_text(&o, text + n);
	}
	n += en;
	text[n++] = '\'';
	te_reply(h, req, 0x01, (const uint8_t *)text, n > 127u ? 127u : n);
	h->ops->ramdisk_offer(h->ctx);	/* S-16.021 */
}

/* S-10c.086 */
static void te_upgrade(struct s10c_host *h, const struct s10c_te_req *req,
		       const uint8_t *p, size_t n)
{
	const enum s10c_gate g = n >= 1 && p[0] >= 0x02 && p[0] <= 0x05 ?
				 gate(h, S10C_LINK_MIDI) : S10C_GATE_OK;

	if (g != S10C_GATE_OK) {
		te_reply_str(h, req, 0x01, gate_text(g));
		return;
	}
	if (n >= 2 && p[0] == 0x03) {
		te_chunk(h, req, p, n);
	} else if (n >= 18 && p[0] == 0x02) {
		te_begin(h, req, p);
	} else if (n >= 1 && p[0] == 0x04) {
		te_finish(h, req);
	} else if (n >= 1 && p[0] == 0x01) {	/* S-10c.087 */
		te_reply(h, req, 0x00, NULL, 0);
	} else if (n >= 1 && p[0] == 0x05) {	/* S-10c.091 */
		te_reply(h, req, 0x00, (const uint8_t *)"\x09", 1);
		h->ops->reboot_after_dfu(h->ctx);
	} else {
		te_reply(h, req, 0x03, NULL, 0);
	}
}

static void te_request(struct s10c_host *h, const uint8_t *m, size_t mn)
{
	struct s10c_te_req req;
	char text[S10C_INFO_TEXT_MAX];
	size_t n;

	s10c_te_parse(m, mn, &req);
	if (req.overflow) {			/* S-10c.082 */
		end_transfer(h);
		te_reply_str(h, &req, 0x01, "buffer overflow");
		return;
	}
	n = s10c_unpack7(req.packed, req.packed_len, h->buf);
	switch (req.cmd) {
	case 0x01:				/* S-10c.084 */
		info(h, text, &n);
		te_reply(h, &req, 0x00, (const uint8_t *)text, n);
		break;
	case 0x02:				/* S-10c.085, quirk Q-C1 */
		if (n > 242u)
			te_reply_str(h, &req, 0x01, "buffer overflow");
		else
			te_reply(h, &req, 0x00, h->buf, n);
		break;
	case 0x03:
		te_upgrade(h, &req, h->buf, n);
		break;
	default:				/* S-10c.083 */
		te_reply(h, &req, 0x02, NULL, 0);
		break;
	}
}

/* S-10c.032: the bytes of a frame incomplete at the request are dropped */
void s10c_host_cdc_line_state(struct s10c_host *h)
{
	h->ops->cdc_rx_restart(h->ctx);
}

/* S-10c.011: the family member from the metadata's sku when it is built,
 * where the device's member follows the sku */
static void identity(struct s10c_host *h)
{
	uint8_t r[S10C_IDENTITY_REPLY_LEN];
	struct s10c_info v;
	uint16_t sku = 0xFFFFu;

	if (h->dev->ident.member_from_sku) {
		h->ops->read_info(h->ctx, &v);
		sku = v.sku > 0xFFFFu ? 0xFFFFu : (uint16_t)v.sku;
	}
	s10c_identity_reply(h->dev, sku, r);
	h->ops->send_sysex(h->ctx, r, sizeof(r));
}

/* S-10c.007, S-10c.010, S-10c.021 */
void s10c_host_midi_message(struct s10c_host *h, const uint8_t *m, size_t n)
{
	uint8_t stream[S10C_CARRIER_MAX_RX];
	size_t sn;

	switch (s10c_classify(h->dev, m, n)) {
	case S10C_MSG_TE:
		te_request(h, m, n);
		break;
	case S10C_MSG_IDENTITY:
		identity(h);
		break;
	case S10C_MSG_CARRIER:
		/* each carrier message starts the receiver afresh (quirk
		 * S-10c Q-C10); one over 512 stream bytes is ignored whole */
		if (s10c_carrier_stream(m, n, stream, &sn)) {
			h->ops->midi_rx_restart(h->ctx);
			h->ops->midi_rx_feed(h->ctx, stream, sn);
		}
		break;
	case S10C_MSG_IGNORED:
	default:
		break;
	}
}

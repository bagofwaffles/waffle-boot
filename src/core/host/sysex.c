/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/host/sysex.h"

#include "waffle/boot/device.h"

/* ---- packing ---- */

size_t s10c_packed_len(size_t n)
{
	return n + (n + 6u) / 7u;
}

size_t s10c_unpacked_len(size_t m)
{
	return m - (m + 7u) / 8u;
}

/* S-10c.001 */
size_t s10c_pack7(const uint8_t *in, size_t n, uint8_t *out)
{
	size_t o = 0;

	for (size_t i = 0; i < n; i += 7u) {
		size_t g = n - i < 7u ? n - i : 7u;
		uint8_t hdr = 0;

		for (size_t k = 0; k < g; k++)
			hdr = (uint8_t)(hdr | ((in[i + k] >> 7) << k));
		out[o++] = hdr;
		for (size_t k = 0; k < g; k++)
			out[o++] = (uint8_t)(in[i + k] & 0x7Fu);
	}
	return o;
}

/* S-10c.002: bit 7 of every received byte is ignored; a last group with a
 * header and no data adds nothing. */
size_t s10c_unpack7(const uint8_t *in, size_t m, uint8_t *out)
{
	size_t o = 0;

	for (size_t i = 0; i < m; i += 8u) {
		uint8_t hdr = (uint8_t)(in[i] & 0x7Fu);

		for (size_t k = 1; k < 8u && i + k < m; k++)
			out[o++] = (uint8_t)((in[i + k] & 0x7Fu) |
					     ((((unsigned)hdr >> (k - 1)) & 1u) << 7));
	}
	return o;
}

/* ---- event packets ---- */

/* S-10c.003 */
void s10c_usbmidi_send(const uint8_t *msg, size_t n, s10c_packet_fn emit,
		       void *ctx)
{
	for (size_t i = 0; i < n; i += 3u) {
		size_t left = n - i;
		uint8_t p[4] = { 0x04, 0, 0, 0 };

		if (left <= 3u)
			p[0] = (uint8_t)(0x04u + left);	/* 5, 6 or 7 */
		else
			left = 3u;
		for (size_t k = 0; k < left; k++)
			p[1 + k] = msg[i + k];
		emit(ctx, p);
	}
}

void s10c_midi_rx_init(struct s10c_midi_rx *rx)
{
	rx->len = 0;
	rx->overlong = false;
}

static void rx_add(struct s10c_midi_rx *rx, uint8_t b)
{
	if (rx->len == S10C_SYSEX_MAX_RECEIVED)
		rx->overlong = true;	/* S-10c.006: discarded at its end */
	else
		rx->buf[rx->len++] = b;
}

/* S-10c.004, S-10c.006 */
size_t s10c_midi_rx_packet(struct s10c_midi_rx *rx, const uint8_t packet[4])
{
	unsigned cin = packet[0] & 0x0Fu;
	size_t n;

	if ((packet[0] >> 4) != 0)	/* another cable: ignored */
		return 0;
	switch (cin) {
	case 0x4:
		for (unsigned k = 1; k < 4; k++)
			rx_add(rx, packet[k]);
		return 0;
	case 0xF:
		if (packet[1] < 0x80u)
			rx_add(rx, packet[1]);
		return 0;
	case 0x5:
	case 0x6:
	case 0x7:
		for (unsigned k = 1; k <= cin - 4u; k++)
			rx_add(rx, packet[k]);
		n = rx->len;
		if (rx->overlong || n < 2 || rx->buf[0] != 0xF0 ||
		    rx->buf[n - 1] != 0xF7)
			n = 0;
		rx->len = 0;
		rx->overlong = false;
		return n;
	default:
		return 0;	/* any other code index: ignored */
	}
}

/* ---- classification ---- */

/* S-10c.011 */
void s10c_identity_reply(const struct wb_device *dev, uint16_t sku,
			 uint8_t out[S10C_IDENTITY_REPLY_LEN])
{
	static const uint8_t head[8] = { 0xF0, 0x7E, 0x00, 0x06, 0x02, 0x00,
					 0x20, 0x76 };
	unsigned i;

	for (i = 0; i < sizeof(head); i++)
		out[i] = head[i];
	out[2] = dev->ident.midi_byte;
	out[8] = dev->ident.midi_family[0];
	out[9] = dev->ident.midi_family[1];
	wb_midi_member(dev, sku, out + 10);
	for (i = 12; i < 16u; i++)
		out[i] = 0x00;		/* software revision */
	out[16] = 0xF7;
}

static const uint8_t te_head[4] = { 0xF0, 0x00, 0x20, 0x76 };
#define CARRIER_HEAD 6u
#define CARRIER_ID 0x66u

/* S-10c.020: F0 00 20 76, device.midi_byte, 66 */
static bool carrier_head(const struct wb_device *dev, const uint8_t *m, size_t n)
{
	return n >= CARRIER_HEAD && m[0] == 0xF0 && m[1] == 0x00 &&
	       m[2] == 0x20 && m[3] == 0x76 && m[4] == dev->ident.midi_byte &&
	       m[5] == CARRIER_ID;
}

static bool starts(const uint8_t *m, size_t n, const uint8_t *h, size_t hn)
{
	if (n < hn)
		return false;
	for (size_t i = 0; i < hn; i++) {
		if (m[i] != h[i])
			return false;
	}
	return true;
}

/* S-10c.081 */
static bool is_te(const uint8_t *m, size_t n)
{
	if (n < 10u || !starts(m, n, te_head, 4) || m[5] != 0x40u ||
	    m[n - 1] != 0xF7u)
		return false;
	/* the reply form (flags bit 6 clear) needs 11 bytes */
	return (m[6] & 0x40u) != 0 || n >= 11u;
}

/* S-10c.007 */
enum s10c_msg_class s10c_classify(const struct wb_device *dev,
				  const uint8_t *m, size_t n)
{
	if (is_te(m, n))
		return S10C_MSG_TE;
	if (n == 6 && m[0] == 0xF0 && m[1] == 0x7E &&
	    (m[2] == dev->ident.midi_byte || m[2] == 0x7F) && m[3] == 0x06 &&
	    m[4] == 0x01 && m[5] == 0xF7)
		return S10C_MSG_IDENTITY;	/* S-10c.010 */
	if (n >= 7 && carrier_head(dev, m, n) && m[n - 1] == 0xF7)
		return S10C_MSG_CARRIER;
	return S10C_MSG_IGNORED;
}

/* ---- the carrier ---- */

/* S-10c.020 */
size_t s10c_carrier_build(const struct wb_device *dev, const uint8_t *stream,
			  size_t n, uint8_t *out)
{
	size_t o = CARRIER_HEAD;

	for (size_t i = 0; i < sizeof(te_head); i++)
		out[i] = te_head[i];
	out[4] = dev->ident.midi_byte;
	out[5] = CARRIER_ID;
	o += s10c_pack7(stream, n, out + o);
	out[o++] = 0xF7;
	return o;
}

/* S-10c.022 (S-16.021, quirk Q-C1) */
void s10c_carrier_send_frame(const struct wb_device *dev,
			     const uint8_t *encoded, size_t n,
			     s10c_sysex_fn emit, void *ctx)
{
	uint8_t msg[S10C_SYSEX_MAX_SENT];

	for (size_t i = 0; i < n; i += S10C_CARRIER_MAX_STREAM) {
		size_t k = n - i;

		if (k > S10C_CARRIER_MAX_STREAM)
			k = S10C_CARRIER_MAX_STREAM;
		emit(ctx, msg, s10c_carrier_build(dev, encoded + i, k, msg));
	}
}

/* S-10c.021 */
bool s10c_carrier_stream(const uint8_t *m, size_t n, uint8_t *out,
			 size_t *out_len)
{
	size_t body = n - 7u;

	if (s10c_unpacked_len(body) > S10C_CARRIER_MAX_RX)
		return false;
	*out_len = s10c_unpack7(m + 6, body, out);
	return true;
}

/* ---- TE requests and replies ---- */

/* S-10c.080, S-10c.081, S-10c.082 */
void s10c_te_parse(const uint8_t *m, size_t n, struct s10c_te_req *r)
{
	uint8_t flags = m[6];
	size_t start = (flags & 0x40u) ? 9u : 10u;	/* reply form: status */

	r->cmd = m[8];
	r->has_id = (flags & 0x20u) != 0;
	r->id = r->has_id ? (uint16_t)(((flags & 0x1Fu) << 7) | (m[7] & 0x7Fu)) :
			    0;
	r->packed = m + start;
	r->packed_len = n - 1u - start;
	r->overflow = s10c_unpacked_len(r->packed_len) > S10C_TE_MAX_PAYLOAD;
}

/* S-10c.080 */
size_t s10c_te_reply_build(const struct wb_device *dev,
			   const struct s10c_te_req *req, uint8_t status,
			   const uint8_t *payload, size_t n, uint8_t *out)
{
	size_t o = 0;

	for (size_t i = 0; i < sizeof(te_head); i++)
		out[o++] = te_head[i];
	out[o++] = dev->ident.midi_byte;
	out[o++] = 0x40;
	out[o++] = req->has_id ?
		   (uint8_t)(0x20u | (((unsigned)req->id >> 7) & 0x1Fu)) : 0x00;
	out[o++] = req->has_id ? (uint8_t)(req->id & 0x7Fu) : 0x00;
	out[o++] = req->cmd;
	out[o++] = status;
	o += s10c_pack7(payload, n, out + o);
	out[o++] = 0xF7;
	return o;
}

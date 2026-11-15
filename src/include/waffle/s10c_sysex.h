/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_S10C_SYSEX_H
#define WAFFLE_S10C_SYSEX_H

/*
 * S-10c sections 1-3 and 8's message form: the SysEx forms on the USB MIDI
 * link. 8-to-7 packing, USB-MIDI 1.0 event packets, classification of a
 * received message, the identity reply, the carrier of the S-06 byte stream
 * and the TE request/reply form. The USB MIDI endpoint and the S-06
 * encoder and receiver are reached through callbacks.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- packing (S-10c.001, S-10c.002) ---- */

/* n + ceil(n / 7) */
size_t s10c_packed_len(size_t n);
/* m - ceil(m / 8) */
size_t s10c_unpacked_len(size_t m);

/* Packs `n` bytes into `out`, which holds s10c_packed_len(n) bytes. */
size_t s10c_pack7(const uint8_t *in, size_t n, uint8_t *out);
/* Unpacks `m` bytes into `out`, which holds s10c_unpacked_len(m) bytes. */
size_t s10c_unpack7(const uint8_t *in, size_t m, uint8_t *out);

/* ---- USB-MIDI event packets (S-10c.003-S-10c.006) ---- */

#define S10C_SYSEX_MAX_SENT 288u	/* S-10c.005 */
#define S10C_SYSEX_MAX_RECEIVED 8192u	/* S-10c.006 */

typedef void (*s10c_packet_fn)(void *ctx, const uint8_t packet[4]);
typedef void (*s10c_sysex_fn)(void *ctx, const uint8_t *msg, size_t n);

/* Sends one whole SysEx message as event packets on cable 0 (S-10c.003). */
void s10c_usbmidi_send(const uint8_t *msg, size_t n, s10c_packet_fn emit,
		       void *ctx);

/* Reassembly of received messages (S-10c.004, S-10c.006). */
struct s10c_midi_rx {
	size_t len;
	bool overlong;
	uint8_t buf[S10C_SYSEX_MAX_RECEIVED];
};

void s10c_midi_rx_init(struct s10c_midi_rx *rx);

/*
 * Takes one received event packet. Returns the length of a complete message
 * (F0 ... F7, at most 8192 bytes) now in rx->buf, valid until the next call;
 * 0 when no message completed.
 */
size_t s10c_midi_rx_packet(struct s10c_midi_rx *rx, const uint8_t packet[4]);

/* ---- what a received message is (S-10c.007, S-10c.010, S-10c.081) ---- */

enum s10c_msg_class {
	S10C_MSG_IGNORED,
	S10C_MSG_TE,
	S10C_MSG_IDENTITY,
	S10C_MSG_CARRIER,
};

enum s10c_msg_class s10c_classify(const uint8_t *m, size_t n);

/* The identity reply (S-10c.011). */
#define S10C_IDENTITY_REPLY_LEN 17u
extern const uint8_t s10c_identity_reply[S10C_IDENTITY_REPLY_LEN];

/* ---- the S-06 carrier (S-10c.020-S-10c.022) ---- */

#define S10C_CARRIER_MAX_STREAM 245u	/* stream bytes per message sent */
#define S10C_CARRIER_MAX_RX 512u	/* stream bytes per message received */

/* Builds one carrier message for `n` (at most 245) stream bytes into `out`,
 * which holds 7 + s10c_packed_len(n) bytes. Returns its length. */
size_t s10c_carrier_build(const uint8_t *stream, size_t n, uint8_t *out);

/* The carrier messages for one frame's encoded bytes, delimiter included
 * (S-10c.022): one message when they number at most 245, else consecutive
 * messages of at most 245 stream bytes each. */
void s10c_carrier_send_frame(const uint8_t *encoded, size_t n,
			     s10c_sysex_fn emit, void *ctx);

/* The stream bytes of a received carrier message into `out` (512 bytes).
 * Returns false when the message is ignored whole (S-10c.021). */
bool s10c_carrier_stream(const uint8_t *m, size_t n, uint8_t *out,
			 size_t *out_len);

/* ---- TE requests and replies (S-10c.080, S-10c.081) ---- */

#define S10C_TE_MAX_PAYLOAD 4096u	/* S-10c.082 */

struct s10c_te_req {
	uint8_t cmd;
	bool has_id;
	uint16_t id;			/* 12 bits */
	const uint8_t *packed;		/* the packed payload */
	size_t packed_len;
	bool overflow;			/* unpacks to more than 4096 bytes */
};

/* The fields of a message classified S10C_MSG_TE. */
void s10c_te_parse(const uint8_t *m, size_t n, struct s10c_te_req *r);

/* Builds a TE reply into `out`, which holds 11 + s10c_packed_len(n)
 * bytes. Returns its length. */
size_t s10c_te_reply_build(const struct s10c_te_req *req, uint8_t status,
			   const uint8_t *payload, size_t n, uint8_t *out);

#endif /* WAFFLE_S10C_SYSEX_H */

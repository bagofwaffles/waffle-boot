/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's PSoC link (src/front/psoc/psoc.c) against a scripted PSoC on a
 * fake byte link and a fake clock that moves on each delay and each read:
 * S-06 frames against libwaffle's codec and receiver, S-07.020's greeting,
 * S-07.021's exit packet and wait, S-07.011's 100 ms, S-07.032's hold
 * against libwaffle's over keys.json, S-07.033's one key down against a
 * reading written out here, S-07.054's
 * charge wait, S-07.051's power-off and its exception for recovery's
 * in-place write (the hold), S-02.084's battery request, S-07.076's ping,
 * S-07.024 and S-07.055-S-07.057. On the OP-1F (S-18): the key reading
 * over its 58 keys and 8 bytes (S-07.032, S-07.033), S-07.046's LED forms
 * with the change rule, S-07.047's breathing against a reference written
 * out here, a power-off with no 0x40 {01}, and no PMIC pass-through id.
 */
#include <string.h>

#include "support/check.h"

#include "front/front.h"
#include "front/psoc/psoc.h"
#include "waffle/link/frame.h"
#include "waffle/link/rx.h"
#include "waffle/psoc/rules.h"
#include "waffle/util/crc.h"
#include "vectors/s07_vectors.h"

#define MS 1000u

/* ---- the scripted PSoC ---- */

struct item {			/* what the master wrote */
	uint64_t t;
	int raw;
	uint8_t route, seq, id, len;
	uint8_t d[256];
};

struct answer {
	int on;
	uint8_t id, len;
	uint8_t pl[FR_S06_MAX_PAYLOAD];
	uint32_t delay;
};

static struct fake {
	uint64_t now;
	/* PSoC to master: bytes and when each is there */
	uint8_t in[65536];
	uint64_t in_at[65536];
	size_t in_n, in_pos;
	/* master to PSoC */
	struct s06_rx rx;
	struct item out[1024];
	size_t nout;
	uint8_t wire[65536];
	size_t nwire;
	/* the script */
	unsigned greets;
	unsigned answer_greet_at;	/* 0x06 {40} after this greeting; 0 never */
	uint8_t answer_greet_byte;
	unsigned psoc_greet_at;		/* 0x05 from the PSoC after this one */
	uint32_t exit_greet_us;		/* 0x05 this long after the exit packet */
	uint8_t exit_greet_id;
	struct answer ans[256];		/* per request id */
	uint64_t exit_t;
	int exit_seen;
	unsigned poweroffs;
	size_t nout_at_poweroff;
	uint64_t switch_on_at;		/* a plain 0x30 from then on: 0x31 {10} */
} fk;

static const uint8_t exit_packet[7] = { 0x01, 0x3B, 0x00, 0x00, 0xC4, 0xFF, 0x17 };

static void psoc_at(uint64_t at, uint8_t route, uint8_t id, const uint8_t *pl,
		    size_t n)
{
	uint8_t w[S06_MAX_WIRE];
	size_t len = s06_frame_encode(route, 0x55, id, pl, n, wire_crc8_frame, w);

	for (size_t i = 0; i < len; i++) {
		fk.in[fk.in_n] = w[i];
		fk.in_at[fk.in_n++] = at;
	}
}

static void bytes_at(uint64_t at, const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		fk.in[fk.in_n] = b[i];
		fk.in_at[fk.in_n++] = at;
	}
}

static void react(const struct item *it)
{
	static const uint8_t hello[1] = { 0x01 };

	if (it->id == 0x05 && it->route == 0x14) {
		fk.greets++;
		if (fk.greets == fk.answer_greet_at) {
			uint8_t b = fk.answer_greet_byte ? fk.answer_greet_byte : 0x40;

			psoc_at(fk.now + 2 * MS, 0x41, 0x06, &b, 1);
		}
		if (fk.greets == fk.psoc_greet_at)
			psoc_at(fk.now + 2 * MS, 0x41, 0x05, hello, 1);
	}
	if (it->id == 0x30 && it->route == 0x14 && fk.switch_on_at != 0u &&
	    fk.now >= fk.switch_on_at) {
		static const uint8_t on[1] = { 0x10 };

		psoc_at(fk.now + 1 * MS, 0x41, 0x31, on, 1);
	}
	if ((it->route >> 4) >= 8 && fk.ans[it->id].on) {
		const struct answer *a = &fk.ans[it->id];

		psoc_at(fk.now + a->delay, s06_swap_route(it->route), a->id, a->pl, a->len);
	}
}

static size_t fk_read(void *ctx, uint8_t *buf, size_t max)
{
	size_t n = 0;

	(void)ctx;
	fk.now += 1;		/* a poll takes time */
	while (n < max && fk.in_pos < fk.in_n && fk.in_at[fk.in_pos] <= fk.now)
		buf[n++] = fk.in[fk.in_pos++];
	return n;
}

static size_t fk_write(void *ctx, const uint8_t *buf, size_t n)
{
	(void)ctx;
	memcpy(fk.wire + fk.nwire, buf, n);
	fk.nwire += n;
	if (n == sizeof exit_packet && memcmp(buf, exit_packet, n) == 0) {
		struct item *it = &fk.out[fk.nout++];

		memset(it, 0, sizeof *it);
		it->t = fk.now;
		it->raw = 1;
		it->len = (uint8_t)n;
		memcpy(it->d, buf, n);
		fk.exit_seen = 1;
		fk.exit_t = fk.now;
		if (fk.exit_greet_us) {
			const uint8_t pl[1] = { 0x40 };

			psoc_at(fk.now + fk.exit_greet_us, 0x41,
				fk.exit_greet_id ? fk.exit_greet_id : 0x05, pl, 1);
		}
		return n;
	}
	for (size_t i = 0; i < n; i++) {
		int ev = s06_rx_byte(&fk.rx, buf[i], wire_crc8_frame);

		if (ev == S06_RX_DELIVER) {
			struct item *it = &fk.out[fk.nout++];

			it->t = fk.now;
			it->raw = 0;
			it->route = fk.rx.buf[0];
			it->seq = fk.rx.buf[1];
			it->id = fk.rx.buf[2];
			it->len = fk.rx.buf[3];
			memcpy(it->d, fk.rx.buf + 4, it->len);
			react(it);
		} else if (ev != S06_RX_NONE) {
			fk.out[fk.nout].raw = 2;	/* anything else is wrong */
			fk.out[fk.nout++].t = fk.now;
		}
	}
	return n;
}

static bool fk_idle(void *ctx)
{
	(void)ctx;
	return true;
}

static uint64_t fk_now(void *ctx)
{
	(void)ctx;
	return fk.now;
}

static void fk_delay(void *ctx, uint32_t us)
{
	(void)ctx;
	fk.now += us;
}

static const struct fr_bytes fk_io = { fk_read, fk_write, fk_idle, NULL };
static const struct fr_clock fk_clock = { fk_now, fk_delay, NULL };
static struct fr_psoc psoc;

static void on_poweroff(void *ctx)
{
	(void)ctx;
	fk.poweroffs++;
	fk.nout_at_poweroff = fk.nout;
}

static void setup(void)
{
	memset(&fk, 0, sizeof fk);
	fk.now = 1000;
	s06_rx_init(&fk.rx, S06_ADDR_PSOC);
	fr_psoc_init(&psoc, &fr_dev_opxy, &fk_io, &fk_clock);
	psoc.on_poweroff = on_poweroff;
}

static void setup_dev(const struct fr_dev *dev)
{
	setup();
	fr_psoc_init(&psoc, dev, &fk_io, &fk_clock);
	psoc.on_poweroff = on_poweroff;
}

static void answer(uint8_t req, uint8_t id, const uint8_t *pl, size_t n,
		   uint32_t delay)
{
	struct answer *a = &fk.ans[req];

	a->on = 1;
	a->id = id;
	a->len = (uint8_t)n;
	if (n)
		memcpy(a->pl, pl, n);
	a->delay = delay;
}

static size_t count(uint8_t id, uint8_t route, size_t from)
{
	size_t n = 0;

	for (size_t i = from; i < fk.nout; i++)
		if (!fk.out[i].raw && fk.out[i].id == id && fk.out[i].route == route)
			n++;
	return n;
}

static bool is(const struct item *it, uint8_t route, uint8_t id,
	       const uint8_t *pl, size_t n)
{
	return !it->raw && it->route == route && it->id == id && it->len == n &&
	       (n == 0 || memcmp(it->d, pl, n) == 0);
}

static void no_bad_bytes(void)
{
	for (size_t i = 0; i < fk.nout; i++)
		CHECK(fk.out[i].raw != 2);
}

/* ---- S-06 ---- */

static void payload(uint8_t *p, size_t n, unsigned kind)
{
	for (size_t i = 0; i < n; i++) {
		switch (kind) {
		case 0: p[i] = 0x00; break;
		case 1: p[i] = 0xFF; break;
		case 2: p[i] = (uint8_t)(i % 7u == 0 ? 0 : 0xFF); break;
		case 3: p[i] = (uint8_t)(i * 37u + 11u); break;
		default: p[i] = (uint8_t)(i < 253u ? 0xFF : 0x00); break;
		}
	}
}

static void test_encode(void)
{
	static const uint8_t check[] = "123456789";
	static const uint8_t ids[] = { 0x00, 0x05, 0x41, 0x97, 0xFF };
	static const uint8_t routes[] = { 0x14, 0x84, 0x41, 0x00 };
	static const size_t lens[] = { 0, 1, 2, 3, 7, 200, 248, 249 };
	uint8_t pl[FR_S06_MAX_PAYLOAD + 2], a[300], b[300];

	CHECK(fr_s06_crc8(check, 9) == 0xD8u);	/* S-06.003 */
	for (size_t i = 0; i < 300; i++) {
		payload(pl, sizeof pl, 3);
		CHECK(fr_s06_crc8(pl, i % 250u) == wire_crc8_frame(pl, i % 250u));
	}
	for (unsigned kind = 0; kind < 5; kind++)
		for (size_t l = 0; l < ARRAY_LEN(lens); l++)
			for (size_t i = 0; i < ARRAY_LEN(ids); i++) {
				const uint8_t r = routes[(i + l) % ARRAY_LEN(routes)];
				size_t na, nb;

				payload(pl, lens[l], kind);
				na = fr_s06_encode(r, (uint8_t)(i * 50u + l), ids[i], pl, lens[l], a);
				nb = s06_frame_encode(r, (uint8_t)(i * 50u + l), ids[i], pl, lens[l],
						      wire_crc8_frame, b);
				CHECK(na == nb && na != 0);
				CHECK(memcmp(a, b, nb) == 0);
			}
	/* S-06.002: never cut */
	CHECK(fr_s06_encode(0x14, 0, 1, pl, 250, a) == 0);
	CHECK(s06_frame_encode(0x14, 0, 1, pl, 250, wire_crc8_frame, b) == 0);
}

/* Sends: byte for byte libwaffle's, sequence numbers from 0 (S-06.020). */
static void test_send(void)
{
	uint8_t pl[FR_S06_MAX_PAYLOAD], w[S06_MAX_WIRE];
	size_t at = 0;

	setup();
	for (unsigned k = 0; k < 6; k++) {
		const size_t n = k == 5 ? FR_S06_MAX_PAYLOAD : k * 40u;
		size_t len;

		payload(pl, n, k % 5u);
		fr_psoc_send(&psoc, (uint8_t)(0x30 + k), pl, n);
		len = s06_frame_encode(0x14, (uint8_t)k, (uint8_t)(0x30 + k), pl, n,
				       wire_crc8_frame, w);
		CHECK(fk.nwire == at + len);
		CHECK(memcmp(fk.wire + at, w, len) == 0);
		at += len;
	}
	fr_psoc_send(&psoc, 0x30, pl, 250);	/* not sent, no number used */
	CHECK(fk.nwire == at);
	fr_psoc_send(&psoc, 0x30, NULL, 0);
	CHECK(fk.nout == 7 && fk.out[6].seq == 6);
	no_bad_bytes();
}

/* Receive: replies of every shape decode back; junk, a frame for another
 * node, a bad check and a cut frame before them change nothing. */
static void test_receive(void)
{
	uint8_t pl[FR_S06_MAX_PAYLOAD], w[S06_MAX_WIRE];
	static const uint8_t junk[] = { 0x11, 0x22, 0x00, 0x00, 0x05, 0x41, 0x00 };
	struct fr_frame r;

	for (unsigned kind = 0; kind < 5; kind++) {
		for (size_t n = 0; n <= FR_S06_MAX_PAYLOAD; n += (n < 8 ? 1 : 61)) {
			const size_t len = n > FR_S06_MAX_PAYLOAD ? FR_S06_MAX_PAYLOAD : n;
			size_t wl;

			setup();
			payload(pl, len, kind);
			bytes_at(fk.now, junk, sizeof junk);
			/* for node 2: skipped */
			psoc_at(fk.now, 0x42, 0x09, pl, len);
			/* a bad check */
			wl = s06_frame_encode(0x48, 1, 0x0A, pl, len, wire_crc8_frame, w);
			w[wl - 2] ^= 0x01;
			if (w[wl - 2] == 0)
				w[wl - 2] = 0x02;
			bytes_at(fk.now, w, wl);
			/* cut by a delimiter */
			bytes_at(fk.now, w, wl / 2);
			bytes_at(fk.now, (const uint8_t[]){ 0x00 }, 1);
			answer(0x09, 0x0A, pl, len, 3 * MS);
			CHECK(fr_psoc_request(&psoc, 0x09, NULL, 0, 100 * MS, &r) == 1);
			CHECK(r.id == 0x0A && r.route == 0x48 && r.len == len);
			CHECK(memcmp(r.payload, pl, len) == 0);
			CHECK(fk.out[0].route == 0x84);	/* S-06.030: slot 0 */
		}
		/* the maximum, once more, exactly */
		if (kind == 4) {
			setup();
			payload(pl, FR_S06_MAX_PAYLOAD, kind);
			answer(0x09, 0x0A, pl, FR_S06_MAX_PAYLOAD, 1 * MS);
			CHECK(fr_psoc_request(&psoc, 0x09, NULL, 0, 100 * MS, &r) == 1);
			CHECK(r.len == FR_S06_MAX_PAYLOAD &&
			      memcmp(r.payload, pl, FR_S06_MAX_PAYLOAD) == 0);
		}
	}
	no_bad_bytes();
}

/* ---- S-07.020: the greeting ---- */

static void test_greeting(void)
{
	static const uint8_t hello[2] = { 0x01, 0x04 }, ok[1] = { 0x40 };

	for (unsigned n = 0; n <= 10; n++) {
		const unsigned want = n ? n : 10;
		uint64_t first = 0, last = 0;
		unsigned seen = 0;

		setup();
		fk.answer_greet_at = n;
		CHECK(fr_psoc_greet(&psoc) == (n ? 1 : 0));
		CHECK(psoc.greeted == (n != 0));
		for (size_t i = 0; i < fk.nout; i++) {
			if (!is(&fk.out[i], 0x14, 0x05, hello, 2))
				continue;
			if (seen)
				CHECK(fk.out[i].t - last >= 10 * MS);
			else
				first = fk.out[i].t;
			last = fk.out[i].t;
			seen++;
		}
		CHECK(seen == want);
		CHECK(fk.nout == want);		/* nothing else sent */
		if (!n)
			CHECK(last - first <= 200 * MS);	/* ten within 200 ms */
		/* ended as soon as it was greeted */
		if (n)
			CHECK(fk.now - last < 3 * MS);
		CHECK(fr_psoc_sleep(&psoc, 50 * MS) == FR_PSOC_OK);
		CHECK(fk.nout == want);
	}
	/* a 0x06 with another byte does not greet */
	setup();
	fk.answer_greet_at = 2;
	fk.answer_greet_byte = 0x41;
	CHECK(fr_psoc_greet(&psoc) == 0);
	CHECK(count(0x05, 0x14, 0) == 10);
	/* the PSoC's own 0x05 greets, answered 0x06 {40} alone (S-07.024) */
	setup();
	fk.psoc_greet_at = 3;
	CHECK(fr_psoc_greet(&psoc) == 1);
	CHECK(count(0x05, 0x14, 0) == 3);
	CHECK(fk.nout == 4 && is(&fk.out[3], 0x14, 0x06, ok, 1));
	no_bad_bytes();
}

/* ---- S-07.021: the exit packet and the wait ---- */

static void test_greet_wait(void)
{
	static const uint8_t ok[1] = { 0x40 };
	uint64_t t;

	/* silent: 1.5 s, nothing sent after the 7 raw bytes */
	setup();
	CHECK(fr_psoc_greet(&psoc) == 0);
	CHECK(fr_psoc_greet_wait(&psoc, 1500 * MS) == 0);
	CHECK(fk.nout == 11 && fk.out[10].raw == 1);
	CHECK(fk.nwire >= 7 && memcmp(fk.wire + fk.nwire - 7, exit_packet, 7) == 0);
	CHECK(fk.now - fk.exit_t >= 1500 * MS && fk.now - fk.exit_t < 1501 * MS);

	/* a PSoC that greets 300 ms in: answered, and the wait ends */
	setup();
	fk.exit_greet_us = 300 * MS;
	CHECK(fr_psoc_greet(&psoc) == 0);
	t = fk.now;
	CHECK(fr_psoc_greet_wait(&psoc, 1500 * MS) == 1);
	CHECK(fk.exit_seen && fk.exit_t - t < 1 * MS);
	CHECK(fk.now >= fk.exit_t + 300 * MS && fk.now < fk.exit_t + 303 * MS);
	CHECK(fk.nout == 12 && fk.out[10].raw == 1);
	CHECK(is(&fk.out[11], 0x14, 0x06, ok, 1));

	/* or with 0x06 {40}: ended, nothing sent */
	setup();
	fk.exit_greet_us = 300 * MS;
	fk.exit_greet_id = 0x06;
	CHECK(fr_psoc_greet(&psoc) == 0);
	CHECK(fr_psoc_greet_wait(&psoc, 1500 * MS) == 1);
	CHECK(fk.now < fk.exit_t + 303 * MS);
	CHECK(fk.nout == 11);
	no_bad_bytes();
}

/* ---- S-07.011, S-07.032, S-07.033 ---- */

static void test_requests(void)
{
	static const uint8_t m1[8] = { 0x40, 0, 0, 0, 0, 0, 0, 0 };
	uint64_t t;

	/* no reply: 100 ms */
	setup();
	t = fk.now;
	CHECK(fr_psoc_key_status(&psoc) == FR_PSOC_OK);
	CHECK(!psoc.keys_replied && psoc.keys_len == 0);
	CHECK(fk.now - t >= 100 * MS && fk.now - t < 101 * MS);
	CHECK(fk.nout == 1 && fk.out[0].route == 0x84 && fk.out[0].id == 0x41 &&
	      fk.out[0].len == 0);

	/* answered at 80 ms: used, its payload kept, whatever its id */
	setup();
	answer(0x41, 0x05, m1, sizeof m1, 80 * MS);
	t = fk.now;
	CHECK(fr_psoc_key_status(&psoc) == FR_PSOC_OK);
	CHECK(psoc.keys_replied && psoc.keys_len == 8);
	CHECK(memcmp(psoc.keys, m1, 8) == 0);
	CHECK(fk.now - t < 81 * MS);
	CHECK(fr_keys_one(&fr_dev_opxy, psoc.keys, psoc.keys_len) == FR_KEY_1);
	CHECK(!fr_keys_hold(&fr_dev_opxy, psoc.keys, psoc.keys_len));
	CHECK(fk.nout == 1);		/* a reply is never a greeting */

	/* answered at 150 ms: not used */
	setup();
	answer(0x41, 0x42, m1, sizeof m1, 150 * MS);
	CHECK(fr_psoc_key_status(&psoc) == FR_PSOC_OK);
	CHECK(!psoc.keys_replied);
	CHECK(fr_psoc_sleep(&psoc, 100 * MS) == FR_PSOC_OK);
	CHECK(fk.nout == 1);
	no_bad_bytes();
}

/* S-07.012: a late reply to the waited 0x30 or 0x36 (0x31, 0x32, 0x37) that
 * comes while the key status is pending is no key status: it is handled as
 * the power message it is, and the key status's own reply is used. With no
 * such reply outstanding, any id completes the pending request. */
static void test_late_reply(void)
{
	static const uint8_t on[1] = { 0x10 }, m4[4] = { 0x00, 0x02, 0x00, 0x00 },
			     batt[2] = { 0x01, 0x50 };

	/* the 0x30's reply 150 ms after it, 50 ms into the key status */
	setup();
	answer(0x36, 0x37, batt, sizeof batt, 5 * MS);
	answer(0x30, 0x31, on, sizeof on, 150 * MS);
	answer(0x41, 0x05, m4, sizeof m4, 80 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	CHECK(!psoc.state_known);
	CHECK(fr_psoc_key_status(&psoc) == FR_PSOC_OK);
	CHECK(psoc.keys_replied && psoc.keys_len == sizeof m4 && memcmp(psoc.keys, m4, 4) == 0);
	CHECK(fr_keys_one(&fr_dev_opxy, psoc.keys, psoc.keys_len) == FR_KEY_4);
	CHECK(psoc.state_known && psoc.state == FR_POWER_ON);

	/* after an unanswered waited 0x30 and 0x36, a 0x37 and a 0x32 while
	 * the key status waits: neither is taken, each counts as the late
	 * reply it is, and the rule is then version 7's again */
	setup();
	CHECK(fr_psoc_request(&psoc, 0x30, NULL, 0, 100 * MS, NULL) == 0);
	CHECK(fr_psoc_request(&psoc, 0x36, NULL, 0, 100 * MS, NULL) == 0);
	CHECK(psoc.late_30 == 1u && psoc.late_36 == 1u);
	answer(0x41, 0x05, m4, sizeof m4, 60 * MS);
	psoc_at(fk.now + 20 * MS, 0x48, 0x37, batt, sizeof batt);
	psoc_at(fk.now + 30 * MS, 0x48, 0x32, on, sizeof on);
	CHECK(fr_psoc_key_status(&psoc) == FR_PSOC_OK);
	CHECK(psoc.keys_replied && memcmp(psoc.keys, m4, 4) == 0);
	CHECK(psoc.batt_len == sizeof batt && psoc.state_known && psoc.state == FR_POWER_ON);
	CHECK(psoc.late_30 == 0u && psoc.late_36 == 0u);

	/* with no waited request unanswered (none, or each answered), a key
	 * status answered with 0x31, 0x32 or 0x37 is taken as version 7 took
	 * it: every id completes the request by its slot (S-07.012), the
	 * factory PSoC's 0x42 (S-07.032) as any other */
	for (unsigned k = 0; k < 3u; k++) {
		static const uint8_t ids[3] = { 0x31, 0x32, 0x37 };

		setup();
		if (k == 2u) {
			answer(0x30, 0x31, on, sizeof on, 5 * MS);
			answer(0x36, 0x37, batt, sizeof batt, 5 * MS);
			CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
			fk.ans[0x30].on = fk.ans[0x36].on = 0;
		}
		answer(0x41, ids[k], m4, sizeof m4, 40 * MS);
		CHECK(fr_psoc_key_status(&psoc) == FR_PSOC_OK);
		CHECK(psoc.keys_replied && psoc.keys_len == sizeof m4 && memcmp(psoc.keys, m4, 4) == 0);
		CHECK(fr_keys_one(&fr_dev_opxy, psoc.keys, psoc.keys_len) == FR_KEY_4);
		CHECK(k == 2u ? psoc.state == FR_POWER_ON && psoc.batt_len == sizeof batt :
				!psoc.state_known && psoc.batt_len == 0u);
		CHECK(fk.poweroffs == 0u);
	}

	/* the waited 0x30 still takes 0x31 and 0x32, the 0x36 0x37, and,
	 * with the other's reply outstanding, neither the other's */
	setup();
	CHECK(fr_psoc_request(&psoc, 0x36, NULL, 0, 100 * MS, NULL) == 0);
	psoc_at(fk.now + 10 * MS, 0x48, 0x37, batt, sizeof batt);
	answer(0x30, 0x32, on, sizeof on, 20 * MS);
	CHECK(fr_psoc_request(&psoc, 0x30, NULL, 0, 100 * MS, NULL) == 1);
	CHECK(psoc.batt_len == sizeof batt);
	setup();
	CHECK(fr_psoc_request(&psoc, 0x30, NULL, 0, 100 * MS, NULL) == 0);
	psoc_at(fk.now + 10 * MS, 0x48, 0x31, on, sizeof on);
	answer(0x36, 0x37, batt, sizeof batt, 20 * MS);
	CHECK(fr_psoc_battery(&psoc) == FR_PSOC_OK);
	CHECK(psoc.batt_len == sizeof batt && psoc.state_known && psoc.state == FR_POWER_ON);
	no_bad_bytes();
}

/* S-07.033's reading, written out: key k is bit k & 7 of byte k >> 3, keys
 * 0x00-0x3F, precomp (0x17) ignored, missing bytes 00. */
static unsigned ref_one(const uint8_t *p, size_t n)
{
	unsigned key = FR_KEYS_NONE;

	if (p == NULL)
		return FR_KEYS_UNREAD;
	for (unsigned k = 0; k < 72u; k++) {
		if (k == 0x17u || k >> 3 >= n || ((p[k >> 3] >> (k & 7u)) & 1u) == 0u)
			continue;
		if (key != FR_KEYS_NONE)
			return FR_KEYS_MANY;
		key = k;
	}
	return key;
}

static void key_compare(const uint8_t *p, size_t n)
{
	struct s07_key_status ks;

	s07_key_status_decode(&wb_device_opxy, p, n, &ks);
	CHECK(fr_keys_hold(&fr_dev_opxy, p, n) == ks.hold);
	CHECK(fr_keys_one(&fr_dev_opxy, p, n) == ref_one(p, n));
	/* S-07.033: Power + M4 is the one chord, as the main image reads it */
	CHECK((fr_keys_one(&fr_dev_opxy, p, n) == FR_KEY_4) == (s07_chord_digit(&wb_device_opxy, p, n) == 4u));
}

/* S-07.032's hold bits (byte 3 bit 0 or byte 1 bit 5); the debug and
 * precomp bits alone are not hold; the hold key with a digit key is hold
 * and, as a key reading, two keys; a short reply reads 00. */
static void test_keys_hold_bits(void)
{
	static const uint8_t b3[4] = { 0, 0, 0, 0x01 }, b1[2] = { 0, 0x20 },
			     dbg[4] = { 0, 0x0C, 0, 0 }, pre[4] = { 0, 0, 0x80, 0 },
			     m1[4] = { 0x40, 0, 0, 0x01 }, m4[2] = { 0, 0x22 },
			     other[4] = { 0, 0xDF, 0xFF, 0xFE };

	CHECK(fr_keys_hold(&fr_dev_opxy, b3, sizeof b3) && fr_keys_one(&fr_dev_opxy, b3, sizeof b3) == 0x18u);
	CHECK(fr_keys_hold(&fr_dev_opxy, b1, sizeof b1) && fr_keys_one(&fr_dev_opxy, b1, sizeof b1) == 0x0Du);
	CHECK(!fr_keys_hold(&fr_dev_opxy, dbg, sizeof dbg) && !fr_keys_hold(&fr_dev_opxy, pre, sizeof pre));
	CHECK(fr_keys_one(&fr_dev_opxy, pre, sizeof pre) == FR_KEYS_NONE);
	CHECK(fr_keys_hold(&fr_dev_opxy, m1, sizeof m1) && fr_keys_one(&fr_dev_opxy, m1, sizeof m1) == FR_KEYS_MANY);
	CHECK(fr_keys_hold(&fr_dev_opxy, m4, sizeof m4) && fr_keys_one(&fr_dev_opxy, m4, sizeof m4) == FR_KEYS_MANY);
	CHECK(!fr_keys_hold(&fr_dev_opxy, other, sizeof other));
	CHECK(!fr_keys_hold(&fr_dev_opxy, b3, 3u));		/* byte 3 missing */
}

/* fr_keys_one(): the keys recovery reads, each alone and with precomp; no
 * reply; nothing down; two keys. */
static void test_keys_one(void)
{
	static const uint8_t keys[] = { FR_KEY_1, FR_KEY_2, FR_KEY_3, FR_KEY_4, FR_KEY_8 };
	uint8_t p[10];

	CHECK(fr_keys_one(&fr_dev_opxy, NULL, 0) == FR_KEYS_UNREAD);
	CHECK(fr_keys_one(&fr_dev_opxy, NULL, 9) == FR_KEYS_UNREAD);
	memset(p, 0, sizeof p);
	CHECK(fr_keys_one(&fr_dev_opxy, p, 0) == FR_KEYS_NONE);
	CHECK(fr_keys_one(&fr_dev_opxy, p, sizeof p) == FR_KEYS_NONE);
	for (unsigned i = 0; i < ARRAY_LEN(keys); i++) {
		memset(p, 0, sizeof p);
		p[keys[i] >> 3] = (uint8_t)(1u << (keys[i] & 7u));
		CHECK(fr_keys_one(&fr_dev_opxy, p, sizeof p) == keys[i]);
		p[2] |= 0x80u;		/* precomp */
		CHECK(fr_keys_one(&fr_dev_opxy, p, sizeof p) == keys[i]);
		CHECK(fr_keys_one(&fr_dev_opxy, p, (keys[i] >> 3) + 0u) == FR_KEYS_NONE);	/* cut short */
		p[0] |= 0x01u;
		CHECK(fr_keys_one(&fr_dev_opxy, p, sizeof p) == FR_KEYS_MANY);
	}
	/* M1, M2 and M4 as S-07.033 numbers them: byte 0 bits 6 and 7, byte 1
	 * bit 1 */
	memset(p, 0, sizeof p);
	p[0] = 0x40u;
	CHECK(fr_keys_one(&fr_dev_opxy, p, 2) == FR_KEY_1);
	p[0] = 0x80u;
	CHECK(fr_keys_one(&fr_dev_opxy, p, 2) == FR_KEY_2);
	p[0] = 0x00u;
	p[1] = 0x02u;
	CHECK(fr_keys_one(&fr_dev_opxy, p, 2) == FR_KEY_4);
	/* byte 8, keys 0x40-0x47 (steps 9 to 16), is read; byte 9 is not */
	memset(p, 0, sizeof p);
	p[8] = 0x01u;
	CHECK(fr_keys_one(&fr_dev_opxy, p, sizeof p) == 0x40u);
	p[8] = 0x80u;
	CHECK(fr_keys_one(&fr_dev_opxy, p, 9) == 0x47u);
	p[8] = 0xFFu;
	CHECK(fr_keys_one(&fr_dev_opxy, p, sizeof p) == FR_KEYS_MANY);
	p[8] = 0x00u;
	p[9] = 0xFFu;
	CHECK(fr_keys_one(&fr_dev_opxy, p, sizeof p) == FR_KEYS_NONE);
	/* key 1 or key 4 with a step key: many keys */
	for (unsigned k = 0x40u; k < 0x48u; k++) {
		memset(p, 0, sizeof p);
		p[8] = (uint8_t)(1u << (k & 7u));
		p[0] = 0x40u;
		CHECK(fr_keys_one(&fr_dev_opxy, p, sizeof p) == FR_KEYS_MANY);
		p[0] = 0x00u;
		p[1] = 0x02u;
		CHECK(fr_keys_one(&fr_dev_opxy, p, sizeof p) == FR_KEYS_MANY);
		CHECK(s07_chord_digit(&wb_device_opxy, p, sizeof p) == 0u);
	}
	/* a short reply (S-07.079): the bytes it does not carry read 00 */
	memset(p, 0, sizeof p);
	p[1] = 0x02u;
	CHECK(fr_keys_one(&fr_dev_opxy, p, 8) == FR_KEY_4 && s07_chord_digit(&wb_device_opxy, p, 8) == 4u);
	p[7] = 0x80u;			/* key 0x3F */
	CHECK(fr_keys_one(&fr_dev_opxy, p, 8) == FR_KEYS_MANY && s07_chord_digit(&wb_device_opxy, p, 8) == 0u);
	p[7] = 0x00u;
	p[8] = 0x01u;			/* not carried by 8 bytes */
	CHECK(fr_keys_one(&fr_dev_opxy, p, 8) == FR_KEY_4 && s07_chord_digit(&wb_device_opxy, p, 8) == 4u);
	CHECK(fr_keys_one(&fr_dev_opxy, p, 9) == FR_KEYS_MANY && s07_chord_digit(&wb_device_opxy, p, 9) == 0u);
}

static void test_keys(void)
{
	uint8_t p[10];
	uint32_t x = 12345;

	for (size_t i = 0; i < s07v_status_count; i++) {
		const struct s07v_status *v = &s07v_status[i];

		key_compare(v->none ? NULL : v->p, v->len);
		CHECK(fr_keys_hold(&fr_dev_opxy, v->none ? NULL : v->p, v->len) == (v->hold != 0));
	}
	/* chord_status.json: Power + M4 alone is the chord; keys 1 and 2 are none */
	for (size_t i = 0; i < s07v_chords_count; i++) {
		const struct s07v_chord *v = &s07v_chords[i];

		key_compare(v->none ? NULL : v->p, v->len);
		CHECK((fr_keys_one(&fr_dev_opxy, v->none ? NULL : v->p, v->len) == FR_KEY_4) == (v->chord == 4));
	}
	/* every one and two bits of ten bytes, at every length */
	for (unsigned a = 0; a < 80; a++)
		for (unsigned b = a; b < 80; b++)
			for (size_t n = 0; n <= 10; n += 2) {
				memset(p, 0, sizeof p);
				p[a >> 3] = (uint8_t)(p[a >> 3] | 1u << (a & 7u));
				p[b >> 3] = (uint8_t)(p[b >> 3] | 1u << (b & 7u));
				key_compare(p, n);
			}
	for (unsigned k = 0; k < 2000; k++) {
		for (size_t i = 0; i < sizeof p; i++) {
			x = x * 1103515245u + 12345u;
			p[i] = (uint8_t)((x >> 16) & (k & 1u ? 0x41u : 0xFFu));
		}
		key_compare(p, k % 11u);
	}
	key_compare(NULL, 0);
}

/* ---- S-07.054: the first requests and the charge wait ---- */

static void test_charge_wait(void)
{
	static const uint8_t batt[7] = { 0x02, 0x32, 0x19, 0x00, 0x64, 0x0F, 0xA0 },
			     s16[1] = { 0x16 }, s10[1] = { 0x10 },
			     red[4] = { 0x03, 0xC8, 0x00, 0x00 }, lim[2] = { 0x00, 0x32 };
	uint64_t t0, prev = 0;
	size_t from, rounds = 0;

	setup();
	answer(0x36, 0x37, batt, 7, 5 * MS);
	answer(0x30, 0x31, s16, 1, 5 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	CHECK(fk.out[0].id == 0x36 && fk.out[0].route == 0x84);
	CHECK(fk.out[1].id == 0x30 && fk.out[1].route == 0x84);
	CHECK(psoc.state_known && psoc.state == FR_POWER_CHARGE);
	CHECK(psoc.batt_len == 7 && psoc.batt[1] == 0x32);
	CHECK(count(0x40, 0x14, 0) >= 1);	/* S-07.042: forced, red */
	CHECK(is(&fk.out[fk.nout - 1], 0x14, 0x40, red, 4));
	from = fk.nout;
	t0 = fk.now;
	psoc_at(t0 + 3500 * MS, 0x41, 0x31, s10, 1);
	CHECK(fr_psoc_charge_wait(&psoc) == FR_PSOC_OK);
	CHECK(fk.now >= t0 + 3500 * MS && fk.now < t0 + 3520 * MS);
	CHECK(psoc.state == FR_POWER_ON && !psoc.off);
	CHECK(psoc.target_ma == 0 && psoc.applied_ma == 0);
	for (size_t i = from; i < fk.nout; i++) {
		if (!is(&fk.out[i], 0x14, 0x36, NULL, 0))
			continue;
		/* 0x36 then 0x30, plain, then the VBUS update */
		CHECK(is(&fk.out[i + 1], 0x14, 0x30, NULL, 0));
		CHECK(fk.out[i + 2].id == 0x33 && fk.out[i + 2].route == 0x84);
		if (rounds)
			CHECK(fk.out[i].t - prev > 1000 * MS &&
			      fk.out[i].t - prev < 1100 * MS);
		prev = fk.out[i].t;
		rounds++;
	}
	CHECK(rounds == 4);
	CHECK(count(0x34, 0x14, from) >= 1);	/* 500 mA: {00 32} */
	for (size_t i = from; i < fk.nout; i++)
		if (fk.out[i].id == 0x34)
			CHECK(is(&fk.out[i], 0x14, 0x34, lim, 2));
	CHECK(count(0x32, 0x14, 0) == 0);	/* never acknowledged */
	CHECK(fk.poweroffs == 0);
	no_bad_bytes();
}

/* ---- what waits while a request does ---- */

static unsigned g_served;

static void count_serve(void)
{
	g_served++;
}

/* While the key status waits, the PSoC sends six key events (0x42, which
 * the front does not serve), then 0x31 {14}: the events are not kept, the
 * power message is, and the power-off runs once the request is over. Six
 * pings fill the inbox and a 0x31 {14} after them is kept all the same.
 * Under the hold, 0x14 10 14 16 10 14 kept in order: the kept power-off
 * stands. And the long loops serve the link (fr_serve()). */
static void test_kept(void)
{
	static const uint8_t ev[9] = { 0x40 }, s10[1] = { 0x10 }, s14[1] = { 0x14 },
			     ping[2] = { 0xAA, 0xBB };
	static const uint8_t seq[6] = { 0x14, 0x10, 0x14, 0x16, 0x10, 0x14 };
	static uint8_t big[0x2800];
	struct fr_sha256 sh;

	setup();
	answer(0x30, 0x31, s10, 1, 5 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	for (unsigned i = 0; i < 6u; i++)
		psoc_at(fk.now + (10u + i) * MS, 0x41, 0x42, ev, sizeof ev);
	psoc_at(fk.now + 20 * MS, 0x41, 0x31, s14, 1);
	CHECK(fr_psoc_key_status(&psoc) == FR_PSOC_OFF);
	CHECK(fk.poweroffs == 1);

	setup();
	answer(0x30, 0x31, s10, 1, 5 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	for (unsigned i = 0; i < 6u; i++)
		psoc_at(fk.now + (10u + i) * MS, 0x41, 0x01, ping, sizeof ping);
	psoc_at(fk.now + 20 * MS, 0x41, 0x31, s14, 1);
	CHECK(fr_psoc_key_status(&psoc) == FR_PSOC_OFF);
	CHECK(fk.poweroffs == 1);

	setup();
	answer(0x30, 0x31, s10, 1, 5 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	CHECK(fr_psoc_hold(&psoc, true) == FR_PSOC_OK);
	for (unsigned i = 0; i < sizeof seq; i++)
		psoc_at(fk.now + (10u + i) * MS, 0x41, 0x31, &seq[i], 1);
	CHECK(fr_psoc_key_status(&psoc) == FR_PSOC_OK);
	CHECK(psoc.off_due && psoc.off_state == FR_POWER_OFF && psoc.state == FR_POWER_OFF);
	CHECK(fk.poweroffs == 0);
	CHECK(fr_psoc_hold(&psoc, false) == FR_PSOC_OFF);
	CHECK(fk.poweroffs == 1);

	/* S-07.042: in state 0x16 the charge LED is forced, and set after
	 * each message: three 0x31 {16} and two 0x37 while the key status
	 * waits give five LED messages */
	{
		static const uint8_t s16[1] = { 0x16 }, b2[2] = { 0x01, 0x10 };
		size_t from;

		setup();
		answer(0x30, 0x31, s16, 1, 5 * MS);
		CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
		CHECK(psoc.state == FR_POWER_CHARGE);
		from = fk.nout;
		for (unsigned i = 0; i < 3u; i++)
			psoc_at(fk.now + (10u + i) * MS, 0x41, 0x31, s16, 1);
		for (unsigned i = 0; i < 2u; i++)
			psoc_at(fk.now + (20u + i) * MS, 0x41, 0x37, b2, sizeof b2);
		CHECK(fr_psoc_key_status(&psoc) == FR_PSOC_OK);
		CHECK(count(0x40, 0x14, from) == 5u);
		/* and at the hand-off, a ping is answered with the power
		 * message (S-07.076's front's point) */
		from = fk.nout;
		psoc_at(fk.now + 1 * MS, 0x41, 0x01, ping, sizeof ping);
		psoc_at(fk.now + 2 * MS, 0x41, 0x31, s16, 1);
		fk.now += 3 * MS;
		CHECK(fr_psoc_poll(&psoc) == FR_PSOC_OK);
		CHECK(count(0x40, 0x14, from) == 1u && count(0x02, 0x14, from) == 1u);
	}

	/* fr_psoc_take(), as the long loops' hook calls it: twenty key events
	 * and then 0x31 {14} are taken in, sending nothing and handling
	 * nothing; the events are not kept, the power-off runs at the next
	 * poll */
	setup();
	answer(0x30, 0x31, s10, 1, 5 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	{
		const size_t n0 = fk.nout;

		for (unsigned i = 0; i < 20u; i++)
			psoc_at(fk.now + 1 * MS, 0x41, 0x42, ev, sizeof ev);
		psoc_at(fk.now + 1 * MS, 0x41, 0x31, s14, 1);
		fk.now += 2 * MS;
		for (unsigned i = 0; i < 40u; i++)
			fr_psoc_take(&psoc);
		CHECK(fk.nout == n0 && fk.poweroffs == 0);
		CHECK(psoc.in_count == 0u && psoc.n_states == 1u);
		CHECK(fr_psoc_poll(&psoc) == FR_PSOC_OFF && fk.poweroffs == 1);
	}

	g_served = 0;
	fr_serve_hook = count_serve;
	(void)fr_crc32(0, big, sizeof big);
	CHECK(g_served >= sizeof big / FR_SERVE_BYTES);
	g_served = 0;
	fr_sha256_init(&sh);
	fr_sha256_add(&sh, big, sizeof big);
	CHECK(g_served >= sizeof big / FR_SERVE_BYTES);
	fr_serve_hook = NULL;
	no_bad_bytes();
}

/* ---- S-07.051: the power-off ---- */

static void test_poweroff(void)
{
	static const uint8_t s10[1] = { 0x10 }, s14[1] = { 0x14 }, s16[1] = { 0x16 },
			     rep[3] = { 0x00, 0x00, 0x02 }, all_off[1] = { 0x01 };
	const uint8_t *states[2] = { s14, s16 };

	for (unsigned k = 0; k < 2; k++) {
		size_t n;

		setup();
		answer(0x30, 0x31, s10, 1, 5 * MS);
		CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
		CHECK(psoc.state == FR_POWER_ON);
		psoc_at(fk.now + 50 * MS, 0x41, 0x31, states[k], 1);
		CHECK(fr_psoc_sleep(&psoc, 200 * MS) == FR_PSOC_OFF);
		CHECK(fk.poweroffs == 1);
		n = fk.nout_at_poweroff;
		CHECK(fk.nout == n + 3);
		CHECK(is(&fk.out[n], 0x14, 0x51, rep, 3));
		CHECK(is(&fk.out[n + 1], 0x14, 0x40, all_off, 1));
		CHECK(is(&fk.out[n + 2], 0x14, 0x32, states[k], 1));
		/* then nothing */
		n = fk.nwire;
		fr_psoc_send(&psoc, 0x30, NULL, 0);
		fr_psoc_report(&psoc, FR_REPORT_HELD);
		fr_psoc_subscribe(&psoc);
		fr_psoc_reset(&psoc);
		fr_psoc_power_queries(&psoc);
		CHECK(fr_psoc_request(&psoc, 0x41, NULL, 0, 100 * MS, NULL) == FR_PSOC_OFF);
		CHECK(fr_psoc_poll(&psoc) == FR_PSOC_OFF);
		CHECK(fr_psoc_key_status(&psoc) == FR_PSOC_OFF);
		CHECK(fr_psoc_greet(&psoc) == FR_PSOC_OFF);
		CHECK(fr_psoc_charge_wait(&psoc) == FR_PSOC_OFF);
		CHECK(fr_psoc_service(&psoc) == FR_PSOC_OFF);
		CHECK(fk.nwire == n);
		CHECK(fk.poweroffs == 1);
	}
	/* as the reply to a waited 0x30 */
	setup();
	answer(0x30, 0x31, s14, 1, 5 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OFF);
	CHECK(fk.poweroffs == 1 && fk.out[fk.nout - 1].id == 0x32);
	no_bad_bytes();
}

/* ---- S-07.051's exception: the hold during an in-place write (S-02.084) ---- */

/* Nothing of a power-off since item `from`: no report, no LED clear, no
 * 0x32, and on_poweroff not called. */
static void no_poweroff_since(size_t from)
{
	CHECK(fk.poweroffs == 0);
	CHECK(!psoc.off);
	CHECK(count(0x32, 0x14, from) == 0);
	for (size_t i = from; i < fk.nout; i++) {
		CHECK(!(fk.out[i].id == 0x51 && fk.out[i].len == 3 && fk.out[i].d[0] == 0x00 &&
			fk.out[i].d[2] == 0x02));
		CHECK(!(fk.out[i].id == 0x40 && fk.out[i].len == 1 && fk.out[i].d[0] == 0x01));
	}
}

/* The power-off's sequence, once, from item n: the shutdown report, the
 * LED clear, 0x32 {s}; then nothing. */
static void poweroff_ran(size_t n, uint8_t s)
{
	static const uint8_t rep[3] = { 0x00, 0x00, 0x02 }, all_off[1] = { 0x01 };

	CHECK(fk.poweroffs == 1);
	CHECK(psoc.off);
	CHECK(fk.nout == n + 3);
	CHECK(is(&fk.out[n], 0x14, 0x51, rep, 3));
	CHECK(is(&fk.out[n + 1], 0x14, 0x40, all_off, 1));
	CHECK(is(&fk.out[n + 2], 0x14, 0x32, &s, 1));
}

static void test_hold(void)
{
	static const uint8_t s10[1] = { 0x10 }, s14[1] = { 0x14 }, s16[1] = { 0x16 },
			     s12[1] = { 0x12 }, ping[2] = { 0x5A, 0xA5 };
	size_t n;

	/* 0x31 {14} while held: kept, nothing sent; a ping still answered;
	 * the release runs the power-off once */
	for (unsigned id = 0x31; id <= 0x32; id++) {
		setup();
		answer(0x30, 0x31, s10, 1, 5 * MS);
		CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
		CHECK(psoc.state == FR_POWER_ON);
		CHECK(fr_psoc_hold(&psoc, true) == FR_PSOC_OK);
		n = fk.nout;
		psoc_at(fk.now + 5 * MS, 0x41, (uint8_t)id, s14, 1);
		psoc_at(fk.now + 10 * MS, 0x41, 0x01, ping, 2);
		CHECK(fr_psoc_sleep(&psoc, 100 * MS) == FR_PSOC_OK);
		CHECK(psoc.state == FR_POWER_OFF);	/* recorded */
		no_poweroff_since(n);
		CHECK(fk.nout == n + 1 && is(&fk.out[n], 0x14, 0x02, ping, 2));
		CHECK(fr_psoc_poll(&psoc) == FR_PSOC_OK);
		CHECK(fr_psoc_hold(&psoc, true) == FR_PSOC_OK);	/* still held */
		no_poweroff_since(n);
		n = fk.nout;
		CHECK(fr_psoc_hold(&psoc, false) == FR_PSOC_OFF);
		poweroff_ran(n, 0x14);
		/* once: nothing more, whatever comes */
		n = fk.nwire;
		CHECK(fr_psoc_hold(&psoc, false) == FR_PSOC_OFF);
		CHECK(fr_psoc_hold(&psoc, true) == FR_PSOC_OFF);
		psoc_at(fk.now + 1 * MS, 0x41, 0x31, s14, 1);
		CHECK(fr_psoc_sleep(&psoc, 10 * MS) == FR_PSOC_OFF);
		CHECK(fk.nwire == n && fk.poweroffs == 1);
	}

	/* 0x16 after 0x10 while held: kept, then run with 0x16 */
	setup();
	answer(0x30, 0x31, s10, 1, 5 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	CHECK(fr_psoc_hold(&psoc, true) == FR_PSOC_OK);
	n = fk.nout;
	psoc_at(fk.now + 5 * MS, 0x41, 0x31, s16, 1);
	CHECK(fr_psoc_sleep(&psoc, 50 * MS) == FR_PSOC_OK);
	no_poweroff_since(n);
	CHECK(fk.nout == n);
	CHECK(fr_psoc_hold(&psoc, false) == FR_PSOC_OFF);
	poweroff_ran(n, 0x16);

	/* S-07.051: a later report of 0x10 (by 0x31 or 0x32) drops a kept
	 * power-off: the switch is on again. The release sends nothing. */
	for (unsigned id = 0x31; id <= 0x32; id++) {
		setup();
		answer(0x30, 0x31, s10, 1, 5 * MS);
		CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
		CHECK(fr_psoc_hold(&psoc, true) == FR_PSOC_OK);
		n = fk.nout;
		psoc_at(fk.now + 5 * MS, 0x41, 0x31, s14, 1);
		CHECK(fr_psoc_sleep(&psoc, 20 * MS) == FR_PSOC_OK);
		CHECK(psoc.off_due);
		psoc_at(fk.now + 5 * MS, 0x41, (uint8_t)id, s10, 1);
		CHECK(fr_psoc_sleep(&psoc, 20 * MS) == FR_PSOC_OK);
		CHECK(!psoc.off_due && psoc.state == FR_POWER_ON);
		no_poweroff_since(n);
		n = fk.nout;
		CHECK(fr_psoc_hold(&psoc, false) == FR_PSOC_OK);
		CHECK(fk.nout == n);
		no_poweroff_since(n);
		CHECK(!psoc.off);
	}
	/* 14, 10, 14: kept again; the release runs it once */
	setup();
	answer(0x30, 0x31, s10, 1, 5 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	CHECK(fr_psoc_hold(&psoc, true) == FR_PSOC_OK);
	n = fk.nout;
	psoc_at(fk.now + 5 * MS, 0x41, 0x31, s14, 1);
	psoc_at(fk.now + 10 * MS, 0x41, 0x31, s10, 1);
	psoc_at(fk.now + 15 * MS, 0x41, 0x31, s14, 1);
	CHECK(fr_psoc_sleep(&psoc, 50 * MS) == FR_PSOC_OK);
	CHECK(psoc.off_due);
	no_poweroff_since(n);
	n = fk.nout;
	CHECK(fr_psoc_hold(&psoc, false) == FR_PSOC_OFF);
	poweroff_ran(n, 0x14);
	CHECK(fr_psoc_hold(&psoc, false) == FR_PSOC_OFF);
	CHECK(fk.poweroffs == 1);

	/* held and released with no power-off message: nothing */
	setup();
	answer(0x30, 0x31, s10, 1, 5 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	CHECK(fr_psoc_hold(&psoc, true) == FR_PSOC_OK);
	n = fk.nout;
	psoc_at(fk.now + 5 * MS, 0x41, 0x31, s12, 1);		/* no power-off */
	CHECK(fr_psoc_sleep(&psoc, 50 * MS) == FR_PSOC_OK);
	CHECK(fr_psoc_hold(&psoc, false) == FR_PSOC_OK);
	no_poweroff_since(n);
	/* and a 0x16 after 0x12 starts none, held or not (S-07.050) */
	CHECK(fr_psoc_hold(&psoc, true) == FR_PSOC_OK);
	psoc_at(fk.now + 5 * MS, 0x41, 0x31, s16, 1);
	CHECK(fr_psoc_sleep(&psoc, 50 * MS) == FR_PSOC_OK);
	CHECK(fr_psoc_hold(&psoc, false) == FR_PSOC_OK);
	no_poweroff_since(n);

	/* no hold: the power-off runs at once, as ever */
	setup();
	answer(0x30, 0x31, s10, 1, 5 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	CHECK(fr_psoc_hold(&psoc, true) == FR_PSOC_OK);
	CHECK(fr_psoc_hold(&psoc, false) == FR_PSOC_OK);
	psoc_at(fk.now + 5 * MS, 0x41, 0x31, s14, 1);
	CHECK(fr_psoc_sleep(&psoc, 50 * MS) == FR_PSOC_OFF);
	CHECK(fk.poweroffs == 1);
	CHECK(fk.out[fk.nout - 1].id == 0x32);
	no_bad_bytes();
}

/* ---- S-02.084's battery request: one 0x36, its 0x37 kept ---- */

static void test_battery_request(void)
{
	static const uint8_t batt[7] = { 0x01, 0x47, 0x1C, 0x00, 0x96, 0x0F, 0x3C },
			     none[1] = { 0x00 };
	uint64_t t;

	setup();
	answer(0x36, 0x37, batt, sizeof batt, 7 * MS);
	t = fk.now;
	CHECK(fr_psoc_battery(&psoc) == FR_PSOC_OK);
	CHECK(fk.nout == 1 && fk.out[0].id == 0x36 && fk.out[0].route == 0x84 && fk.out[0].len == 0);
	CHECK(psoc.batt_len == sizeof batt && memcmp(psoc.batt, batt, sizeof batt) == 0);
	CHECK(fk.now - t < 8 * MS + 100);
	/* no reply: 100 ms, nothing kept but what was there */
	setup();
	t = fk.now;
	CHECK(fr_psoc_battery(&psoc) == FR_PSOC_OK);
	CHECK(fk.nout == 1 && count(0x36, 0x84, 0) == 1);
	CHECK(psoc.batt_len == 0);
	CHECK(fk.now - t >= 100 * MS && fk.now - t < 101 * MS);
	/* a battery removed: status 00 kept as it came */
	setup();
	answer(0x36, 0x37, none, 1, 2 * MS);
	CHECK(fr_psoc_battery(&psoc) == FR_PSOC_OK);
	CHECK(psoc.batt_len == 1 && psoc.batt[0] == 0x00);
	no_bad_bytes();
}

/* ---- S-07.076, S-07.024 ---- */

static void test_ping(void)
{
	static const uint8_t pl[3] = { 0xAA, 0x00, 0xBB }, map[9] = { 0x40 };

	setup();
	psoc_at(fk.now + 1 * MS, 0x41, 0x01, pl, 3);
	psoc_at(fk.now + 2 * MS, 0x41, 0x42, map, 9);	/* ignored */
	psoc_at(fk.now + 3 * MS, 0x41, 0x7E, pl, 3);	/* not served */
	psoc_at(fk.now + 4 * MS, 0x41, 0x45, pl, 1);
	CHECK(fr_psoc_sleep(&psoc, 10 * MS) == FR_PSOC_OK);
	CHECK(fk.nout == 1 && is(&fk.out[0], 0x14, 0x02, pl, 3));
	/* one during a request is answered once the request is over */
	setup();
	psoc_at(fk.now + 10 * MS, 0x41, 0x01, pl, 3);
	CHECK(fr_psoc_request(&psoc, 0x41, NULL, 0, 100 * MS, NULL) == 0);
	CHECK(fk.nout == 2 && is(&fk.out[1], 0x14, 0x02, pl, 3));
	no_bad_bytes();
}

static void test_regreet(void)
{
	static const uint8_t ok[1] = { 0x40 }, held[3] = { 0x3C, 0xFF, 0x02 },
			     sub[2] = { 0x01, 0x01 }, hi[1] = { 0x01 };

	/* before the subscription: 0x06 {40} only */
	setup();
	psoc_at(fk.now + 1 * MS, 0x41, 0x05, hi, 1);
	CHECK(fr_psoc_sleep(&psoc, 10 * MS) == FR_PSOC_OK);
	CHECK(fk.nout == 1 && is(&fk.out[0], 0x14, 0x06, ok, 1));
	/* after it: greeting, {3C FF 02}, 0x43, then 0x06 {40} */
	setup();
	fr_psoc_subscribe(&psoc);
	CHECK(is(&fk.out[0], 0x14, 0x43, sub, 2) && psoc.started);
	fk.answer_greet_at = 2;
	psoc_at(fk.now + 1 * MS, 0x41, 0x05, hi, 1);
	CHECK(fr_psoc_sleep(&psoc, 100 * MS) == FR_PSOC_OK);
	CHECK(fk.nout == 6);
	CHECK(fk.out[1].id == 0x05 && fk.out[2].id == 0x05);
	CHECK(is(&fk.out[3], 0x14, 0x51, held, 3));
	CHECK(is(&fk.out[4], 0x14, 0x43, sub, 2));
	CHECK(is(&fk.out[5], 0x14, 0x06, ok, 1));
	no_bad_bytes();
}

/* ---- S-07.055-S-07.057 ---- */

static void test_usb(void)
{
	static const uint8_t st[2] = { 0x00, 0x0A }, lim[2] = { 0x00, 0x32 },
			     refused[2] = { 0x01, 0x00 };

	setup();
	answer(0x33, 0x33, st, 2, 2 * MS);
	CHECK(fr_psoc_service(&psoc) == FR_PSOC_OK);
	CHECK(fk.nout == 0);			/* target 0: nothing */
	fr_psoc_usb(&psoc, true, 500);
	CHECK(fr_psoc_service(&psoc) == FR_PSOC_OK);
	CHECK(fk.nout == 2 && fk.out[0].id == 0x33 && fk.out[0].route == 0x84);
	CHECK(is(&fk.out[1], 0x14, 0x34, lim, 2));
	CHECK(psoc.applied_ma == 500);
	CHECK(fr_psoc_service(&psoc) == FR_PSOC_OK);
	CHECK(fk.nout == 2);			/* reached */
	fr_psoc_usb(&psoc, false, 0);		/* a reset: 100 mA */
	CHECK(psoc.target_ma == 100);
	CHECK(fr_psoc_service(&psoc) == FR_PSOC_OK);
	CHECK(fk.nout == 2);
	/* a status other than 0: nothing changes, asked on each pass */
	setup();
	answer(0x33, 0x33, refused, 2, 2 * MS);
	fr_psoc_usb(&psoc, true, 500);
	CHECK(fr_psoc_service(&psoc) == FR_PSOC_OK);
	CHECK(fr_psoc_service(&psoc) == FR_PSOC_OK);
	CHECK(fk.nout == 2 && count(0x33, 0x84, 0) == 2 && psoc.applied_ma == 0);
	/* no reply: as {00 00} (S-07.058) */
	setup();
	fr_psoc_usb(&psoc, true, 1000);
	CHECK(fr_psoc_service(&psoc) == FR_PSOC_OK);
	CHECK(fk.nout == 2 && fk.out[1].id == 0x34 && fk.out[1].d[1] == 100);
	CHECK(psoc.applied_ma == 1000);
	no_bad_bytes();
}

static void test_misc(void)
{
	static const uint8_t start[3] = { 0, 0, 0 }, rst[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

	setup();
	fr_psoc_power_queries(&psoc);
	fr_psoc_report(&psoc, FR_REPORT_START);
	fr_psoc_reset(&psoc);
	CHECK(fk.nout == 4);
	CHECK(is(&fk.out[0], 0x14, 0x30, NULL, 0));
	CHECK(is(&fk.out[1], 0x14, 0x36, NULL, 0));
	CHECK(is(&fk.out[2], 0x14, 0x51, start, 3));
	CHECK(is(&fk.out[3], 0x14, 0x97, rst, 4));
	no_bad_bytes();
}

/* ---- the OP-1F (S-18) ---- */

/* S-07.033 on the OP-1F, written out: keys 0x00-0x39 in bytes 0 to 7, no
 * precomp, missing bytes 00, nothing after byte 7. */
static unsigned ref_one_1f(const uint8_t *p, size_t n)
{
	unsigned key = FR_KEYS_NONE;

	if (p == NULL)
		return FR_KEYS_UNREAD;
	for (unsigned k = 0; k < 58u; k++) {
		if (k >> 3 >= n || ((p[k >> 3] >> (k & 7u)) & 1u) == 0u)
			continue;
		if (key != FR_KEYS_NONE)
			return FR_KEYS_MANY;
		key = k;
	}
	return key;
}

static void test_1f_keys(void)
{
	const struct fr_dev *d = &fr_dev_op1f;
	uint8_t p[10];

	CHECK(fr_keys_one(d, NULL, 0) == FR_KEYS_UNREAD && fr_keys_hold(d, NULL, 0));
	/* each key alone, past the count included */
	for (unsigned k = 0; k < 80u; k++) {
		memset(p, 0, sizeof p);
		p[k >> 3] = (uint8_t)(1u << (k & 7u));
		CHECK(fr_keys_one(d, p, sizeof p) == ref_one_1f(p, sizeof p));
		CHECK(fr_keys_one(d, p, sizeof p) == (k < 58u ? k : FR_KEYS_NONE));
		/* S-07.032: hold is 0x39 (byte 7 bit 1) or 0x2C (byte 5 bit 4) */
		CHECK(fr_keys_hold(d, p, sizeof p) == (k == 0x39u || k == 0x2Cu));
	}
	/* the OP-XY's precomp key is an ordinary key here */
	memset(p, 0, sizeof p);
	p[2] = 0x80u;
	CHECK(fr_keys_one(d, p, sizeof p) == 0x17u);
	p[1] = 0x02u;
	CHECK(fr_keys_one(d, p, sizeof p) == FR_KEYS_MANY);
	/* the chord's key alone (provisionally 0x09), and with others */
	memset(p, 0, sizeof p);
	p[1] = 0x02u;
	CHECK(fr_keys_one(d, p, 8) == d->key_recovery && d->key_recovery == 0x09u);
	p[8] = 0xFFu;			/* not read */
	CHECK(fr_keys_one(d, p, 9) == 0x09u);
	p[7] = 0x04u;			/* key 0x3A: none of the 58 */
	CHECK(fr_keys_one(d, p, 9) == 0x09u);
	p[7] = 0x02u;			/* key 0x39, a hold key */
	CHECK(fr_keys_one(d, p, 9) == FR_KEYS_MANY && fr_keys_hold(d, p, 9));
	CHECK(!fr_keys_hold(d, p, 7));	/* byte 7 missing */
	/* pairs against the reference, at several lengths */
	for (unsigned a = 0; a < 72; a++)
		for (unsigned b = a; b < 72; b++)
			for (size_t n = 0; n <= 10; n += 3) {
				memset(p, 0, sizeof p);
				p[a >> 3] = (uint8_t)(p[a >> 3] | 1u << (a & 7u));
				p[b >> 3] = (uint8_t)(p[b >> 3] | 1u << (b & 7u));
				CHECK(fr_keys_one(d, p, n) == ref_one_1f(p, n));
			}
	/* NULL is the default device's reading */
	memset(p, 0, sizeof p);
	p[2] = 0x80u;
	CHECK(fr_keys_one(NULL, p, sizeof p) == FR_KEYS_NONE);
}

/* The 0x40 messages sent from item `from`. */
static size_t leds(size_t from, const struct item **out, size_t max)
{
	size_t n = 0;

	for (size_t i = from; i < fk.nout && n < max; i++)
		if (!fk.out[i].raw && fk.out[i].id == 0x40u)
			out[n++] = &fk.out[i];
	return n;
}

/* S-07.046, S-07.047 in state 0x10: {06 r 48 g 48 b 48}, sent only when the
 * color changes, none of S-07.040's forms. */
static void test_1f_led_forms(void)
{
	static const uint8_t s10[1] = { 0x10 }, low[2] = { 0x01, 10 }, full[2] = { 0x01, 80 },
			     red[7] = { 0x06, 0xC8, 0x48, 0x00, 0x48, 0x00, 0x48 },
			     green[7] = { 0x06, 0x00, 0x48, 0xC8, 0x48, 0x00, 0x48 };
	const struct item *l[64] = { NULL };
	size_t n;

	setup_dev(&fr_dev_op1f);
	answer(0x36, 0x37, low, 2, 5 * MS);
	answer(0x30, 0x31, s10, 1, 5 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	n = leds(0, l, ARRAY_LEN(l));
	/* the battery reply before any state: dark, as at power-on, so
	 * nothing; then red, once */
	CHECK(n == 1 && is(l[0], 0x14, 0x40, red, 7));
	/* the same state again: nothing; a charge of 80: green, once */
	psoc_at(fk.now + 5 * MS, 0x41, 0x31, s10, 1);
	psoc_at(fk.now + 6 * MS, 0x41, 0x31, s10, 1);
	CHECK(fr_psoc_sleep(&psoc, 20 * MS) == FR_PSOC_OK);
	CHECK(leds(0, l, ARRAY_LEN(l)) == 1);
	psoc_at(fk.now + 5 * MS, 0x41, 0x37, full, 2);
	psoc_at(fk.now + 6 * MS, 0x41, 0x37, full, 2);
	CHECK(fr_psoc_sleep(&psoc, 20 * MS) == FR_PSOC_OK);
	n = leds(0, l, ARRAY_LEN(l));
	CHECK(n == 2 && is(l[1], 0x14, 0x40, green, 7));
	for (size_t i = 0; i < n; i++)
		CHECK(l[i]->len == 7 && l[i]->d[0] == 0x06);
	no_bad_bytes();
}

/* sin by its series, after reducing x to [-pi, pi]. */
static double ref_sin(double x)
{
	const double pi = 3.14159265358979323846;
	double term, sum;

	while (x > pi)
		x -= 2.0 * pi;
	while (x < -pi)
		x += 2.0 * pi;
	term = x;
	sum = x;
	for (int k = 1; k < 12; k++) {
		term *= -x * x / ((2.0 * k) * (2.0 * k + 1.0));
		sum += term;
	}
	return sum;
}

/* S-07.047's level at phase phi for a base of 0xC8, the fraction dropped. */
static unsigned ref_level(double phi)
{
	return (unsigned)(200.0 * (0.25 + 0.75 * (0.5 + 0.5 * ref_sin(phi))));
}

/* S-07.047 in the charge wait: red breathing (charge 50, at most 90 %),
 * each message within one level of the reference at its time, the phase
 * 0.6 pi rad/s from the first update, the levels from 0x32 to 0xC8, only
 * changes sent. The VBUS request is answered, so that no two updates are
 * 100 ms or more apart; the switch goes on after about 4 s. */
static void test_1f_breathing(void)
{
	static const uint8_t batt[2] = { 0x01, 50 }, s16[1] = { 0x16 }, vb[2] = { 0x00, 0x32 },
			     green[7] = { 0x06, 0x00, 0x48, 0xC8, 0x48, 0x00, 0x48 };
	static const struct item *l[1024];
	unsigned lo = 0xFFu, hi = 0, off = 0;
	uint64_t t0;
	size_t n;

	setup_dev(&fr_dev_op1f);
	answer(0x36, 0x37, batt, 2, 2 * MS);
	answer(0x30, 0x31, s16, 1, 2 * MS);
	answer(0x33, 0x33, vb, 2, 1 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	CHECK(psoc.state == FR_POWER_CHARGE);
	fk.switch_on_at = fk.now + 3800 * MS;
	CHECK(fr_psoc_charge_wait(&psoc) == FR_PSOC_OK);
	n = leds(0, l, ARRAY_LEN(l));
	CHECK(n > 100u && n < ARRAY_LEN(l));
	if (n < 2u)
		return;
	/* the first update in 0x16 is phase 0: factor 0.625, 0x7D */
	CHECK(l[0]->len == 7 && l[0]->d[1] == 0x7Du);
	t0 = l[0]->t;
	/* the switch on: state 0x10 at charge 50, green */
	CHECK(is(l[n - 1], 0x14, 0x40, green, 7));
	for (size_t i = 0; i + 1u < n; i++) {
		const struct item *it = l[i];
		const double phi = 0.6 * 3.14159265358979 * (double)(it->t - t0) / 1e6;
		const unsigned r = it->d[1], want = ref_level(phi);

		CHECK(it->len == 7 && it->d[0] == 0x06 && it->d[2] == 0x48 && it->d[3] == 0 &&
		      it->d[4] == 0x48 && it->d[5] == 0 && it->d[6] == 0x48);
		if (r + 1u < want || r > want + 1u)
			off++;
		if (i > 0)
			CHECK(r != l[i - 1]->d[1]);	/* only changes */
		if (r < lo)
			lo = r;
		if (r > hi)
			hi = r;
	}
	CHECK(off == 0u);
	CHECK(lo >= 0x32u && lo <= 0x33u && hi >= 0xC7u && hi <= 0xC8u);
	no_bad_bytes();
}

/* S-07.047's 100 ms rule: an update more than 100 ms after the one before
 * moves the phase by nothing. Updates here are 0x31 {16} messages. */
static void test_1f_breath_gap(void)
{
	static const uint8_t batt[2] = { 0x01, 50 }, s16[1] = { 0x16 };
	const struct item *l[16] = { NULL };
	size_t n;

	setup_dev(&fr_dev_op1f);
	answer(0x36, 0x37, batt, 2, 2 * MS);
	answer(0x30, 0x31, s16, 1, 2 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	CHECK(leds(0, l, ARRAY_LEN(l)) == 1 && l[0]->d[1] == 0x7Du);	/* phase 0 */
	/* 60 ms on: 0.1131 rad, 125 + 75 sin = 133.5 */
	psoc_at(fk.now + 60 * MS, 0x41, 0x31, s16, 1);
	CHECK(fr_psoc_sleep(&psoc, 70 * MS) == FR_PSOC_OK);
	n = leds(0, l, ARRAY_LEN(l));
	CHECK(n == 2 && (l[1]->d[1] == 133u || l[1]->d[1] == 132u));
	/* 150 ms later: no step, so the same level and no message */
	psoc_at(fk.now + 140 * MS, 0x41, 0x31, s16, 1);
	CHECK(fr_psoc_sleep(&psoc, 150 * MS) == FR_PSOC_OK);
	CHECK(leds(0, l, ARRAY_LEN(l)) == 2);
	/* 90 ms after that: 0.2827 rad in all, 125 + 75 sin = 145.9 */
	psoc_at(fk.now + 80 * MS, 0x41, 0x31, s16, 1);
	CHECK(fr_psoc_sleep(&psoc, 90 * MS) == FR_PSOC_OK);
	n = leds(0, l, ARRAY_LEN(l));
	CHECK(n == 3 && (l[2]->d[1] == 145u || l[2]->d[1] == 146u));
	no_bad_bytes();
}

/* S-07.051 on the OP-1F: the shutdown report and the echo, no 0x40 {01}. */
static void test_1f_poweroff(void)
{
	static const uint8_t s10[1] = { 0x10 }, s14[1] = { 0x14 }, rep[3] = { 0x00, 0x00, 0x02 };
	size_t n;

	setup_dev(&fr_dev_op1f);
	answer(0x30, 0x31, s10, 1, 5 * MS);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	psoc_at(fk.now + 50 * MS, 0x41, 0x31, s14, 1);
	CHECK(fr_psoc_sleep(&psoc, 200 * MS) == FR_PSOC_OFF);
	CHECK(fk.poweroffs == 1);
	n = fk.nout_at_poweroff;
	CHECK(fk.nout == n + 2);
	CHECK(is(&fk.out[n], 0x14, 0x51, rep, 3));
	CHECK(is(&fk.out[n + 1], 0x14, 0x32, s14, 1));
	for (size_t i = 0; i < fk.nout; i++)
		CHECK(!(fk.out[i].id == 0x40u && fk.out[i].len == 1u));
	no_bad_bytes();
}

/* What the front sends on the OP-1F holds no PMIC pass-through id (S-07
 * Devices: no 0x70, 0x71, 0x73 and no request 0x01); a ping from the PSoC
 * is still answered. */
static void test_1f_no_pmic(void)
{
	static const uint8_t s10[1] = { 0x10 }, ping[1] = { 0xAA };

	setup_dev(&fr_dev_op1f);
	fk.answer_greet_at = 1;
	answer(0x30, 0x31, s10, 1, 5 * MS);
	CHECK(fr_psoc_greet(&psoc) == 1);
	fr_psoc_power_queries(&psoc);
	CHECK(fr_psoc_power_requests(&psoc) == FR_PSOC_OK);
	fr_psoc_subscribe(&psoc);
	CHECK(fr_psoc_key_status(&psoc) == FR_PSOC_OK);
	psoc_at(fk.now + 5 * MS, 0x41, 0x01, ping, 1);
	CHECK(fr_psoc_sleep(&psoc, 20 * MS) == FR_PSOC_OK);
	CHECK(count(0x02, 0x14, 0) == 1);
	for (size_t i = 0; i < fk.nout; i++) {
		CHECK(fk.out[i].id != 0x70u && fk.out[i].id != 0x71u && fk.out[i].id != 0x73u);
		CHECK(fk.out[i].id != 0x01u);
	}
	no_bad_bytes();
}

int main(void)
{
	RUN(test_encode);
	RUN(test_send);
	RUN(test_receive);
	RUN(test_greeting);
	RUN(test_greet_wait);
	RUN(test_requests);
	RUN(test_late_reply);
	RUN(test_keys);
	RUN(test_keys_hold_bits);
	RUN(test_keys_one);
	RUN(test_charge_wait);
	RUN(test_poweroff);
	RUN(test_hold);
	RUN(test_battery_request);
	RUN(test_kept);
	RUN(test_ping);
	RUN(test_regreet);
	RUN(test_usb);
	RUN(test_misc);
	RUN(test_1f_keys);
	RUN(test_1f_led_forms);
	RUN(test_1f_breathing);
	RUN(test_1f_breath_gap);
	RUN(test_1f_poweroff);
	RUN(test_1f_no_pmic);
	return check_summary("test_front_psoc");
}

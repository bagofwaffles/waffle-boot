/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-06 receiver: S-06.013, S-06.021, S-06.040-S-06.049 against
 * tests/oracle/S-06-framing/vectors/receive.json, fed whole and byte by byte, and the
 * resynchronization property of S-06.049 over random prefixes.
 */
#include "waffle/link/rx.h"

#include "s06_test.h"
#include "vectors/s06_vectors.h"

struct got {
	int kind;
	uint8_t data[S06_MAX_WIRE];
	size_t len;
};

/* Feed bytes, recording the receive.json view of each event. */
static size_t feed(struct s06_rx *rx, const uint8_t *p, size_t n,
		   struct got *g, size_t max)
{
	size_t k = 0;

	for (size_t i = 0; i < n; i++) {
		int ev = s06_rx_byte(rx, p[i], t_crc8);

		if (ev == S06_RX_NONE || k >= max)
			continue;
		if (ev == S06_RX_DELIVER) {
			g[k].kind = 1;
			g[k].len = rx->n - 1u;		/* without the check */
		} else if (ev == S06_RX_FORWARD) {
			g[k].kind = 2;
			g[k].len = rx->n;
		} else {
			g[k].kind = 3;
			g[k].len = 0;
		}
		memcpy(g[k].data, rx->buf, g[k].len);
		k++;
	}
	return k;
}

static void test_vectors(void)
{
	static struct got g[16];

	for (size_t i = 0; i < s06v_rx_count; i++) {
		const struct s06v_rx *v = &s06v_rx[i];
		const struct s06v_rx_event *e = &s06v_rx_events[v->first_event];
		struct s06_rx rx;
		size_t k;

		t_case = v->name;
		s06_rx_init(&rx, v->address);
		k = feed(&rx, v->wire, v->len, g, 16);
		CHECK_EQ(k, v->n_events);
		for (size_t j = 0; j < k && j < v->n_events; j++) {
			CHECK_EQ(g[j].kind, e[j].kind);
			if (e[j].kind != 3)
				CHECK_MEM(g[j].data, g[j].len, e[j].data, e[j].len);
		}
	}
}

/* S-06.040: byte by byte gives the same events as whole (feed is per byte
 * already); here a fresh receiver per chunk boundary checks there is no
 * hidden dependence on call boundaries: two halves fed separately. */
static void test_split(void)
{
	static struct got a[16], b[16];

	for (size_t i = 0; i < s06v_rx_count; i++) {
		const struct s06v_rx *v = &s06v_rx[i];
		struct s06_rx rx1, rx2;
		size_t ka, kb;

		t_case = v->name;
		s06_rx_init(&rx1, v->address);
		ka = feed(&rx1, v->wire, v->len, a, 16);
		s06_rx_init(&rx2, v->address);
		kb = feed(&rx2, v->wire, v->len / 2, b, 16);
		kb += feed(&rx2, v->wire + v->len / 2, v->len - v->len / 2,
			   b + kb, 16 - kb);
		CHECK_EQ(ka, kb);
		for (size_t j = 0; j < ka && j < kb; j++)
			CHECK_EQ(a[j].kind, b[j].kind);
	}
}

/* S-06.049: any prefix, then a delimiter and a good frame: delivered last. */
static void test_resync(void)
{
	static struct got g[300];
	uint8_t frame[S06_MAX_WIRE], buf[1024];
	const uint8_t payload[3] = { 0xAA, 0x00, 0x55 };
	size_t fn = s06_frame_encode(0x41, 7, 0x09, payload, 3, t_crc8, frame);
	uint32_t seed = 49;

	t_case = "resync";
	for (int r = 0; r < 2000; r++) {
		size_t plen = t_rand(&seed) % 600u, n, k;
		struct s06_rx rx;

		for (size_t i = 0; i < plen; i++) {
			uint32_t x = t_rand(&seed);
			buf[i] = (uint8_t)((x & 7u) == 0 ? 0 : x >> 3);
		}
		buf[plen] = 0;
		memcpy(buf + plen + 1, frame, fn);
		n = plen + 1 + fn;
		s06_rx_init(&rx, 1);
		k = feed(&rx, buf, n, g, 300);
		CHECK(k >= 1);
		if (k >= 1) {
			CHECK_EQ(g[k - 1].kind, 1);
			CHECK_EQ(g[k - 1].len, 4u + 3u);
			CHECK_EQ(g[k - 1].data[0], 0x41);
		}
	}
}

/* S-06.013: own address or bit 3; never 0. */
static void test_for_node(void)
{
	t_case = "for-node";
	for (uint8_t own = 1; own <= 5; own++)
		for (uint8_t d = 0; d < 16; d++)
			CHECK_EQ(s06_rx_for_node(own, d), d == own || d >= 8);
}

int main(void)
{
	test_vectors();
	test_split();
	test_resync();
	test_for_node();
	return t_finish("test_s06_rx");
}

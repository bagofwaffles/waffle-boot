/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for S-07's rules without I/O (src/core/psoc/rules.c) and the
 * PSoC bootloader-host protocol (src/core/psoc/cypress.c).
 *
 * Commands (one result line each):
 *   keys BITMAP              -> the numbers of the keys down (s07_key_down), or "-"
 *   status PAYLOAD|none      -> REPLIED HOLD DEBUG PRECOMP (s07_key_status_decode)
 *   power PREV STATE         -> 0|1 (s07_power_off_trigger; PREV 0x100: none)
 *   battery PAYLOAD          -> PRESENT CHARGING CHARGE TEMP MA MV USABLE
 *   led STATE CHARGE         -> RGB FORCED (s07_charge_led; STATE 0x100: none)
 *   vbus APPLIED TARGET ST N -> APPLIED SEND_T (s07_vbus_update; SEND_T -1: none)
 *   pmic V                   -> the write value (s07_pmic_write_value)
 *   cypacket CODE KIND DATA  -> the packet (s07_cy_packet; KIND 0 sum, 1 CRC)
 *   cycheck KIND BYTES       -> the check over BYTES (s07_cy_checksum)
 *   probe BYTES              -> 0|1 (s07_cy_probe_ok)
 *   exitpacket               -> s07_cy_exit_packet
 *   image CYACD              -> 1 when the file is bad (s07_cyacd_open, s07_cyacd_check)
 *   run CYACD UNLOCK REPLIES -> OK AT WHY, then every packet sent (s07_cy_run;
 *                               CYACD the file's bytes; REPLIES comma-separated,
 *                               taken in order, "none": no reply within 5 s)
 *
 * The CRC variant's CRC-16/X-25 is test glue, written here from S-07.082's
 * parameters.
 */
#include <stdio.h>
#include <string.h>

#include "hlib.h"
#include "waffle/psoc/cypress.h"
#include "waffle/psoc/rules.h"

static uint8_t buf[H_MAXLINE / 2];

static uint16_t crc16_x25(const uint8_t *d, size_t n)
{
	uint16_t crc = 0xFFFF;

	for (size_t i = 0; i < n; i++) {
		crc ^= d[i];
		for (int b = 0; b < 8; b++)
			crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u)
					 : (uint16_t)(crc >> 1);
	}
	return (uint16_t)~crc;
}

/* ---- a run against replies given in advance ---- */

#define MAX_REPLIES 1024
#define MAX_SENT 1024

struct replay {
	uint8_t reply[MAX_REPLIES][16];
	int len[MAX_REPLIES];		/* -1: no reply */
	size_t n, next;
	uint8_t sent[MAX_SENT][S07_CY_MAX_PACKET];
	size_t sent_len[MAX_SENT], nsent;
	int overflow;
};

static struct replay rp;

static int hexval(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

/* REPLIES into rp; false if malformed. */
static int parse_replies(const char *s)
{
	rp.n = 0;
	rp.next = 0;
	rp.nsent = 0;
	rp.overflow = 0;
	if (strcmp(s, "-") == 0)
		return 1;
	for (;;) {
		const char *e = strchr(s, ',');
		size_t k = e ? (size_t)(e - s) : strlen(s);

		if (rp.n == MAX_REPLIES)
			return 0;
		if (k == 4 && strncmp(s, "none", 4) == 0) {
			rp.len[rp.n++] = -1;
		} else {
			if (k % 2u || k / 2u > sizeof rp.reply[0])
				return 0;
			for (size_t i = 0; i < k; i += 2) {
				int h = hexval(s[i]), l = hexval(s[i + 1]);

				if (h < 0 || l < 0)
					return 0;
				rp.reply[rp.n][i / 2] = (uint8_t)(h << 4 | l);
			}
			rp.len[rp.n++] = (int)(k / 2u);
		}
		if (!e)
			return 1;
		s = e + 1;
	}
}

static void rp_send(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	if (rp.nsent == MAX_SENT || n > S07_CY_MAX_PACKET) {
		rp.overflow = 1;
		return;
	}
	memcpy(rp.sent[rp.nsent], p, n);
	rp.sent_len[rp.nsent++] = n;
}

/* The next reply, when it is there and n bytes long; otherwise none. */
static bool rp_recv(void *ctx, uint8_t *b, size_t n, uint32_t timeout_us)
{
	(void)ctx;
	(void)timeout_us;
	if (rp.next == rp.n)
		return false;
	if (rp.len[rp.next] != (int)n) {
		rp.next++;
		return false;
	}
	memcpy(b, rp.reply[rp.next++], n);
	return true;
}

int main(void)
{
	struct h_cmd c;

	while (h_next(&c)) {
		const char *k = c.tok[0];

		if (strcmp(k, "keys") == 0 && c.n == 2) {
			size_t n = h_hex(&c, 1, buf, sizeof buf);
			bool any = false;

			if (n != S07_BITMAP) {
				h_error(&c, "bitmap length");
				continue;
			}
			for (unsigned i = 0; i < S07_KEYS; i++)
				if (s07_key_down(buf, i)) {
					h_putu(i);
					any = true;
				}
			if (!any)
				h_put("-");
			h_end();
		} else if (strcmp(k, "status") == 0 && c.n == 2) {
			struct s07_key_status ks;

			if (strcmp(c.tok[1], "none") == 0)
				s07_key_status_decode(&wb_device_opxy, NULL, 0, &ks);
			else
				s07_key_status_decode(&wb_device_opxy, buf, h_hex(&c, 1, buf, sizeof buf), &ks);
			h_putu(ks.replied);
			h_putu(ks.hold);
			h_putu(ks.debug);
			h_putu(ks.precomp);
			h_end();
		} else if (strcmp(k, "power") == 0 && c.n == 3) {
			h_putu(s07_power_off_trigger((unsigned)h_num(&c, 1), (unsigned)h_num(&c, 2)));
			h_end();
		} else if (strcmp(k, "battery") == 0 && c.n == 2) {
			struct s07_battery b;

			s07_battery_decode(buf, h_hex(&c, 1, buf, sizeof buf), &b);
			h_putu(b.present);
			h_putu(b.charging);
			h_putu(b.charge);
			h_putu(b.temp);
			h_putu(b.ma);
			h_putu(b.mv);
			h_putu(s07_battery_usable(&b));
			h_end();
		} else if (strcmp(k, "led") == 0 && c.n == 3) {
			uint8_t rgb[3];
			bool forced = false;

			s07_charge_led((unsigned)h_num(&c, 1), (uint8_t)h_num(&c, 2), rgb, &forced);
			h_puthex(rgb, sizeof rgb);
			h_putu(forced);
			h_end();
		} else if (strcmp(k, "vbus") == 0 && c.n == 5) {
			int t = -1;
			char s[16];

			h_putu(s07_vbus_update((uint32_t)h_num(&c, 1), (uint32_t)h_num(&c, 2),
					       (uint8_t)h_num(&c, 3), (uint8_t)h_num(&c, 4), &t));
			snprintf(s, sizeof s, "%d", t);
			h_put(s);
			h_end();
		} else if (strcmp(k, "pmic") == 0 && c.n == 2) {
			h_putx(s07_pmic_write_value((uint8_t)h_num(&c, 1)));
			h_end();
		} else if (strcmp(k, "cypacket") == 0 && c.n == 4) {
			uint8_t d[S07_CY_MAX_PACKET], p[S07_CY_MAX_PACKET + 7];
			size_t n = h_hex(&c, 3, d, sizeof d);
			size_t len = s07_cy_packet((uint8_t)h_num(&c, 1), d, n,
						   (int)h_num(&c, 2), crc16_x25, p);

			h_puthex(p, len);
			h_end();
		} else if (strcmp(k, "cycheck") == 0 && c.n == 3) {
			size_t n = h_hex(&c, 2, buf, sizeof buf);

			h_putx(s07_cy_checksum(buf, n, (int)h_num(&c, 1), crc16_x25));
			h_end();
		} else if (strcmp(k, "probe") == 0 && c.n == 2) {
			h_putu(s07_cy_probe_ok(buf, h_hex(&c, 1, buf, sizeof buf)));
			h_end();
		} else if (strcmp(k, "image") == 0 && c.n == 2) {
			struct s07_cyacd f;
			size_t n = h_hex(&c, 1, buf, sizeof buf);

			h_putu(!(s07_cyacd_open(&f, (const char *)buf, n) &&
				 s07_cyacd_check(&f)));
			h_end();
		} else if (strcmp(k, "exitpacket") == 0 && c.n == 1) {
			h_puthex(s07_cy_exit_packet, sizeof s07_cy_exit_packet);
			h_end();
		} else if (strcmp(k, "run") == 0 && c.n == 4) {
			static const struct s07_cy_io io = { rp_send, rp_recv, 0 };
			uint8_t unlock[S07_CY_UNLOCK_LEN];
			struct s07_cy_result res;
			struct s07_cyacd f;
			size_t n = h_hex(&c, 1, buf, sizeof buf);
			size_t un = h_hex(&c, 2, unlock, sizeof unlock);

			if (un != sizeof unlock || !parse_replies(c.tok[3])) {
				h_error(&c, "unlock or replies");
				continue;
			}
			if (!s07_cyacd_open(&f, (const char *)buf, n)) {
				h_error(&c, "cyacd header");
				continue;
			}
			s07_cy_run(&io, &f, unlock, crc16_x25, &res);
			if (rp.overflow) {
				h_error(&c, "too many packets");
				continue;
			}
			h_putu(res.ok);
			h_putx(res.at);
			h_putu(res.why);
			for (size_t i = 0; i < rp.nsent; i++)
				h_puthex(rp.sent[i], rp.sent_len[i]);
			h_end();
		} else {
			h_error(&c, "unknown command");
		}
	}
	return h_status();
}

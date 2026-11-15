/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for S-08's reading rules (src/s08_rules.c). Payloads are hex
 * ("-": empty); "none" is no reply.
 *
 * Commands (one result line each):
 *   greet STATE|unknown PAYLOAD -> ID BYTE, or "-" when ignored (s08_answer_greeting)
 *   mode PAYLOAD|none           -> 0 no answer | 1 in DFU | 2 not (s08_mode_answer)
 *   slot PAYLOAD|none           -> the slot, or -1 (s08_boot_slot)
 *   accept ID PAYLOAD           -> 0|1 (s08_counts_as_greeting)
 *   dfu CMD ID PAYLOAD          -> 0 fail | 1 ok | 2 busy (s08_classify_dfu)
 *   product SKU OK              -> the product word (s08_product_word)
 *   prepare V0 V1 V2 V3 TYPE PRODUCT -> the 14-byte payload (s08_prepare_payload)
 *   header LEN V0 V1 V2 V3 PRODUCT   -> the 64-byte header (s08_te_header) of LEN image
 *                                       bytes (i * 7 + 3) mod 256
 */
#include <stdio.h>
#include <string.h>

#include "hlib.h"
#include "waffle/s08_nordic.h"

static uint8_t buf[H_MAXLINE / 2];
static uint8_t image[0x10000];

/* Test glue: CRC-16/XMODEM, as the caller supplies it (S-08.044). */
static uint16_t xmodem(const uint8_t *d, size_t n)
{
	uint32_t crc = 0;

	for (size_t i = 0; i < n; i++) {
		crc ^= (uint32_t)d[i] << 8;
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000u ? crc << 1 ^ 0x1021u : crc << 1) & 0xFFFFu;
	}
	return (uint16_t)crc;
}

static void versions(const struct h_cmd *c, int first, uint16_t v[4])
{
	for (int i = 0; i < 4; i++)
		v[i] = (uint16_t)h_num(c, first + i);
}

int main(void)
{
	struct h_cmd c;

	while (h_next(&c)) {
		const char *k = c.tok[0];

		if (strcmp(k, "greet") == 0 && c.n == 3) {
			int state = strcmp(c.tok[1], "unknown") == 0 ? S08_STATE_UNKNOWN : (int)h_num(&c, 1);
			size_t n = h_hex(&c, 2, buf, sizeof buf);
			uint8_t id = 0, byte = 0;

			if (s08_answer_greeting(state, buf, n, &id, &byte)) {
				h_putx(id);
				h_putx(byte);
			} else {
				h_put("-");
			}
			h_end();
		} else if ((strcmp(k, "mode") == 0 || strcmp(k, "slot") == 0) && c.n == 2) {
			bool none = strcmp(c.tok[1], "none") == 0;
			size_t n = none ? 0 : h_hex(&c, 1, buf, sizeof buf);
			const uint8_t *p = none ? NULL : buf;
			char s[24];

			if (k[0] == 'm')
				snprintf(s, sizeof s, "%d", s08_mode_answer(p, n));
			else
				snprintf(s, sizeof s, "%ld", s08_boot_slot(p, n));
			h_put(s);
			h_end();
		} else if (strcmp(k, "accept") == 0 && c.n == 3) {
			struct s06_frame f;

			memset(&f, 0, sizeof f);
			f.id = (uint8_t)h_num(&c, 1);
			f.len = (uint8_t)h_hex(&c, 2, f.payload, sizeof f.payload);
			h_putu(s08_counts_as_greeting(&f));
			h_end();
		} else if (strcmp(k, "dfu") == 0 && c.n == 4) {
			size_t n = h_hex(&c, 3, buf, sizeof buf);

			h_putu((unsigned long)s08_classify_dfu((uint8_t)h_num(&c, 1), (uint8_t)h_num(&c, 2), buf, n));
			h_end();
		} else if (strcmp(k, "product") == 0 && c.n == 3) {
			h_putx(s08_product_word((uint32_t)h_num(&c, 1), h_num(&c, 2) != 0));
			h_end();
		} else if (strcmp(k, "prepare") == 0 && c.n == 7) {
			uint16_t v[4];
			uint8_t out[14];

			versions(&c, 1, v);
			s08_prepare_payload(v, (uint8_t)h_num(&c, 5), (uint32_t)h_num(&c, 6), out);
			h_puthex(out, sizeof out);
			h_end();
		} else if (strcmp(k, "header") == 0 && c.n == 7) {
			uint32_t len = (uint32_t)h_num(&c, 1);
			uint16_t v[4];
			uint8_t out[S08_HEADER_LEN];

			if (len > sizeof image) {
				h_error(&c, "image too long");
				continue;
			}
			for (uint32_t i = 0; i < len; i++)
				image[i] = (uint8_t)(i * 7u + 3u);
			versions(&c, 2, v);
			s08_te_header(image, len, v, (uint32_t)h_num(&c, 6), xmodem, out);
			h_puthex(out, sizeof out);
			h_end();
		} else {
			h_error(&c, "unknown command");
		}
	}
	return h_status();
}

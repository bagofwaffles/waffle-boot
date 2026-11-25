/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-08 reading rules: greeting answers, key:value queries, DFU replies and
 * the transfer's bytes.
 */
#include "waffle/nordic/nordic.h"
#include "waffle/util/bytes.h"

#define ID_GREET_OK 0x06u
#define ID_DFU 0x0Du
#define ID_DFU_REPLY 0x0Eu
#define DFU_ENTER 0xEEu
#define DFU_EXIT 0x18u
#define REPLY_OK 0x54u			/* 'T' */
#define REPLY_BUSY 0x92u

/* S-08.011, S-08.012 */
bool s08_answer_greeting(int state, const uint8_t *p, size_t n, uint8_t *id,
			 uint8_t *byte)
{
	int flag = n ? p[0] : -1;

	if (state == S08_STATE_UNKNOWN)
		return false;
	*id = ID_GREET_OK;
	*byte = 0x40;
	if (state == S08_STATE_IDLE && flag == 0xBB) {
		*id = ID_DFU;
		*byte = DFU_EXIT;
	} else if (state == S08_STATE_UPGRADING && (flag == 0xBB || flag == 0xAA)) {
		*id = ID_DFU;
		*byte = DFU_ENTER;
	}
	return true;
}

/* S-08 Terms: the key before an entry's first ':', the value up to the
 * next ';' or the end; keys equal exactly (S-16.098); the first match. */
long s08_kv_find(const uint8_t *p, size_t n, const char *key,
		 const uint8_t **val)
{
	size_t at = 0;

	while (at <= n) {
		size_t end = at, colon;

		while (end < n && p[end] != ';')
			end++;
		for (colon = at; colon < end && p[colon] != ':'; colon++)
			;
		if (colon < end) {
			size_t k = 0;

			while (key[k] && at + k < colon && p[at + k] == (uint8_t)key[k])
				k++;
			if (!key[k] && at + k == colon) {
				*val = p + colon + 1;
				return (long)(end - colon - 1);
			}
		}
		at = end + 1;
	}
	return -1;
}

/* S-08.021 */
int s08_mode_answer(const uint8_t *p, size_t n)
{
	static const uint8_t bl[10] = { 'b', 'o', 'o', 't', 'l', 'o', 'a', 'd', 'e', 'r' };
	const uint8_t *v;
	long len;

	if (!p)
		return S08_NO_ANSWER;
	len = s08_kv_find(p, n, "mode", &v);
	if (len < 0)
		return S08_NO_ANSWER;
	return len == 10 && wb_equal(v, bl, 10) ? S08_IN_DFU : S08_NOT_IN_DFU;
}

/* S-08.022: one or more decimal digits and nothing else. */
long s08_boot_slot(const uint8_t *p, size_t n)
{
	const uint8_t *v;
	long len = p ? s08_kv_find(p, n, "boot_slot", &v) : -1;
	uint32_t slot = 0;

	if (len <= 0)
		return -1;
	for (long i = 0; i < len; i++) {
		if (v[i] < '0' || v[i] > '9')
			return -1;
		/* S-08.022: never reduced to a smaller number; a value too
		 * large to hold is answered as no slot */
		if (slot > 214748364u)
			return -1;
		slot = slot * 10u + (uint32_t)(v[i] - '0');
		if (slot > 0x7FFFFFFFu)
			return -1;
	}
	return (long)slot;
}

/* S-08.014: answered only when the last answer was sent more than 20 ms
 * before the arrival; time wraps, so the difference is signed. */
bool s08_greeting_due(bool answered, uint32_t answered_at, uint32_t at)
{
	return !answered || (int32_t)(at - answered_at) > (int32_t)S08_ANSWER_GAP_US;
}

/* S-08.031 step 4: id 0x06, byte 0 is 0x40; an empty payload never counts
 * (S-16.185). */
bool s08_counts_as_greeting(const struct s06_frame *f)
{
	return f->id == ID_GREET_OK && f->len > 0 && f->payload[0] == 0x40;
}

/* S-08.040 */
int s08_classify_dfu(uint8_t cmd, uint8_t reply_id, const uint8_t *p,
		     size_t n)
{
	if ((cmd == S08_DFU_PREPARE || cmd == S08_DFU_PERFORM) && n > 0 &&
	    p[0] == REPLY_BUSY)
		return S08_DFU_BUSY;
	if (reply_id == ID_DFU_REPLY && n > 0 && p[0] == REPLY_OK)
		return S08_DFU_OK;
	return S08_DFU_FAIL;		/* an empty payload too (S-16.185) */
}

/* S-08.045: base is device.product_base */
uint32_t s08_product_word(uint32_t base, uint32_t sku, bool read_ok)
{
	if (!read_ok)
		return 0;
	return base | (sku <= 999u ? sku : 0u);
}

/* S-08.048, S-11.070: the prepare is the first request after the query. */
bool s08_after_prepare(const struct s08_failure *f)
{
	return f->at >= S08_AT_PREPARE;
}

/* S-08.043 */
void s08_prepare_payload(const uint16_t version[4], uint8_t type,
			 uint32_t product, uint8_t out[14])
{
	out[0] = S08_DFU_PREPARE;
	for (unsigned i = 0; i < 4; i++)
		wb_put_be16(out + 1 + 2 * i, version[i]);
	out[9] = type;
	wb_put_be32(out + 10, product);
}

/* S-08.044: S-11.004's layout, device id 0x03, address 0x0001F000 */
void s08_te_header(const uint8_t *image, uint32_t len,
		   const uint16_t version[4], uint32_t product,
		   s08_crc16_fn crc, uint8_t out[S08_HEADER_LEN])
{
	wb_fill(out, 0, S08_HEADER_LEN);
	wb_put_be32(out + 0x00, 0xBEEFCAFEu);
	out[0x04] = 0x03;
	wb_put_be32(out + 0x05, len);
	wb_put_be16(out + 0x09, crc(image, len));
	for (unsigned i = 0; i < 4; i++)
		wb_put_be16(out + 0x0B + 2 * i, version[i]);
	wb_put_be32(out + 0x13, 0x0001F000u);
	wb_put_be32(out + 0x17, product);
}

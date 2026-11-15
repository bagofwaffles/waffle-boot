/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/s17_revisions.h"

/* ---- 1. The revision strap ---- */

unsigned s17_strap_index(unsigned pa8, unsigned pa0)
{
	return 2u * (pa8 & 1u) + (pa0 & 1u);
}

/* S-17.001 */
uint8_t s17_decode_strap(unsigned pa8, unsigned pa0)
{
	static const uint8_t table[4] = { S17_NO_REVISION, 4, 5, 6 };

	return table[s17_strap_index(pa8, pa0)];
}

/* S-17.003, S-16.170 */
enum s17_panel_reset s17_panel_reset_path(unsigned pa8)
{
	return (pa8 & 1u) ? S17_PANEL_RESET_HANDSHAKE_OR_PULSE :
			    S17_PANEL_RESET_AT_RELEASE;
}

/* ---- 2. The reported revision ---- */

/* S-17.012: nothing survives a reset; the new strap sample is decoded. */
void s17_rev_power_on(struct s17_rev *r, unsigned pa8, unsigned pa0)
{
	r->decoded = s17_decode_strap(pa8, pa0);
	/* S-17.010: no revision can be reported before the metadata read
	 * (host commands come only in held mode, after it); the decoded value
	 * stands until then. */
	r->reported = r->decoded;
	r->meta = S17_META_UNREAD;
}

void s17_rev_metadata(struct s17_rev *r, enum s17_meta_state outcome)
{
	r->meta = outcome;
	switch (outcome) {
	case S17_META_READ:
		r->reported = r->decoded;	/* S-17.010 */
		break;
	case S17_META_RECORD_FAILED:		/* S-17.015 */
	case S17_META_SERIAL_FAILED:
		r->reported = 0;
		break;
	case S17_META_UNREAD:
	default:
		break;
	}
}

/* S-17.011 */
bool s17_rev_greeting(struct s17_rev *r, const uint8_t *payload, size_t len,
		      uint8_t *byte)
{
	/* Ignored as a whole while the record has not been read (S-16.098),
	 * and for the rest of a power-on whose record read failed
	 * (S-17.015). */
	if (r->meta != S17_META_READ && r->meta != S17_META_SERIAL_FAILED)
		return false;
	if (len < 2)
		return false;
	r->reported = payload[1];
	*byte = payload[1];
	return true;
}

uint8_t s17_rev_reported(const struct s17_rev *r)
{
	return r->reported;
}

/* S-17.013 */
size_t s17_hw_rev_entry(uint8_t rev, char *buf, size_t cap)
{
	static const char key[] = "hw_rev:";
	char tmp[S17_HW_REV_ENTRY_MAX];
	size_t n = sizeof(key) - 1;

	for (size_t i = 0; i < n; i++)
		tmp[i] = key[i];
	n += s11_u32_text(rev, tmp + n, sizeof(tmp) - n);
	if (n > cap)
		return 0;
	for (size_t i = 0; i < n; i++)
		buf[i] = tmp[i];
	return n;
}

/* S-17.014 */
void s17_hex2(uint8_t byte, char out[2])
{
	static const char digits[] = "0123456789abcdef";

	out[0] = digits[byte >> 4];
	out[1] = digits[byte & 0x0Fu];
}

/* ---- 3. Coprocessor versions ---- */

/* S-17.020 */
void s17_copro_init(struct s17_copro *c)
{
	static const struct s11_version zero = { 0, 0, 0, 0 };

	c->psoc = zero;
	c->nordic_app = zero;
	c->nordic_bl = zero;
	c->psoc_serial_len = 0;
}

static bool key_equals(const uint8_t *k, size_t len, const char *key)
{
	size_t i = 0;

	for (; i < len; i++) {
		if (key[i] == '\0' || (uint8_t)key[i] != k[i])
			return false;
	}
	return key[i] == '\0';
}

/* S-17.023: entries separated by ';', key before the first ':', exact key
 * match (S-16.098), the first matching entry wins. */
bool s17_kv_lookup(const uint8_t *payload, size_t len, const char *key,
		   const uint8_t **value, size_t *value_len)
{
	size_t start = 0;

	while (start <= len) {
		size_t end = start, colon = 0;
		bool has_colon = false;

		while (end < len && payload[end] != ';') {
			if (!has_colon && payload[end] == ':') {
				colon = end;
				has_colon = true;
			}
			end++;
		}
		if (has_colon && key_equals(payload + start, colon - start, key)) {
			*value = payload + colon + 1;
			*value_len = end - colon - 1;
			return true;
		}
		start = end + 1;
	}
	return false;
}

/* S-17.024: the first 63 characters; at least two '.' */
bool s17_version_value(const uint8_t *value, size_t len,
		       struct s11_version *out)
{
	return s11_version_parse(value, len, S11_VERSION_VALUE_WINDOW, out);
}

/* S-17.023, step 1: sw_version, else app_sw_version. A present but
 * unreadable sw_version gives no version: the fallback applies only when
 * there is no sw_version entry. */
static bool step1_version(const uint8_t *p, size_t len, struct s11_version *v)
{
	const uint8_t *val;
	size_t vlen;

	if (!s17_kv_lookup(p, len, "sw_version", &val, &vlen) &&
	    !s17_kv_lookup(p, len, "app_sw_version", &val, &vlen))
		return false;
	return s17_version_value(val, vlen, v);
}

void s17_copro_held_reply(struct s17_copro *c, enum s17_peer peer,
			  const uint8_t *payload, size_t len)
{
	struct s11_version v;
	const uint8_t *val;
	size_t vlen;

	switch (peer) {
	case S17_PEER_NORDIC:
		if (step1_version(payload, len, &v))
			c->nordic_app = v;
		break;
	case S17_PEER_PSOC:
		if (step1_version(payload, len, &v))
			c->psoc = v;
		if (s17_kv_lookup(payload, len, "serial", &val, &vlen)) {
			if (vlen > S17_SERIAL_MAX)	/* S-17.023 */
				vlen = S17_SERIAL_MAX;
			for (size_t i = 0; i < vlen; i++)
				c->psoc_serial[i] = (char)val[i];
			c->psoc_serial_len = (uint8_t)vlen;
		}
		break;
	default:
		break;
	}
}

/* S-17.023, step 2: sw_version only; without a readable one neither value
 * changes. */
void s17_copro_dfu_reply(struct s17_copro *c, const uint8_t *payload,
			 size_t len)
{
	struct s11_version app, bl;
	const uint8_t *val;
	size_t vlen;

	if (!s17_kv_lookup(payload, len, "sw_version", &val, &vlen) ||
	    !s17_version_value(val, vlen, &app))
		return;
	c->nordic_app = app;
	if (s17_kv_lookup(payload, len, "bl_version", &val, &vlen) &&
	    s17_version_value(val, vlen, &bl))
		c->nordic_bl = bl;
}

bool s17_reply_in_bootloader(const uint8_t *payload, size_t len)
{
	const uint8_t *val;
	size_t vlen;

	return s17_kv_lookup(payload, len, "mode", &val, &vlen) &&
	       key_equals(val, vlen, "bootloader");
}

static int exchange(const struct s17_link_ops *ops, void *ctx,
		    enum s17_peer peer, uint8_t *buf)
{
	int n = ops->info_exchange(ctx, peer, S17_REPLY_TIMEOUT_MS, buf,
				   S17_REPLY_MAX);

	return n > (int)S17_REPLY_MAX ? -1 : n;
}

/* S-17.021 step 1, S-17.022. S-17.042: the same exchanges whatever
 * version a coprocessor reports. */
void s17_copro_refresh(struct s17_copro *c, const struct s17_link_ops *ops,
		       void *ctx)
{
	uint8_t buf[S17_REPLY_MAX];
	int n;

	n = exchange(ops, ctx, S17_PEER_NORDIC, buf);
	if (n >= 0)
		s17_copro_held_reply(c, S17_PEER_NORDIC, buf, (size_t)n);
	n = exchange(ops, ctx, S17_PEER_PSOC, buf);
	if (n >= 0)
		s17_copro_held_reply(c, S17_PEER_PSOC, buf, (size_t)n);
}

/* S-17.021 step 2, S-17.022 */
bool s17_copro_dfu_read(struct s17_copro *c, const struct s17_link_ops *ops,
			void *ctx)
{
	uint8_t buf[S17_REPLY_MAX];
	int n;

	n = exchange(ops, ctx, S17_PEER_NORDIC, buf);
	if (n < 0 || !s17_reply_in_bootloader(buf, (size_t)n))
		return false;
	n = exchange(ops, ctx, S17_PEER_NORDIC, buf);
	if (n >= 0)
		s17_copro_dfu_reply(c, buf, (size_t)n);
	return true;
}

/* S-17.032: never the forced marker */
bool s17_own_version_ok(const struct s11_version *v)
{
	return !s11_version_is_forced_marker(v);
}

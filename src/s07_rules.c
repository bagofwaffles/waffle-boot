/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07 rules without I/O.
 */
#include "waffle/s07_rules.h"
#include "waffle/bytes.h"

/* S-07.032, S-16.008 */
void s07_key_status_decode(const uint8_t *p, size_t len,
			   struct s07_key_status *ks)
{
	uint8_t b[4] = { 0, 0, 0, 0 };

	if (!p) {
		/* no reply within 100 ms: held, debug not held */
		ks->replied = false;
		ks->hold = true;
		ks->debug = false;
		ks->precomp = false;
		return;
	}
	wb_copy(b, p, len < 4 ? len : 4);
	ks->replied = true;
	ks->hold = (b[3] & 0x01u) || (b[1] & 0x20u);
	ks->debug = (b[1] & 0x0Cu) != 0;
	ks->precomp = (b[2] & 0x80u) != 0;
}

/* S-07.033: the 72 keys, payload bytes 0 to 8; a byte the reply does not
 * carry reads 00 (S-07.079); byte 9 and later are not read. */
unsigned s07_chord_digit(const uint8_t *p, size_t len)
{
	uint8_t b[S07_BITMAP] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };

	if (!p)
		return 0;
	wb_copy(b, p, len < S07_BITMAP ? len : S07_BITMAP);
	b[2] &= 0x7Fu;			/* precomp goes with the chord */
	if (!s07_key_down(b, S07_CHORD_KEY))
		return 0;
	b[S07_CHORD_KEY >> 3] &= (uint8_t)~(1u << (S07_CHORD_KEY & 7u));
	for (unsigned i = 0; i < S07_BITMAP; i++) {
		if (b[i] != 0u)
			return 0;	/* another key, hold or debug */
	}
	return 4u;
}

/* S-07.051 */
bool s07_power_off_trigger(unsigned prev, unsigned s)
{
	return s == S07_POWER_OFF ||
	       (s == S07_POWER_CHARGE && prev == S07_POWER_ON);
}

/* S-07.053 */
void s07_battery_decode(const uint8_t *p, size_t len, struct s07_battery *b)
{
	uint8_t v[7] = { 0, 0, 0, 0, 0, 0, 0 };

	wb_copy(v, p, len < 7 ? len : 7);
	if (v[0] == 0) {	/* removed: every value cleared */
		b->present = false;
		b->charging = false;
		b->charge = 0;
		b->temp = 0;
		b->ma = 0;
		b->mv = 0;
		return;
	}
	b->present = true;
	b->charging = v[0] == 0x02;
	b->charge = v[1];
	b->temp = v[2];
	b->ma = wb_get_be16(v + 3);
	b->mv = wb_get_be16(v + 5);
}

bool s07_battery_usable(const struct s07_battery *b)
{
	return b->present && b->charge > 5 && b->mv > 3500;
}

/* S-07.042 */
void s07_charge_led(unsigned state, uint8_t charge, uint8_t rgb[3],
		    bool *forced)
{
	static const uint8_t red[3] = { 0xC8, 0, 0 }, green[3] = { 0, 0xC8, 0 },
			     off[3] = { 0, 0, 0 };
	const uint8_t *v = off;

	*forced = false;
	if (state == S07_POWER_ON) {
		v = charge < 15 ? red : green;
	} else if (state == S07_POWER_CHARGE) {
		v = charge > 90 ? green : red;
		*forced = true;
	}
	wb_copy(rgb, v, 3);
}

/* S-07.055 */
uint32_t s07_vbus_update(uint32_t applied, uint32_t target, uint8_t st,
			 uint8_t n, int *send_t)
{
	uint32_t t = target / 10u;	/* rounded down: 2 mA asks for nothing */

	*send_t = -1;
	if (st != 0)
		return applied;
	if (n < t) {
		/* S-07.055: every target of S-07.057 is at most 1000 mA, so t
		 * fits the byte; the clamp only guards a caller's mistake */
		*send_t = (int)(t > 0xFFu ? 0xFFu : t);
		return (uint32_t)*send_t * 10u;
	}
	return (uint32_t)n * 10u;
}

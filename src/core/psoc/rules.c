/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07 rules without I/O.
 */
#include "waffle/psoc/rules.h"
#include "waffle/util/bytes.h"
#include "waffle/util/div64.h"

/* A key of dev's table down in b; WB_KEY_NONE never is. */
static bool key_set(const struct wb_device *dev, const uint8_t *b, unsigned k)
{
	return k != WB_KEY_NONE && k < dev->keys.count && s07_key_down(b, k);
}

static void clear_key(uint8_t *b, unsigned k)
{
	b[k >> 3] &= (uint8_t)~(1u << (k & 7u));
}

void s07_keys_mask(const struct wb_device *dev, uint8_t b[S07_BITMAP])
{
	for (unsigned k = dev->keys.count; k < 8u * S07_BITMAP; k++)
		clear_key(b, k);
}

/* The reply's keys: bitmap_bytes bytes, a byte it does not carry 00
 * (S-07.079), the bytes after them and the bits past the last key not read. */
static void reply_keys(const struct wb_device *dev, const uint8_t *p, size_t len,
		       uint8_t b[S07_BITMAP])
{
	const size_t n = dev->keys.bitmap_bytes < S07_BITMAP ? dev->keys.bitmap_bytes : S07_BITMAP;

	wb_fill(b, 0, S07_BITMAP);
	wb_copy(b, p, len < n ? len : n);
	s07_keys_mask(dev, b);
}

/* S-07.032, S-16.008 */
void s07_key_status_decode(const struct wb_device *dev, const uint8_t *p, size_t len,
			   struct s07_key_status *ks)
{
	const struct wb_dev_keys *k = &dev->keys;
	uint8_t b[S07_BITMAP];
	bool d0, d1;

	if (!p) {
		/* no reply within 100 ms: held, debug not held */
		ks->replied = false;
		ks->hold = true;
		ks->debug = false;
		ks->precomp = false;
		return;
	}
	reply_keys(dev, p, len, b);
	ks->replied = true;
	ks->hold = key_set(dev, b, k->hold[0]) || key_set(dev, b, k->hold[1]);
	d0 = key_set(dev, b, k->debug[0]);
	d1 = key_set(dev, b, k->debug[1]);
	ks->debug = k->debug_both ? d0 && d1 : d0 || d1;
	ks->precomp = key_set(dev, b, k->precomp);
}

/* S-07.033: the device's keys, payload bytes 0 to bitmap_bytes - 1; a byte
 * the reply does not carry reads 00 (S-07.079); later bytes are not read. */
unsigned s07_chord_digit(const struct wb_device *dev, const uint8_t *p, size_t len)
{
	const unsigned chord = dev->keys.recovery;
	uint8_t b[S07_BITMAP];

	if (!p)
		return 0;
	reply_keys(dev, p, len, b);
	if (dev->keys.precomp != WB_KEY_NONE)
		clear_key(b, dev->keys.precomp);	/* precomp goes with the chord */
	if (!key_set(dev, b, chord))
		return 0;
	clear_key(b, chord);
	for (unsigned i = 0; i < S07_BITMAP; i++) {
		if (b[i] != 0u)
			return 0;	/* another key, hold or debug */
	}
	return wb_key_digit(dev, chord);
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

/* sin over a quarter turn, 128 steps, x 65536; the 129th, 65536, is SINE_TOP */
static const uint16_t quarter_sine[128] = {
	0u, 804u, 1608u, 2412u, 3216u, 4019u, 4821u, 5623u,
	6424u, 7224u, 8022u, 8820u, 9616u, 10411u, 11204u, 11996u,
	12785u, 13573u, 14359u, 15143u, 15924u, 16703u, 17479u, 18253u,
	19024u, 19792u, 20557u, 21320u, 22078u, 22834u, 23586u, 24335u,
	25080u, 25821u, 26558u, 27291u, 28020u, 28745u, 29466u, 30182u,
	30893u, 31600u, 32303u, 33000u, 33692u, 34380u, 35062u, 35738u,
	36410u, 37076u, 37736u, 38391u, 39040u, 39683u, 40320u, 40951u,
	41576u, 42194u, 42806u, 43412u, 44011u, 44604u, 45190u, 45769u,
	46341u, 46906u, 47464u, 48015u, 48559u, 49095u, 49624u, 50146u,
	50660u, 51166u, 51665u, 52156u, 52639u, 53114u, 53581u, 54040u,
	54491u, 54934u, 55368u, 55794u, 56212u, 56621u, 57022u, 57414u,
	57798u, 58172u, 58538u, 58896u, 59244u, 59583u, 59914u, 60235u,
	60547u, 60851u, 61145u, 61429u, 61705u, 61971u, 62228u, 62476u,
	62714u, 62943u, 63162u, 63372u, 63572u, 63763u, 63944u, 64115u,
	64277u, 64429u, 64571u, 64704u, 64827u, 64940u, 65043u, 65137u,
	65220u, 65294u, 65358u, 65413u, 65457u, 65492u, 65516u, 65531u,
};
#define SINE_TOP 65536u

static uint32_t sine_step(uint32_t i)
{
	return i < 128u ? quarter_sine[i] : SINE_TOP;
}

/* |sin| x 65536 for x in a quarter turn of 2^30, linearly between steps. */
static uint32_t quarter(uint32_t x)
{
	const uint32_t i = x >> 23, frac = (x >> 7) & 0xFFFFu;
	uint32_t a;

	if (i >= 128u)
		return SINE_TOP;
	a = quarter_sine[i];
	return a + (uint32_t)(((uint64_t)(sine_step(i + 1u) - a) * frac) >> 16);
}

/* 0.3 turns (0.6 pi rad) a second: 2^32 x 0.3 per 10^6 us */
#define BREATHE_PER_MS 1288490u

uint32_t s07_breathe_step(uint32_t phase, uint32_t d_us)
{
	if (d_us > S07_BREATHE_GAP_US)
		return phase;
	return phase + (uint32_t)wb_udiv64((uint64_t)d_us * BREATHE_PER_MS, 1000u);
}

uint8_t s07_breathe_level(uint8_t base, uint32_t phase)
{
	const uint32_t q = phase >> 30, x = phase & 0x3FFFFFFFu;
	const uint32_t s = quarter((q & 1u) ? 0x40000000u - x : x);
	/* f x 2^16 = 0.625 x 2^16 +- 0.375 x sin x 2^16 */
	const uint32_t d = (uint32_t)(((uint64_t)s * 24576u) >> 16);
	const uint32_t f = q < 2u ? 40960u + d : 40960u - d;

	return (uint8_t)(((uint32_t)base * f) >> 16);
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

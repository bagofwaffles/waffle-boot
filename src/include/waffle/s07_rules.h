/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07 rules without I/O: key bitmaps and key status, the LED change filter
 * and the charge LED, power states, the battery report, the VBUS update and
 * the PMIC write value.
 */
#ifndef WAFFLE_S07_RULES_H
#define WAFFLE_S07_RULES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/device.h"

/* S-07.030: the most keys and bitmap bytes of any device (the OP-XY's);
 * a device's are device.key.count and device.key.bitmap_bytes. */
#define S07_KEYS 72			/* numbers 0x00-0x47 */
#define S07_BITMAP 9

/* Power states (S-07.050). */
#define S07_POWER_ON 0x10u
#define S07_POWER_OFF 0x14u
#define S07_POWER_CHARGE 0x16u
#define S07_POWER_NONE 0x100u		/* no report yet */

struct s07_key_status {
	bool replied;	/* false: no reply within 100 ms (S-07.033 unreadable) */
	bool hold;
	bool debug;
	bool precomp;
};

/* S-07.033: the OP-XY's one chord key, S-15.150's digit 4 (Power + M4); a
 * device's is device.key.recovery. */
#define S07_CHORD_KEY 0x09u

struct s07_battery {
	bool present;
	bool charging;
	uint8_t charge;		/* % */
	uint8_t temp;		/* deg C */
	uint16_t ma;
	uint16_t mv;
};

/* S-07.031: is key k down in a bitmap? */
static inline bool s07_key_down(const uint8_t *bitmap, unsigned k)
{
	return ((unsigned)bitmap[k >> 3] >> (k & 7u) & 1u) != 0;
}

/*
 * S-07.032: decode a 0x41 reply payload (NULL: no reply) by dev's
 * device.key.hold (either), device.key.debug (either, or both with
 * debug_both) and device.key.precomp, over its bitmap_bytes bytes; a byte
 * the reply does not carry reads 00.
 */
void s07_key_status_decode(const struct wb_device *dev, const uint8_t *p, size_t len,
			   struct s07_key_status *ks);

/*
 * S-07.033: the chord a 0x41 reply payload names: the digit of
 * device.key.recovery (4) when its bit is set and every other of the
 * device.key.count keys (bytes 0 to bitmap_bytes - 1) is up but precomp;
 * else 0. Bits a short payload does not carry are clear, and bits past the
 * last key are not read; NULL (no reply: unreadable) is 0. The front reads
 * it (S-02.084); the main image reads no chord.
 */
unsigned s07_chord_digit(const struct wb_device *dev, const uint8_t *p, size_t len);

/* S-07.030, S-07.079: keep dev's keys of a bitmap of S07_BITMAP bytes, with
 * every bit from device.key.count on cleared. */
void s07_keys_mask(const struct wb_device *dev, uint8_t b[S07_BITMAP]);

/* S-07.051: does state s, after prev (S07_POWER_NONE: none), power off? */
bool s07_power_off_trigger(unsigned prev, unsigned s);

/* S-07.053: record a 0x37 payload (7 bytes; a shorter one is zero-padded). */
void s07_battery_decode(const uint8_t *p, size_t len, struct s07_battery *b);

/* S-07.053: present, charge above 5 %, voltage above 3500 mV. */
bool s07_battery_usable(const struct s07_battery *b);

/* S-07.042: the RGB value for a power state and charge; *forced set. */
void s07_charge_led(unsigned state, uint8_t charge, uint8_t rgb[3],
		    bool *forced);

/*
 * S-07.047, the OP-1F's breathing. The phase is a fraction of a turn, a
 * turn being 2^32; s07_breathe_step() is the phase after d_us of 0.3 turns
 * (0.6 pi rad) per second (more than S07_BREATHE_GAP_US: no step; exactly
 * S07_BREATHE_GAP_US steps), s07_breathe_level()
 * base x (0.25 + 0.75 x (0.5 + 0.5 x sin phase)) with the fraction dropped.
 */
#define S07_BREATHE_GAP_US 100000u
uint32_t s07_breathe_step(uint32_t phase, uint32_t d_us);
uint8_t s07_breathe_level(uint8_t base, uint32_t phase);

/*
 * S-07.055: a 0x33 reply {st n} against the target (mA). Returns the new
 * applied limit (mA); *send_t is set to t (10 mA units) when a 0x34 {00 t}
 * is to be sent, else to -1.
 */
uint32_t s07_vbus_update(uint32_t applied, uint32_t target, uint8_t st,
			 uint8_t n, int *send_t);

/* S-07.061: the write value for a read value v. */
static inline uint8_t s07_pmic_write_value(uint8_t v)
{
	return (uint8_t)(v | 0x60u);
}

#endif

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

#define S07_KEYS 72			/* S-07.030: numbers 0x00-0x47 */
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

/* S-07.033: the one chord key, S-15.150's digit 4 (Power + M4). */
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

/* S-07.032: decode a 0x41 reply payload (NULL: no reply). */
void s07_key_status_decode(const uint8_t *p, size_t len,
			   struct s07_key_status *ks);

/*
 * S-07.033: the chord a 0x41 reply payload names: 4 when
 * the chord key's bit is set and every other of the 72 keys (bytes 0 to 8)
 * is up but precomp; else 0. Bits a short payload does not carry are clear; NULL
 * (no reply: unreadable) is 0. The front reads it (S-02.084); the main
 * image reads no chord.
 */
unsigned s07_chord_digit(const uint8_t *p, size_t len);

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

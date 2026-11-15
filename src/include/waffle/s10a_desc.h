/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_S10A_DESC_H
#define WAFFLE_S10A_DESC_H

/*
 * S-10a: the device's identity and its nine descriptor
 * sets, built as bytes for either speed. Independent of the USB stack.
 *
 * String indexes are free (S-10a.005); these follow the vectors: each
 * string field takes the next index in the order the
 * descriptors are listed (device, then each configuration), from 1.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* S-10a.010 */
enum s10a_set {
	S10A_SET_MIDI,
	S10A_SET_MIDI_CDC,
	S10A_SET_DISK,
	S10A_SET_DISK_CDC,
	S10A_SET_MTP,
	S10A_SET_MTP_VENDOR,
	S10A_SET_SERIAL,
	S10A_SET_MTP_CDC,
	S10A_SET_MTP_VENDOR_CDC,
	S10A_SET_COUNT,
};

/* The held-mode states of S-10a.010's table (S-15 says when each starts). */
enum s10a_state {
	S10A_STATE_MAIN,		/* the main screen */
	S10A_STATE_UPGRADE_40,		/* upgrade screen, reboot reason 40 */
	S10A_STATE_UPGRADE,		/* upgrade screen otherwise */
	S10A_STATE_UPGRADE_KEEP,	/* resumed or forced upgrade */
	S10A_STATE_BATTERY,		/* the battery wait (S-16.017) */
	S10A_STATE_FACTORY_MTP_VENDOR,	/* factory menu, vendor MTP item on */
	S10A_STATE_FACTORY_MTP,		/* factory menu, standard MTP item on */
	S10A_STATE_FACTORY_MTP_OFF,	/* factory menu, MTP turned off again */
};

/* What a set carries (S-10a.013). */
enum s10a_function {
	S10A_FN_MIDI,
	S10A_FN_CDC,
	S10A_FN_MSC,
	S10A_FN_MTP,
	S10A_FN_MTP_VENDOR,
};

#define S10A_SERIAL_LEN		8u	/* S-04.040 */
#define S10A_CONFIG_MAX		256u	/* bytes; the largest set needs 175 */
#define S10A_STRING_MAX		66u	/* bytes of a string descriptor */
#define S10A_MAX_CONFIGS	3u
#define S10A_MAX_INTERFACES	4u

/* The endpoints of the sets (S-10a.013). */
#define S10A_EP_CDC_NOTIFY	0x81u
#define S10A_EP_MTP_EVENT	0x83u
#define S10A_EP_BULK_IN		0x87u	/* MIDI, mass storage, MTP with CDC */
#define S10A_EP_BULK_OUT	0x08u
#define S10A_EP_CDC_IN		0x89u	/* CDC data, and MTP alone */
#define S10A_EP_CDC_OUT		0x0Au

/*
 * S-10a.004: the serial number from the metadata record's serial (`n`
 * bytes, at most 8 used), cut at the first NUL, every byte outside
 * 0x20-0x7E made '0'. `out` gets the text ("" when there is none).
 */
void s10a_serial_from_meta(const uint8_t *meta, size_t n,
			   char out[S10A_SERIAL_LEN + 1]);

/*
 * S-10a.010 and S-10a.011: the set a state starts. The serial flag adds the
 * CDC port to the MIDI and disk states; developer mode, which sets the flag
 * too, also adds it to the MTP states. The battery wait never has it. False
 * when the state starts no set (a resumed or forced upgrade: the set
 * presented stays).
 */
bool s10a_set_for_state(enum s10a_state state, bool serial_flag,
			bool dev_mode, enum s10a_set *set);

/* bNumConfigurations (S-10a.002) */
unsigned s10a_desc_num_configs(enum s10a_set set);
/* bNumInterfaces */
unsigned s10a_desc_num_interfaces(enum s10a_set set);
/* The function interface `itf` belongs to and the function's first
 * interface; false when the set has no such interface. */
bool s10a_desc_interface(enum s10a_set set, unsigned itf,
			 enum s10a_function *fn, unsigned *first);
/* Whether `addr` (an endpoint address other than 0) is in the set. */
bool s10a_desc_has_endpoint(enum s10a_set set, uint8_t addr);
/* bMaxPower of configuration `value` (1-based), 0 when there is none. */
unsigned s10a_desc_max_power(enum s10a_set set, unsigned value);

/* The device descriptor (18 bytes) and the qualifier (10 bytes). */
void s10a_desc_device(enum s10a_set set, bool has_serial, uint8_t out[18]);
void s10a_desc_qualifier(enum s10a_set set, uint8_t out[10]);

/*
 * Configuration `index` (0-based) as it is at the given speed, or its
 * other-speed form (bDescriptorType 7 with the other speed's sizes,
 * S-10a.021). Returns wTotalLength, 0 when the index is out of range.
 */
size_t s10a_desc_config(enum s10a_set set, bool has_serial, unsigned index,
			bool high, bool other_speed,
			uint8_t out[S10A_CONFIG_MAX]);

/*
 * String descriptor `index` (S-10a.005): index 0 is the LANGID list.
 * Returns its length, 0 when the set names no such index (a STALL).
 */
size_t s10a_desc_string(enum s10a_set set, const char *serial,
			unsigned index, uint8_t out[S10A_STRING_MAX]);
/* The text of string `index` (1-based), NULL when the set names none. */
const char *s10a_desc_text(enum s10a_set set, const char *serial,
			   unsigned index);

#endif /* WAFFLE_S10A_DESC_H */

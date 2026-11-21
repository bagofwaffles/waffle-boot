/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The devices (S-18), the front's own copy of what it needs of each: the
 * first stage's fingerprint row (S-18.003), the main CPU's pins (S-13b.003,
 * S-13b.004, S-13b.006), the panel's D/C pin (S-13c.001), the keys it
 * reads (S-07.032, S-07.033, S-02 §9), the LED forms and the features it
 * acts on (S-18.020), and recovery's USB identity (S-02.093). The full
 * bootloader's table is src/device.c; test_front_device checks that the
 * two agree.
 */
#ifndef FR_DEVICE_H
#define FR_DEVICE_H

#include "fr.h"

/* The record's codes (fr_handoff.h): FR_HO_DEV_OPXY, FR_HO_DEV_OP1F. */
#define FR_DEV_COUNT 2u

/* S-18.020, the features the front acts on. */
#define FR_DEV_F_STRAPS   0x01u	/* the PA0/PA2/PA8 strap sample */
#define FR_DEV_F_GAMMA    0x02u	/* the panel's digital gamma (S-13c.012) */
#define FR_DEV_F_KEY_LEDS 0x04u	/* S-07.040's LED forms; else S-07.046's */

/* S-13b.003, S-13b.007: PORTB's MUX and FER, the same on every device, and
 * the pins of the flash bus among them (SCK, MISO, MOSI, SEL1), each SPI2's
 * function in multiplexer position 0 with its FER bit set. */
#define FR_PORTB_MUX       0x1400FFFFu
#define FR_PORTB_FER       0xFFFFu
#define FR_FLASH_BUS_PINS  (1u << 10 | 1u << 11 | 1u << 12 | 1u << 15)
#define FR_FLASH_BUS_MUX   0xC3F00000u	/* their 2-bit MUX fields */

/* A GPIO set-up (S-13b.006). */
#define FR_GPIO_IN   0u
#define FR_GPIO_LOW  1u
#define FR_GPIO_HIGH 2u
#define FR_GPIO_KEEP 3u		/* an output at the level it had: no level written */

struct fr_dev_gpio {
	uint8_t pin;		/* WB_PIN(port, bit) */
	uint8_t kind;		/* FR_GPIO_* */
};

/* A key number no key has. */
#define FR_KEY_NONE 0xFFu
/* The most bytes of a key-status reply any device's keys take. */
#define FR_DEV_KEY_BYTES_MAX 9u

struct fr_dev {
	uint8_t id;			/* FR_HO_DEV_* */
	uint8_t flags;			/* FR_DEV_F_* */
	uint32_t fs_n, fs_c;		/* its row of S-18.003's table */
	/* S-13b.003: MUX and FER per port; S-13b.004: the pins made inputs
	 * before the first MUX write */
	uint32_t mux[3];
	uint16_t fer[3];
	const uint8_t *inputs_first;
	uint8_t inputs_first_n;
	/* S-13b.006: the GPIOs the front sets, in order */
	const struct fr_dev_gpio *gpio;
	uint8_t gpio_n;
	uint8_t panel_dc;		/* S-13c.001's D/C, a pin of port A */
	/* keys (S-18.030) */
	uint8_t key_count;		/* keys 0 to key_count - 1 */
	uint8_t key_bytes;		/* the bitmap's bytes */
	uint8_t hold[2];		/* device.key.hold, either one */
	uint8_t precomp;		/* not counted by a reading, or FR_KEY_NONE */
	uint8_t key_back;		/* digit 8: recovery's Back */
	uint8_t key_recovery;		/* the chord's key, held alone */
	/* recovery's port (S-02.093) */
	uint16_t usb_pid;
	const char *usb_product;
};

extern const struct fr_dev fr_dev_opxy;
extern const struct fr_dev fr_dev_op1f;

/* The device of a record code; the default (S-18.004) for any other. */
const struct fr_dev *fr_dev_of(unsigned id);
/* dev, or the default device for NULL. */
const struct fr_dev *fr_dev_or_default(const struct fr_dev *dev);

static inline bool fr_dev_has(const struct fr_dev *dev, unsigned flag)
{
	return (dev->flags & flag) != 0u;
}

/* S-18.003: the argument's offset, the block header's size, the largest n. */
#define FR_FS_ARG_OFF 0x0Cu
#define FR_FS_HEAD    16u
#define FR_FS_N_MAX   0xC000u

/* What a main CPU decided (S-18.003, S-18.004), and the fingerprint
 * read: n the argument plus 16 (0xFFFFFFFF when that does not fit, 0 when
 * the first 16 bytes were not read), c the CRC-32 of [0, n) when
 * 16 < n <= 0xC000 and the read succeeded, else 0. */
struct fr_ident {
	const struct fr_dev *dev;
	uint8_t how;			/* FR_HO_HOW_TABLE or FR_HO_HOW_UNKNOWN */
	uint32_t n, c;
};

/* S-18.003: the most tries of the whole fingerprint read. */
#define FR_FS_TRIES 3u

/*
 * S-18.003, S-18.004: reads the first stage through flash, each byte of
 * [0, n) once per try and nothing else, writes nothing, and names the
 * device whose row holds (n, c); with no such row or n out of range, the
 * default device, how FR_HO_HOW_UNKNOWN. A further try, up to FR_FS_TRIES,
 * follows a try whose read failed or whose (n, c) is in no row, never one
 * that returned FR_FLASH_BUSY (its wait already ran its bound). The first
 * try that names a row decides; otherwise n and c are the last try's.
 */
void fr_identify(const struct fr_flash *flash, struct fr_ident *out);

/* RCU0_STAT.BMODE of a main CPU (S-18.002): SPI master boot. */
#define FR_BOOT_MODE_MAIN 1u
/* PA2, the role strap, in a sample. */
#define FR_STRAP_PA2 (1u << 2)

/* The board's side of the decision. Each is called at most once, in the
 * order fr_decide() states. */
struct fr_decide_hw {
	unsigned (*boot_mode)(void *ctx);	/* RCU0_STAT.BMODE */
	/* S-18.002's fallback sample: PA2's INEN alone, PA2 in FR_STRAP_PA2 */
	uint16_t (*pa2)(void *ctx);
	uint16_t (*strap)(void *ctx);		/* S-13b.001's sample */
	/* the flash bus on its pins (PORTB_MUX and PORTB_FER alone) and SPI2
	 * ready for the first stage's read */
	void (*flash_bus)(void *ctx);
	const struct fr_flash *flash;
	void *ctx;
};

/* What fr_decide() found. */
struct fr_role {
	bool main;		/* a main CPU; else the OP-XY's second CPU */
	bool by_strap;		/* boot mode not 1, the fallback read PA2 = 1 */
	bool strapped;		/* strap holds S-13b.001's sample; else 0 */
	uint16_t strap;
	struct fr_ident ident;	/* the device; FR_HO_HOW_SECOND on the second CPU */
};

/*
 * S-18.001, S-18.002: the boot mode first. Boot mode 1: a main CPU; the
 * flash bus, the first stage's read (fr_identify()), then the strap
 * sample where that device has straps. Another boot mode: the fallback
 * sample, the one before the flash read. PA2 = 0 is the second CPU, the
 * OP-XY, with no flash read, which then takes the strap sample as it
 * always has. PA2 = 1 is a main CPU all the same (by_strap: flags bit 12),
 * which then decides as with boot mode 1: a boot mode that does not read 1
 * on a master never costs the master its start.
 */
void fr_decide(const struct fr_decide_hw *hw, struct fr_role *out);

#endif /* FR_DEVICE_H */

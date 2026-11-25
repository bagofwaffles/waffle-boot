/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_BOOT_DEVICE_H
#define WAFFLE_BOOT_DEVICE_H

/*
 * S-18: the devices one stream serves. Each CPU decides its device once per
 * start (S-18.001 to S-18.004); the main image takes it from the front's
 * hand-off record (S-18.005). A module that differs by device is given a
 * const struct wb_device: there is no global.
 */

#include <stdbool.h>
#include <stdint.h>

enum wb_dev_id { WB_DEV_OPXY = 0, WB_DEV_OP1F = 1 };
#define WB_DEV_COUNT 2u

/* How the device was decided: the hand-off record's offset 153 (S-18.005). */
enum wb_dev_how { WB_DEV_BY_TABLE = 0, WB_DEV_SECOND_CPU = 1, WB_DEV_UNKNOWN = 2 };

/* S-18.020: one bit per feature. */
enum wb_feature {
	WB_F_SECOND_CPU = 1u << 0,
	WB_F_STRAPS = 1u << 1,
	WB_F_EMMC_ON_MAIN = 1u << 2,
	WB_F_PMIC_PASS_THROUGH = 1u << 3,
	WB_F_DIGITAL_GAMMA = 1u << 4,
	WB_F_KEY_LEDS = 1u << 5,
	WB_F_CONTENT_PACKS = 1u << 6,
	WB_F_FAT_REGION = 1u << 7,
	WB_F_FACTORY_WORD = 1u << 8,
	WB_F_CV_CALIBRATION = 1u << 9,
	WB_F_MTP_TWO_ITEMS = 1u << 10,
};
#define WB_FEATURE_COUNT 11u

/* A pin: port (0 = A, 1 = B, 2 = C) times 16 plus its bit, as WB_PIN of
 * the port driver. */
#define WB_DEV_PIN(port, bit) ((uint8_t)((port) * 16u + (bit)))
#define WB_DEV_PA(bit) WB_DEV_PIN(0u, bit)
#define WB_DEV_PB(bit) WB_DEV_PIN(1u, bit)
#define WB_DEV_PC(bit) WB_DEV_PIN(2u, bit)

/* device.gpio_outputs and device.gpio_inputs (S-18.030). */
enum wb_gpio_kind {
	WB_GPIO_IN = 0,
	WB_GPIO_OUT_LOW = 1,
	WB_GPIO_OUT_HIGH = 2,
	WB_GPIO_OUT_KEEP = 3,	/* an output at the level it had: no level written */
};
struct wb_dev_gpio {
	uint8_t pin;
	uint8_t kind;		/* enum wb_gpio_kind */
};

/* A key number (S-07.030); WB_KEY_NONE where a device has none. */
#define WB_KEY_NONE 0xFFu
#define WB_KEYS_MAX 72u
#define WB_BITMAP_MAX 9u

/* S-18.030, identity. */
struct wb_dev_identity {
	uint8_t product_number;		/* device.product_number */
	uint32_t product_base;		/* device.product_base */
	const char *product_text;	/* device.product_text */
	const char *sku_prefix;		/* device.sku_prefix */
	uint16_t usb_pid;		/* device.usb_pid */
	uint16_t midi_pid;		/* device.midi_pid */
	uint8_t midi_byte;		/* device.midi_byte */
	uint8_t midi_family[2];		/* device.midi_family */
	/* device.midi_member: these two bytes, or with member_from_sku the
	 * sku's (wb_midi_member()) and these when the sku is 0xFFFF */
	uint8_t midi_member[2];
	bool member_from_sku;
};

/* S-18.030, memory and media. */
struct wb_dev_memory {
	uint8_t metadata_size;		/* device.metadata_size */
	/* device.ramdisk_regions: one region at ramdisk_base, or free */
	bool ramdisk_one_region;
	uint32_t ramdisk_base, ramdisk_len;
	uint32_t ramdisk_blocks;	/* device.ramdisk_blocks */
	uint8_t emmc_width;		/* device.emmc_width: data lines */
	uint32_t emmc_sectors;		/* device.emmc_sectors */
	bool emmc_sectors_at_least;	/* false: exactly */
	/* device.reset_removes: NULL for S-15's list, else these paths and
	 * nothing else; the FAT region's erase goes with WB_F_FAT_REGION */
	const char *const *reset_only;
	uint8_t reset_only_n;
};

/* S-18.030, the main CPU's pins. */
struct wb_dev_pins {
	uint32_t mux[3];		/* device.porta_mux .. portc_mux */
	uint32_t fer[3];		/* device.porta_fer .. portc_fer */
	const struct wb_dev_gpio *gpio;	/* device.gpio_outputs, then gpio_inputs */
	uint8_t gpio_n;
	uint8_t panel_dc;		/* device.panel_dc */
};

/* S-18.030, keys. */
struct wb_dev_keys {
	uint8_t count;			/* device.key.count: keys 0 to count - 1 */
	uint8_t bitmap_bytes;		/* device.key.bitmap_bytes */
	uint8_t hold[2];		/* device.key.hold: either one */
	uint8_t debug[2];		/* device.key.debug */
	bool debug_both;		/* true: both held; false: either one */
	uint8_t precomp;		/* device.key.precomp, or WB_KEY_NONE */
	/* device.key.digit1 to digit4 (three keys each), then digit5to8 (one
	 * each, the rest WB_KEY_NONE) */
	uint8_t digit[8][3];
	uint8_t digit_knob;		/* the knob's click: its index in digit[0..3] */
	uint8_t shift;			/* device.key.shift */
	uint8_t confirm;		/* device.key.confirm */
	uint8_t com;			/* device.key.com, one of hold[] */
	uint8_t recovery;		/* device.key.recovery: the chord key */
};

struct wb_device {
	enum wb_dev_id id;
	const char *name;		/* "OP-XY", "OP-1F" */
	const char *device_id;		/* the info reply's device_id (S-18.031) */
	const char *contract;		/* the OS hand-off contract (S-05) */
	uint32_t features;		/* enum wb_feature bits */
	/* this device's row of S-18.003's table */
	uint32_t first_stage_n, first_stage_crc;
	struct wb_dev_identity ident;
	struct wb_dev_memory mem;
	struct wb_dev_pins pins;
	struct wb_dev_keys keys;
};

extern const struct wb_device wb_device_opxy;
extern const struct wb_device wb_device_op1f;

/* The device of id; the default (S-18.004) for an id no device has. */
const struct wb_device *wb_device_of(unsigned id);

/* The device the default of S-18.004 takes. */
#define WB_DEV_DEFAULT WB_DEV_OPXY
/* The info reply's device_id when the device is not known (S-18.031). */
#define WB_DEVICE_ID_UNKNOWN "unknown"

static inline bool wb_has(const struct wb_device *dev, enum wb_feature f)
{
	return (dev->features & (uint32_t)f) != 0u;
}

/* The digit (1 to 8) key names on dev (S-18.030), 0 for none. */
unsigned wb_key_digit(const struct wb_device *dev, unsigned key);

/* device.midi_member for a sku (S-18.030). */
void wb_midi_member(const struct wb_device *dev, uint16_t sku, uint8_t out[2]);

/* ---- S-18.002 to S-18.004: telling the devices apart ---- */

/* The boot mode of a main CPU, RCU0_STAT.BMODE's SPI master boot. */
#define WB_BOOT_MODE_MAIN 1u

/* S-18.003: the argument's offset, the first block header's size and the
 * largest n. */
#define WB_FS_ARG_OFF 0x0Cu
#define WB_FS_HEAD 16u
#define WB_FS_N_MAX 0xC000u

/* Reads len bytes of the SPI flash at off; 0 on success. The same form as
 * s02_flash_ops.read. */
typedef int (*wb_flash_read_fn)(void *ctx, uint32_t off, uint8_t *buf, uint32_t len);

struct wb_dev_decision {
	enum wb_dev_id id;
	enum wb_dev_how how;
};

/* S-18.003: the device whose row holds (n, c); false for none. */
bool wb_first_stage_lookup(uint32_t n, uint32_t c, enum wb_dev_id *id);

/*
 * S-18.003, S-18.004: the first-stage fingerprint, read through read (no
 * write), each byte of [0, n) once, and the device it names. *n is the
 * argument at 0x0C plus 16, 0xFFFFFFFF when that does not fit in 32 bits,
 * and 0 when the first 16 bytes could not be read. *c is the CRC-32 of
 * S-02.015 over [0, n) when 16 < n <= 0xC000 and the read succeeded, else
 * 0. A fingerprint in no row, out of range or not read gives the default
 * device with WB_DEV_UNKNOWN. _n makes up to tries tries (the rule below);
 * wb_first_stage_identify() one, S-18.004's later look.
 */
struct wb_dev_decision wb_first_stage_identify_n(wb_flash_read_fn read, void *ctx,
						 uint32_t *n, uint32_t *c, unsigned tries);
static inline struct wb_dev_decision wb_first_stage_identify(wb_flash_read_fn read, void *ctx,
							     uint32_t *n, uint32_t *c)
{
	return wb_first_stage_identify_n(read, ctx, n, c, 1u);
}

/*
 * S-18.003's tries, as the front's fr_identify() makes them: up to
 * WB_FS_TRIES whole reads, a further one only after a try whose read failed
 * or whose (n, c) is in no row, and never after a read that returned
 * WB_FS_READ_BUSY (the flash stayed busy: its wait ran its bound). The
 * first try that names a row decides; otherwise the default, not known,
 * with the last try's n and c.
 */
#define WB_FS_TRIES 3u
#define WB_FS_READ_BUSY (-2)	/* WB_SPIFLASH_BUSY */
static inline struct wb_dev_decision wb_first_stage_identify_tries(wb_flash_read_fn read, void *ctx,
								   uint32_t *n, uint32_t *c)
{
	return wb_first_stage_identify_n(read, ctx, n, c, WB_FS_TRIES);
}

/* S-18.002's fallback sample: PA2's INEN set and no other port bit, the
 * settling time, then PA2 read; it returns PA2 in WB_STRAP_PA2. */
#define WB_STRAP_PA2 (1u << 2)
typedef uint16_t (*wb_strap_fn)(void *ctx);

/*
 * S-18.001, S-18.002 as the front decides (fr_decide()): boot mode 1 is a
 * main CPU; another boot mode takes the fallback sample through pa2, PA2 = 0
 * the OP-XY's second CPU (no flash read), PA2 = 1 a main CPU all the same
 * (fallback: the record's flags bit 12, logged). A main CPU then reads the
 * first stage with the tries above. S-13b.001's own strap sample is the
 * caller's, after this.
 */
struct wb_dev_role {
	struct wb_dev_decision d;
	bool main, fallback;
	uint32_t n, c;
};
void wb_device_decide_role(unsigned boot_mode, wb_strap_fn pa2, void *pa2_ctx,
			   wb_flash_read_fn read, void *ctx, struct wb_dev_role *out);

#endif /* WAFFLE_BOOT_DEVICE_H */

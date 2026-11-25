/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-04: persistent records.  The reboot record and its reasons, the writers
 * of the record and their reset sequence, the CDC request file, the
 * metadata record's fields and write rule, the PSoC status reports, the RTC
 * encoding and the fatal-halt record.
 *
 * Records are passed as byte arrays; the reboot record's memory, the
 * flash, the PSoC link, the Nordic and the watchdog come in as pointers and
 * operations.  Nothing here touches hardware.
 */
#ifndef WAFFLE_S04_RECORDS_H
#define WAFFLE_S04_RECORDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/device.h"

/* --- The reboot record (S-04.001 to S-04.011) --- */

#define S04_REBOOT_ADDR    0x11B01F00u	/* S-04.001, in each core's L1 scratch */
#define S04_REBOOT_SIZE    8u
#define S04_REBOOT_MAGIC   0xDEADF00Du

#define S04_REASON_BOOT      0u
#define S04_REASON_CDC       10u
#define S04_REASON_DEFAULT   20u	/* also an absent record */
#define S04_REASON_MSC       30u
#define S04_REASON_MIDI      40u
#define S04_REASON_BOOT50    50u
#define S04_REASON_STAY      60u
/* S-04.012: boots as 20 does; the display frame memory holds the
 * calibration screen (S-05.016). Written only by W10. */
#define S04_REASON_OS_FRAME_CAL 70u
/* S-04.013: boots as 20 does, and start in boot menu (S-15.175) does not
 * apply to that one start. Written only by W11. */
#define S04_REASON_OS_ONCE 80u
/* S-04.014: boots as 80 does (start in boot menu does not apply), with the
 * OS image read from /yaffs2/os.ldr instead of the boot partition
 * (S-05.035). Written only by W12. */
#define S04_REASON_OS_FILE 90u
/* S-04.072: boots as 90 does, with the OS image read from the file of
 * /yaffs2/os at the list place reason - 100 (S-05.044, S-15.203): 100 is
 * the list's first file. Written only by W13. */
#define S04_REASON_OS_LIST 100u
#define S04_OS_LIST_PLACES 7u
#define S04_REASON_IS_OS_LIST(r) \
	((r) >= S04_REASON_OS_LIST && (r) < S04_REASON_OS_LIST + S04_OS_LIST_PLACES)

/* S-04.002: present when the magic is 0xDEADF00D. */
bool s04_reboot_present(const uint8_t rec[S04_REBOOT_SIZE]);
/* S-04.003: magic and reason at +0..+5; +6..+7 untouched. */
void s04_reboot_write(uint8_t rec[S04_REBOOT_SIZE], uint16_t reason);
/* S-04.004: the reason this boot takes; a present record's magic is zeroed. */
uint16_t s04_reboot_consume(uint8_t rec[S04_REBOOT_SIZE]);

enum s04_screen { S04_SCREEN_MAIN = 0, S04_SCREEN_UPGRADE };
enum s04_transport { S04_TRANSPORT_NONE = 0, S04_TRANSPORT_MSC, S04_TRANSPORT_MIDI };

struct s04_reason_effect {
	bool boot_os;			/* the reason does not stop the boot */
	enum s04_screen screen;		/* first screen in held mode */
	bool cdc;			/* the reason adds the CDC port */
	enum s04_transport transport;	/* upgrade transport */
};

/* S-04.006 to S-04.011, the full 16-bit value. */
void s04_reason_effect(uint16_t reason, struct s04_reason_effect *out);

/* S-04.021: the reason for a host reboot request's payload (mode byte first). */
uint16_t s04_host_mode_reason(const uint8_t *payload, size_t len);

/* --- Writers of the record (S-04.020 to S-04.029) --- */

/* W5 and W8 are withdrawn; the numbers stay S-04.020's. */
enum s04_writer { S04_W1 = 1, S04_W2, S04_W3, S04_W4, S04_W6 = 6, S04_W7, S04_W9 = 9,
		  S04_W10, S04_W11, S04_W12, S04_W13 };
enum s04_nordic { S04_NORDIC_NONE = 0, S04_NORDIC_ENTER_DFU, S04_NORDIC_EXIT_DFU };
enum s04_reset { S04_RESET_HARD = 0, S04_RESET_SOFT };

struct s04_writer_rule {
	uint16_t reason;
	enum s04_reset reset;
	enum s04_nordic nordic;
	bool shutdown;			/* the shutdown of S-04.063 runs */
	bool psoc_reset_request;	/* S-04.065, before the shutdown */
	uint32_t min_delay_ms;		/* before the record write (W3) */
};

/* S-04.027: the Nordic step of a reboot request for its reason. */
enum s04_nordic s04_nordic_step(uint16_t reason);

/*
 * S-04.020: what writer w does.  arg is the host mode reason for W1 (from
 * s04_host_mode_reason), the reason this boot took for W7 and the list
 * place (from 0) for W13 (S-04.072); otherwise ignored.
 */
void s04_writer_rule(enum s04_writer w, uint16_t arg, struct s04_writer_rule *out);

/* The operations a reboot sequence needs.  None returns except as noted. */
struct s04_reboot_ops {
	void *ctx;
	void (*psoc_reset_request)(void *ctx);	/* message 0x97 DE AD BE EF */
	void (*shutdown)(void *ctx);		/* S-04.063, returns */
	void (*nordic_enter_dfu)(void *ctx);	/* returns */
	void (*nordic_exit_dfu)(void *ctx);	/* returns */
	void (*nordic_link_stop)(void *ctx);	/* stop using UART1, returns */
	void (*delay_ms)(void *ctx, uint32_t ms);	/* returns */
	/* S-13b.072: every link's last bytes out, bounded, returns;
	 * NULL: no link to drain */
	void (*drain_links)(void *ctx);
	void (*write_record)(void *ctx, uint16_t reason);	/* returns */
	void (*hard_reset)(void *ctx);		/* arm the watchdog and wait */
	void (*wait_for_psoc)(void *ctx);	/* the soft reboot's stop */
};

/*
 * Run writer w's sequence (S-04.022, S-04.023, S-04.027, S-04.028).  On real
 * hardware it does not return; with test operations it returns after the
 * last one.
 * For W3 the caller has sent its reply to sub-command 05 before this call;
 * the sequence's 10 ms wait then puts the reset at least 10 ms after it
 * (S-04.023, S-10c.091).
 */
void s04_reboot(const struct s04_reboot_ops *ops, enum s04_writer w, uint16_t arg);

/* S-04.024: the CDC request file, scanned one root entry at a time. */
struct s04_cdc_scan {
	bool tfw_seen;
	bool request_seen;
};

void s04_cdc_scan_init(struct s04_cdc_scan *s);
/* name is NUL-terminated; is_regular false for a directory or anything else. */
void s04_cdc_scan_entry(struct s04_cdc_scan *s, const char *name, bool is_regular);
/* True when the scan must write reason 10 and reset hard (W4). */
bool s04_cdc_scan_result(const struct s04_cdc_scan *s);

/* --- The metadata record (S-04.040 to S-04.046) --- */

/* The largest record, the OP-XY's; a device's is device.metadata_size
 * (S-04.040, S-18.030), s04_meta_size(). */
#define S04_META_SIZE      0x34u
#define S04_META_MAGIC     0xDEADBABEu
#define S04_STATE_IDLE     0u
#define S04_STATE_UPGRADE  0xAB0Du

struct s04_meta {
	char serial[9];		/* up to 8 characters, NUL-terminated */
	uint16_t sku;
	uint16_t factory;
	uint16_t os_version[4];
	uint16_t sw_version[4];
	uint32_t state;		/* 0 or 0xAB0D */
	uint32_t cv_gain;
	uint16_t cv_zero;
	uint16_t cv_pair2[2];
};

/* S-04.040: the record's bytes on dev, device.metadata_size. */
uint32_t s04_meta_size(const struct wb_device *dev);

/*
 * S-04.042: the reader's view of the s04_meta_size(dev) bytes at raw. A
 * field dev lacks reads 0: factory without factory-word, the CV words
 * without cv-calibration (S-04.040).
 */
void s04_meta_read(const struct wb_device *dev, const uint8_t *raw, struct s04_meta *m);

/*
 * S-04.043: the s04_meta_size(dev) bytes a write stores, from the record in
 * the flash just before the write and the bootloader's current values.
 * Without factory-word, +0x10..+0x11 are kept in either mode.
 */
void s04_meta_build(const struct wb_device *dev, const uint8_t *before,
		    const struct s04_meta *cur, bool factory_mode, uint8_t *out);

/* S-04.045 M6: true when a failed OS load must write sw_version 0.0.0.0. */
bool s04_m6_needed(const uint16_t sw_version[4]);

/* S-04.046: a set-metadata request; a key is absent when its pointer is NULL. */
struct s04_setmeta {
	const char *serial;
	const char *sku_variant;	/* any text, read as S-10c.056 reads it */
	const char *factory;		/* any text, read as S-10c.056 reads it */
};

/*
 * Apply a set-metadata request to cur. Without factory-word the factory key
 * is passed over (S-04.046): it changes nothing and refuses nothing.  Returns true when the record is to be
 * written (M7); false when nothing changed and nothing is to be written (the
 * mode or option is off, or the serial is not 8 characters).
 */
bool s04_set_metadata(const struct wb_device *dev, struct s04_meta *cur, bool factory_mode,
		      bool option_on, const struct s04_setmeta *req);

/* --- Status reports to the PSoC (S-04.060 to S-04.065) --- */

struct s04_psoc_msg {
	uint8_t id;
	uint8_t len;
	uint8_t payload[4];
};

extern const struct s04_psoc_msg s04_msg_start;		/* S-04.060 */
extern const struct s04_psoc_msg s04_msg_held;		/* S-04.061, S-04.064 */
extern const struct s04_psoc_msg s04_msg_held_screen;	/* S-04.061 0x40 01 */
extern const struct s04_psoc_msg s04_msg_held_bar;	/* S-04.061 0x40 02 00 */
extern const struct s04_psoc_msg s04_msg_handover;	/* S-04.062 */
extern const struct s04_psoc_msg s04_msg_shutdown;	/* S-04.063 step 2 */
extern const struct s04_psoc_msg s04_msg_shutdown_40;	/* S-04.063 step 3 */
extern const struct s04_psoc_msg s04_msg_reset_request;	/* S-04.065 */

/*
 * S-04.066: the shutdown on a device without second-cpu. Step 1, when the
 * volume is mounted: emmc_shutdown() unmounts it inside S-03.013's
 * sequence; its failure does not stop the shutdown. Then reports() sends
 * S-04.063's reports, which on such a device are 0x51 {00 00 02} and,
 * with key-leds only, 0x40 {01} (s07_shutdown_reports()). No power-off
 * request and no wait between the two.
 */
struct s04_shutdown_ops {
	void *ctx;
	bool (*mounted)(void *ctx);
	int (*emmc_shutdown)(void *ctx);
	void (*reports)(void *ctx);
};

void s04_shutdown_one_cpu(const struct s04_shutdown_ops *ops);

/* --- The RTC encoding (S-04.070) --- */

/*
 * Encode a calendar time into RTC0 CLK.  Returns false for a date before
 * 1970-01-01 or one the 15-bit day field cannot hold.
 */
bool s04_rtc_encode(unsigned year, unsigned month, unsigned day, unsigned hour,
		    unsigned minute, unsigned second, uint32_t *clk);

struct s04_datetime {
	unsigned year, month, day, hour, minute, second;
};

void s04_rtc_decode(uint32_t clk, struct s04_datetime *out);

/* --- The fatal-halt record (S-04.071) --- */

#define S04_FATAL_ADDR 0x0803DFD4u
#define S04_FATAL_SIZE 16u

/* The causes the bootloader halts on; the table's rows. */
enum s04_fatal_cause {
	S04_FATAL_DDR_CHECK = 0		/* S-13a.021's DDR check failed */
};

/* The 16 bytes of the record: the cause's words, then the code address pc. */
void s04_fatal_record(enum s04_fatal_cause cause, uint32_t pc,
		      uint8_t out[S04_FATAL_SIZE]);

#endif /* WAFFLE_S04_RECORDS_H */

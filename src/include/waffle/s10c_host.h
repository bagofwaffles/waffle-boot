/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_S10C_HOST_H
#define WAFFLE_S10C_HOST_H

/*
 * S-10c sections 5-9: the host command set on the CDC link and the USB MIDI
 * carrier, the MIDI firmware transfer, progress reports and error texts.
 *
 * Transport-independent: commands arrive as S-06 frames already decoded by
 * the S-06 receiver (s10c_host_frame) or as whole SysEx messages
 * (s10c_host_midi_message); answers leave through struct s10c_ops. The
 * metadata record, OTP, log configuration, reboots, transfer storage and
 * package installation are all reached through the ops.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/device.h"
#include "waffle/s10c_sysex.h"
#include "waffle/s11_common.h"
#include "waffle/s11_version.h"

enum s10c_link {
	S10C_LINK_CDC,
	S10C_LINK_MIDI,
	/* The coprocessor links: S-10c's common command set, as from a host
	 * (S-07.076, S-08.004). */
	S10C_LINK_PSOC,
	S10C_LINK_NORDIC,
};

/* ---- texts (S-10c.054, S-10c.055, S-10c.088, S-10c.101-S-10c.105) ---- */

/* The values of the info text (S-10c.055). */
struct s10c_info {
	uint8_t serial_len;		/* 0-8; 0 when the record is invalid */
	char serial[8];
	bool dsp_ok;			/* OTP item 1 could be read */
	uint8_t dsp_otp[18];		/* OTP item 1 (S-16.095) */
	uint8_t hw_rev;			/* S-17.013 */
	uint32_t sku;
	struct s11_version sw, bl, os;	/* bl: the bootloader's own (S-17.032) */
	uint32_t factory;
};

/* The longest info text: every field at its limit, device_id:unknown. As an
 * S-06 payload it is under 249 bytes, and as a TE reply under 288
 * (S-10c.005). */
#define S10C_INFO_TEXT_MAX 243u

/* The info text with no NUL (S-10c.054, S-10c.055, S-18.031) of dev,
 * decided as how says; returns its length. `buf` holds S10C_INFO_TEXT_MAX
 * bytes. */
size_t s10c_info_text(const struct wb_device *dev, enum wb_dev_how how,
		      const struct s10c_info *v, char *buf);

/* The TE code of a product word: 10 characters (S-10c.088). */
void s10c_te_code(uint32_t word, char out[10]);

/* A set-metadata request's keys (S-10c.056). */
struct s10c_setmeta {
	bool has_serial, has_sku, has_factory;
	uint8_t serial_len;		/* at most 23 */
	char serial[23];
	uint32_t sku;			/* unsigned 32-bit decimal (S-10c.056) */
	uint32_t factory;
};

void s10c_setmeta_parse(const uint8_t *payload, size_t len,
			struct s10c_setmeta *out);

/* The unsigned 32-bit decimal reading of S-10c.056. */
uint32_t s10c_parse_u32(const char *s, size_t len);

/* 100 x done / total, halves up, exactly (S-10c Q-C18); 100 when total is 0
 * (S-10c.103). */
uint8_t s10c_percent(uint32_t done, uint32_t total);
/* The TE reply form's percent: as s10c_percent(), but 0 for a report with
 * no counts (done and total 0, S-10c.103). */
uint8_t s10c_percent_te(uint32_t done, uint32_t total);

#define S10C_TEXT_MAX 247u	/* S-10c.101, S-10c.105 */
#define S10C_LABEL_MAX_TE 62u	/* S-10c.102 */

/* The 0E payload of a progress report on an S-06 stream, `92` and its text
 * (S-10c.101). `out` holds 1 + S10C_TEXT_MAX bytes. */
size_t s10c_progress_s06(uint32_t done, uint32_t total, const char *label,
			 uint8_t *out);

/* The unpacked payload of a TE progress reply (S-10c.102): the percent,
 * then the label cut to S10C_LABEL_MAX_TE bytes, no 00 after it. outcome:
 * the report of S-10c.106, which carries 100. `out` holds
 * 1 + S10C_LABEL_MAX_TE bytes. */
size_t s10c_progress_te(uint32_t done, uint32_t total, bool outcome,
			const char *label, uint8_t *out);

/* The label of a run's step 0-5 (S-10c.104); "" for any other. */
const char *s10c_step_label(unsigned step);
/* The label of the report that follows a step's Skipped (S-10c.106):
 * "[skipped] ", the step's label without its brackets, ": " and why.
 * `out` holds S10C_SKIP_LABEL_MAX + 1 bytes. */
#define S10C_SKIP_LABEL_MAX 47u
size_t s10c_skip_label(unsigned step, enum s11_skip why, char *out);
#define S10C_LABEL_DECRYPTING "[decrypting firmware]"
#define S10C_LABEL_EXTRACTING "[extracting firmware]"

/* How long a write of `len` bytes to a host link waits for the host
 * before it is given up, in ms (S-10c.033, S-16.046). */
uint32_t s10c_write_timeout_ms(size_t len);

/* The error text of a refused package or failed run (S-10c.105).
 * `buf` holds S10C_TEXT_MAX bytes. */
size_t s10c_error_text(const struct s11_outcome *o, char *buf);

/* ---- the host links ---- */

/* Where a submitted package came from (S-10c.075, S-10c.092). */
enum s10c_source {
	S10C_SOURCE_FW_CDC,	/* firmware command on CDC: forced */
	S10C_SOURCE_FW_MIDI,	/* firmware command on the MIDI carrier: forced */
	S10C_SOURCE_TE,		/* MIDI firmware transfer: not forced */
	S10C_SOURCE_FW_COPROC,	/* firmware command on a coprocessor link:
				 * forced (S-10c.045, S-10c.075) */
};

struct s10c_host;

/* S-10c.076: whether Continue to the OS would start now. */
enum s10c_continue {
	S10C_CONT_GO,		/* it starts: reply 54, then continue_os */
	S10C_CONT_BUSY,		/* a scan, host transfer, run or OS write */
	S10C_CONT_NOT_HELD	/* before held mode (S-10c.045) */
};

/* Whether a change a host asks for on a host link is served. */
enum s10c_gate {
	S10C_GATE_OK,
	S10C_GATE_OFF,		/* the host link is off */
	S10C_GATE_ASK,		/* not approved: the unit asks for it now */
	S10C_GATE_BUSY		/* a host's PSoC request holds the unit (S-10c.134) */
};

struct s10c_ops {
	/* An S-06 frame from the master (source 1) on a host link, or on a
	 * coprocessor link (the PSoC's or the Nordic's S-06 link), framed by
	 * the S-06 encoder with the link's next sequence number (S-06.020). */
	void (*send_frame)(void *ctx, enum s10c_link link, uint8_t route,
			   uint8_t id, const uint8_t *payload, size_t len);
	/* One whole SysEx message on the MIDI link (the USB MIDI port packs it into event
	 * packets with s10c_usbmidi_send). */
	void (*send_sysex)(void *ctx, const uint8_t *msg, size_t n);
	/* Whether the CDC port is open (S-10c.031). */
	bool (*cdc_open)(void *ctx);
	/* The S-06 receiver for the CDC link: start afresh (S-10c.032). */
	void (*cdc_rx_restart)(void *ctx);
	/* The S-06 receiver for the MIDI link: start afresh, then bytes. */
	void (*midi_rx_restart)(void *ctx);
	void (*midi_rx_feed)(void *ctx, const uint8_t *stream, size_t n);

	/* The session upgrade option (S-02.055), read each time. */
	bool (*option_on)(void *ctx);
	/* The info values from the current record, OTP and revision. */
	void (*read_info)(void *ctx, struct s10c_info *v);
	/* S-04.046: applies the keys and writes the record; false when the
	 * serial is refused or the write fails. */
	bool (*write_metadata)(void *ctx, const struct s10c_setmeta *m);
	/* The four 32-bit words the boot ROM's OTP interface returns for the
	 * factory serial number, in order (S-10c.057); false when it cannot
	 * be read. */
	bool (*read_fsn)(void *ctx, uint32_t words[4]);
	/* S-14.012-S-14.014 */
	void (*log_config)(void *ctx, const uint8_t *payload, size_t len);
	/* S-14.040: a log line 03 or 04 from the host on CDC */
	void (*log_relay)(void *ctx, uint8_t id, const uint8_t *payload,
			  size_t len);
	/* S-04.022 */
	void (*reboot_os)(void *ctx);
	/* S-04.021, S-16.106 */
	void (*reboot_mode)(void *ctx, bool has_mode, uint8_t mode);
	/* S-10c.091: reason 20 (S-04.020 W3), at least 10 ms after the
	 * reply has been sent. */
	void (*reboot_after_dfu)(void *ctx);

	/* The upgrade screen (S-15); `total` 0 when unknown. */
	void (*upgrade_screen)(void *ctx, uint32_t total);
	/* The begin that opened the screen was refused for its
	 * product word (S-10c.071, S-10c.088), with S-11.020's reason
	 * (S11_REASON_PRODUCT), for s15_up_refused(). NULL: not reported. */
	void (*begin_refused)(void *ctx, enum s11_reason reason);
	/* Prepares the transfer's storage and withdraws the RAM disk from the
	 * host (S-11.050); false when the storage cannot be prepared. */
	bool (*transfer_start)(void *ctx);
	/* Appends bytes; false when they cannot be stored. */
	bool (*transfer_append)(void *ctx, const uint8_t *p, size_t n);
	/* Discards the bytes received so far. */
	void (*transfer_discard)(void *ctx);
	/* Submits the transfer's bytes as one package (S-11.050). Progress
	 * goes through s10c_host_progress() meanwhile. */
	void (*transfer_submit)(void *ctx, enum s10c_source source,
				struct s11_outcome *out);
	/* Offers the RAM disk to the host again (S-11.050). */
	void (*ramdisk_offer)(void *ctx);
	/* S-10c.076: whether Continue to the OS would start now; NULL is
	 * S10C_CONT_NOT_HELD. */
	enum s10c_continue (*continue_check)(void *ctx);
	/* S-10c.076, S-05.033: the restart, after the reply 54 on link;
	 * does not return on the device. */
	void (*continue_os)(void *ctx, enum s10c_link link);
	/* S-10c.059, S-10c.061, S-10c.070: the host link's consent to a change
	 * asked for on CDC, the MIDI link or the Nordic's: a transfer, a
	 * restart, factory unlock, set metadata or the log's configuration
	 * (0xB0). NULL serves every one; the PSoC's link is never asked. */
	enum s10c_gate (*gate)(void *ctx, enum s10c_link link);
};

/* Where progress reports go (S-10c.100). */
struct s10c_progress_sink {
	enum s10c_link link;
	bool te;			/* TE reply form to `req` */
	struct s10c_te_req req;
};

struct s10c_host {
	const struct s10c_ops *ops;
	void *ctx;
	const struct wb_device *dev;	/* S-18.030's values */
	enum wb_dev_how how;		/* S-18.031's device_id */
	bool factory;			/* factory mode (S-10c.053) */
	bool transfer;			/* a transfer is in progress */
	bool got_chunk;			/* ... and has received a chunk */
	uint8_t expected_chunk;		/* S-10c.089 */
	struct s10c_progress_sink sink;
	uint8_t buf[S10C_TE_MAX_PAYLOAD];
};

/* dev is the device the front decided, how as the hand-off record says
 * (S-18.005). */
void s10c_host_init(struct s10c_host *h, const struct s10c_ops *ops,
		    void *ctx, const struct wb_device *dev,
		    enum wb_dev_how how);

/*
 * An S-06 frame that S-06 delivered to the master on a host link
 * (S-10c.040): destination 1 or 8-15. Returns whether the id is served on
 * that link (S-10c.041).
 *
 * On a coprocessor link it is a message the PSoC or the Nordic module
 * passed on as S-10c's common command set (struct s07_ops and s08_ops
 * common, S-07.076, S-08.004): the CDC column of S-10c.041, the answers
 * sent back on that link through send_frame. Log lines 03 and 04 are not
 * served there: the caller relays them with s14_log_relay() and the
 * frame's source, which S-14.040 and S-14.041 tell apart. For an id not
 * served the caller logs it; nothing is sent (S-07.076).
 */
bool s10c_host_frame(struct s10c_host *h, enum s10c_link link, uint8_t route,
		     uint8_t id, const uint8_t *payload, size_t len);

/* A SET_CONTROL_LINE_STATE request, whatever its value (S-10c.032). */
void s10c_host_cdc_line_state(struct s10c_host *h);

/* A complete SysEx message received on the MIDI link (S-10c.007). */
void s10c_host_midi_message(struct s10c_host *h, const uint8_t *m, size_t n);

/* A progress report of the package being checked or installed. Outside a
 * host submission it goes to CDC in the S-06 form (S-10c.100). */
void s10c_host_progress(struct s10c_host *h, uint32_t done, uint32_t total,
			const char *label);

/* The outcome report with no counts: "[done]", "[nothing to do]" or, for a
 * RAM-disk run, "[failed] " and the error text (S-10c.106). */
void s10c_host_outcome(struct s10c_host *h, const struct s11_outcome *o);

#endif /* WAFFLE_S10C_HOST_H */

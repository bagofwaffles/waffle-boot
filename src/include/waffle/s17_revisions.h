/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_S17_REVISIONS_H
#define WAFFLE_S17_REVISIONS_H

/*
 * S-17: the revision strap, the reported revision (hw_rev) and the
 * coprocessor versions. Pure logic: pin levels, metadata-read outcomes,
 * greetings and coprocessor replies come in as values or through
 * struct s17_link_ops.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/device.h"

#include "waffle/s11_version.h"

/* ---- 1. The revision strap ---- */

/* The decoded revision of strap index 00 (S-17.001). */
#define S17_NO_REVISION 0xFFu

/* 2 x PA8 + PA0, each level 0 or 1 (S-17 terms). */
unsigned s17_strap_index(unsigned pa8, unsigned pa0);

/* The decoded revision: 0xFF, 4, 5 or 6 (S-17.001). */
uint8_t s17_decode_strap(unsigned pa8, unsigned pa0);

/* S-13c.004's panel reset case, selected by the master's PA8 (S-17.003). */
enum s17_panel_reset {
	/* PA8 low: the panel counts as reset at the bootloader's own release;
	 * the first stage's recorded reset time is not used. */
	S17_PANEL_RESET_AT_RELEASE,
	/* PA8 high: the first stage's handshake is honored when present and
	 * valid, otherwise one reset pulse. */
	S17_PANEL_RESET_HANDSHAKE_OR_PULSE,
};

enum s17_panel_reset s17_panel_reset_path(unsigned pa8);

/* ---- 2. The reported revision ----
 *
 * S-17.002, S-17.040: the decoded revision feeds only the reported
 * revision (and the master's panel-reset case, S-17.003); no revision is
 * refused or warned about. S-17.004: the slave reports none of it.
 */

enum s17_meta_state {
	S17_META_UNREAD,		/* the start-up read has not finished */
	S17_META_READ,			/* read (a bad magic reads as zeros) */
	S17_META_RECORD_FAILED,		/* the record could not be read */
	S17_META_SERIAL_FAILED,		/* the chip serial could not be read */
};

/* The master's reported revision over one power-on. */
struct s17_rev {
	uint8_t decoded;
	uint8_t reported;
	enum s17_meta_state meta;
};

/* Power-on or any reset of the master: a new strap sample (S-17.012). */
void s17_rev_power_on(struct s17_rev *r, unsigned pa8, unsigned pa0);
/*
 * The same on dev: with straps, s17_rev_power_on(); without them (S-17.016)
 * no sample, pa8 and pa0 unused, and the reported revision 0 until a
 * greeting sets it.
 */
void s17_rev_power_on_dev(struct s17_rev *r, const struct wb_device *dev, unsigned pa8,
			  unsigned pa0);

/* The outcome of the start-up metadata read (S-17.010, S-17.015). */
void s17_rev_metadata(struct s17_rev *r, enum s17_meta_state outcome);

/*
 * A greeting (id 0x05) from the Nordic, delivered to the master as an
 * ordinary message (S-17.011). Returns true when it set the reported
 * revision, which must then be logged (S-17.014); *byte is the value.
 */
bool s17_rev_greeting(struct s17_rev *r, const uint8_t *payload, size_t len,
		      uint8_t *byte);

uint8_t s17_rev_reported(const struct s17_rev *r);

/* Longest hw_rev entry: "hw_rev:255". */
#define S17_HW_REV_ENTRY_MAX 10u

/* Writes "hw_rev:N" with no NUL (S-17.013); returns the length, or 0 when
 * `cap` is too small. */
size_t s17_hw_rev_entry(uint8_t rev, char *buf, size_t cap);

/* The byte as exactly two lower-case hexadecimal digits (S-17.014). */
void s17_hex2(uint8_t byte, char out[2]);

/* ---- 3. Coprocessor versions ---- */

#define S17_SERIAL_MAX 18u

/* The four coprocessor values (S-17.020). */
struct s17_copro {
	struct s11_version psoc;
	struct s11_version nordic_app;
	struct s11_version nordic_bl;
	uint8_t psoc_serial_len;
	char psoc_serial[S17_SERIAL_MAX];	/* not NUL-terminated */
};

/* 0.0.0 and the empty text, as at each power-on (S-17.020). */
void s17_copro_init(struct s17_copro *c);

/*
 * The value of the first entry of a key:value payload whose key equals `key`
 * exactly (S-17.023). Returns false when there is none.
 */
bool s17_kv_lookup(const uint8_t *payload, size_t len, const char *key,
		   const uint8_t **value, size_t *value_len);

/* A version value of a reply (S-17.024): false when it is not a version. */
bool s17_version_value(const uint8_t *value, size_t len,
		       struct s11_version *out);

enum s17_peer {
	S17_PEER_NORDIC,
	S17_PEER_PSOC,
};

/* One reply of S-17.021 step 1 read into the values (S-17.023). */
void s17_copro_held_reply(struct s17_copro *c, enum s17_peer peer,
			  const uint8_t *payload, size_t len);

/*
 * The second reply of S-17.021 step 2, made only after a mode reply of
 * "bootloader": the Nordic application and bootloader versions (S-17.023).
 */
void s17_copro_dfu_reply(struct s17_copro *c, const uint8_t *payload,
			 size_t len);

/* Whether a reply's `mode` entry is exactly "bootloader" (S-17.021). */
bool s17_reply_in_bootloader(const uint8_t *payload, size_t len);

/* The reply wait of S-17.022. */
#define S17_REPLY_TIMEOUT_MS 100u

/* A coprocessor reply is at most an S-06 payload. */
#define S17_REPLY_MAX 256u

struct s17_link_ops {
	/*
	 * Sends the coprocessor info request (id 0x09, empty payload) to `peer`
	 * and waits at most `timeout_ms` for its 0x0A reply. Returns the reply
	 * payload's length (at most `cap`), or -1 when no readable reply came.
	 */
	int (*info_exchange)(void *ctx, enum s17_peer peer,
			     uint32_t timeout_ms, uint8_t *reply, size_t cap);
};

/*
 * S-17.021 step 1, on entering held mode and at the end of an upgrade run
 * that made step 2: one exchange with the Nordic, then one with the PSoC.
 * Two exchanges of 100 ms bound the step well within S-17.022's 2 s.
 */
void s17_copro_refresh(struct s17_copro *c, const struct s17_link_ops *ops,
		       void *ctx);

/*
 * S-17.021 step 2, at the start of an upgrade run: a mode exchange with the
 * Nordic and, only in bootloader mode, a second exchange read for the
 * Nordic's versions. Returns whether the Nordic reported bootloader mode.
 */
bool s17_copro_dfu_read(struct s17_copro *c, const struct s17_link_ops *ops,
			void *ctx);

/* ---- 4. The bootloader's own version ---- */

/* S-17.032: a version the bootloader may report as its own. */
bool s17_own_version_ok(const struct s11_version *v);

#endif /* WAFFLE_S17_REVISIONS_H */

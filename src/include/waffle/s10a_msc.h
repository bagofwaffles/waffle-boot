/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_S10A_MSC_H
#define WAFFLE_S10A_MSC_H

/*
 * S-10a section 4: the mass-storage function. The logical unit (SCSI
 * commands, sense, the disk's states, acquire and release; S-10a.042 to
 * S-10a.052) and the Bulk-Only Transport that carries it (S-10a.040,
 * S-10a.041). Both are independent of the USB stack: the BOT engine moves
 * data through struct s10a_bot_port, which the TinyUSB glue (s10a_usb.c)
 * or a test fills.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "blockdev.h"

#define S10A_BLOCK		512u
#define S10A_SERIAL_MAX		8u	/* S-10a.004 */

#define S10A_SCSI_GOOD		0x00u
#define S10A_SCSI_CHECK		0x02u

/* Sense keys (SPC-3 4.5.6) */
#define S10A_KEY_NONE		0x0u
#define S10A_KEY_NOT_READY	0x2u
#define S10A_KEY_MEDIUM_ERROR	0x3u
#define S10A_KEY_ILLEGAL	0x5u
#define S10A_KEY_UNIT_ATTENTION	0x6u

enum s10a_dir {
	S10A_DIR_NONE,
	S10A_DIR_IN,
	S10A_DIR_OUT,
};

/* LUN 0. Owned by the caller; set up with s10a_lu_init(). */
struct s10a_lu {
	const struct wb_blockdev *dev;
	char serial[S10A_SERIAL_MAX + 1];	/* S-10a.004, "" for none */
	bool offered;		/* S-10a.049 */
	bool stopped;		/* S-10a.048 */
	bool ua;		/* a unit attention owed (S-10a.043) */
	bool acquired;		/* the host holds the disk (S-10a.050) */
	uint32_t releases;	/* releases the host has made */
	uint32_t withdrawals;	/* the bootloader's, counted (S-10a.049) */
	uint8_t key, asc, ascq;	/* the current sense */
	uint32_t info;		/* its information field */
	/* Called on each release (S-10a.050); may be NULL. */
	void (*on_release)(void *ctx);
	void *ctx;
	uint8_t reply[64];	/* the data-in reply of the last command */
};

/*
 * A command as s10a_scsi_command() decided it: the device's intent for the
 * Bulk-Only Transport (S-10a.040). An inline data-in reply is at `data`;
 * a READ or WRITE moves `len` bytes of the blocks from `lba`, and its outcome
 * is settled by s10a_scsi_done(). MODE SELECT's parameter list is received
 * and dropped.
 */
struct s10a_scsi_cmd {
	uint8_t status;		/* S10A_SCSI_GOOD or S10A_SCSI_CHECK */
	enum s10a_dir dir;
	uint32_t len;
	const uint8_t *data;	/* inline data-in, or NULL */
	bool blocks;		/* a READ or WRITE of blocks */
	uint32_t lba;
	bool media;		/* a media command (S-10a.045) */
};

/* The unit with no medium offered (S-10a.047). `serial` is S-10a.004's
 * text (NULL or "" for none), dev->n_sectors the capacity. */
void s10a_lu_init(struct s10a_lu *lu, const struct wb_blockdev *dev,
		  const char *serial);

/* Why the bootloader withdraws the disk (S-10a.049, S-10a.050). */
enum s10a_withdrawal {
	/* A scan (S-11.045) or a host-link transfer (S-11.050): a host
	 * holding the disk releases it, as with an eject. */
	S10A_WITHDRAW_SCAN,
	/* A write of the bootloader's own (S-12.018): the hold ends without a
	 * release. */
	S10A_WITHDRAW_OWN_WRITE,
};

/*
 * The bootloader's side (S-10a.049). Takes effect at the next command: one
 * whose CBW has arrived completes as the state before, unless a bus reset, a
 * Bulk-Only Mass Storage Reset or a re-selection abandons it. A WRITE is the
 * exception: from a withdrawal on, nothing more of its data is stored, even
 * after an offer, and it fails (s10a_scsi_withdrawn()).
 */
void s10a_lu_offer(struct s10a_lu *lu);
void s10a_lu_withdraw(struct s10a_lu *lu, enum s10a_withdrawal why);

/* The bus's side (S-10a.032, S-10a.050, S-10a.080). */
void s10a_lu_bus_reset(struct s10a_lu *lu);
void s10a_lu_select_configuration(struct s10a_lu *lu);
void s10a_lu_deconfigure(struct s10a_lu *lu);

/*
 * Decides a CDB of `cdb_len` bytes (bCBWCBLength). Inline commands are
 * complete when it returns; a READ or WRITE is GOOD so far and ends with
 * s10a_scsi_done().
 */
void s10a_scsi_command(struct s10a_lu *lu, const uint8_t *cdb,
		       unsigned cdb_len, struct s10a_scsi_cmd *cmd);

/* A READ or WRITE that moved all its blocks (ok) or whose block device
 * failed (!good, write: `write`). Returns the command's final status. */
uint8_t s10a_scsi_done(struct s10a_lu *lu, const struct s10a_scsi_cmd *cmd,
		       bool good, bool write);
/* A WRITE whose data a withdrawal cut (S-10a.049): NOT READY 3Ah/00h. */
void s10a_scsi_withdrawn(struct s10a_lu *lu);

/* --- Bulk-Only Transport (S-10a.040, S-10a.041) ------------------------- */

#define S10A_BOT_CBW_LEN	31u
#define S10A_BOT_CSW_LEN	13u
#define S10A_BOT_BUF		4096u	/* data phase chunk, whole blocks */

/*
 * The two bulk endpoints. xfer_in/xfer_out arm one transfer and return
 * false when the endpoint cannot take it (halted, busy); its completion is
 * reported with s10a_bot_xfer_done(). stall_* halt an endpoint.
 */
struct s10a_bot_port {
	void *ctx;
	bool (*xfer_in)(void *ctx, const uint8_t *buf, uint32_t n);
	bool (*xfer_out)(void *ctx, uint8_t *buf, uint32_t n);
	void (*stall_in)(void *ctx);
	void (*stall_out)(void *ctx);
};

enum s10a_bot_state {
	S10A_BOT_IDLE,		/* not configured */
	S10A_BOT_CBW,		/* a CBW expected */
	S10A_BOT_DATA_IN,
	S10A_BOT_DATA_OUT,
	S10A_BOT_CSW_HALTED,	/* the CSW waits for the host to clear IN */
	S10A_BOT_CSW,		/* the CSW is being sent */
	S10A_BOT_RESET_WAIT,	/* a refused CBW: halted until a reset */
};

struct s10a_bot {
	struct s10a_lu *lu;
	const struct s10a_bot_port *port;
	enum s10a_bot_state state;
	struct s10a_scsi_cmd cmd;
	uint32_t tag;
	uint32_t host_len;	/* dCBWDataTransferLength */
	bool host_in;
	uint32_t moved;		/* data phase bytes so far */
	uint32_t want;		/* bytes the data phase will move */
	uint32_t chunk;		/* bytes of the transfer in flight */
	uint32_t residue;
	uint8_t status;		/* bCSWStatus */
	bool stall_after;	/* halt the data endpoint after the data */
	bool store;		/* data-out blocks are written */
	uint32_t withdrawals;	/* the unit's count at the CBW */
	bool io_ok;
	uint8_t csw[S10A_BOT_CSW_LEN];
	uint8_t buf[S10A_BOT_BUF];
};

void s10a_bot_init(struct s10a_bot *b, struct s10a_lu *lu,
		   const struct s10a_bot_port *port);

/* The configuration was selected (or selected again, S-10a.032): any
 * command in progress is abandoned with no CSW; a CBW is expected. */
void s10a_bot_start(struct s10a_bot *b);
/* Deconfigured or bus reset: nothing in progress, nothing armed. */
void s10a_bot_stop(struct s10a_bot *b);

/* A transfer armed through the port has completed with n bytes. */
void s10a_bot_xfer_done(struct s10a_bot *b, bool in, uint32_t n);
/* The host cleared a halt of a bulk endpoint (CLEAR_FEATURE). */
void s10a_bot_clear_halt(struct s10a_bot *b, bool in);
/* Whether a CLEAR_FEATURE(ENDPOINT_HALT) must leave the endpoints halted
 * (S-10a.033: a reset recovery is owed). */
bool s10a_bot_reset_owed(const struct s10a_bot *b);
/* Whether a command is in progress (its CBW read, its CSW not sent). */
bool s10a_bot_busy(const struct s10a_bot *b);

/*
 * The class requests of S-10a.041, bmRequestType, bRequest, wValue and
 * wLength of a request to the mass-storage interface. Returns false for a
 * STALL; for Get Max LUN *reply is the byte to send.
 */
bool s10a_bot_class_request(struct s10a_bot *b, uint8_t bm, uint8_t req,
			    uint16_t value, uint16_t length, uint8_t *reply);

#endif /* WAFFLE_S10A_MSC_H */

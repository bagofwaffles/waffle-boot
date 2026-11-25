/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10a.040, S-10a.041: the Bulk-Only Transport (waffle/usb/msc.h).
 *
 * Where BOT 1.0 6.7 lets the device end a data phase short or halt the
 * endpoint (cases 4, 5, 9, 11), this one always halts it: a short packet
 * cannot end a phase whose length is a multiple of the packet size.
 */
#include "waffle/usb/msc.h"

#include <string.h>

#include "waffle/util/bytes.h"

#define CBW_SIGNATURE	0x43425355u	/* "USBC" */
#define CSW_SIGNATURE	0x53425355u	/* "USBS" */
#define CSW_GOOD	0u
#define CSW_FAILED	1u
#define CSW_PHASE	2u

void s10a_bot_init(struct s10a_bot *b, struct s10a_lu *lu,
		   const struct s10a_bot_port *port)
{
	memset(b, 0, sizeof(*b));
	b->lu = lu;
	b->port = port;
}

static void arm_cbw(struct s10a_bot *b)
{
	b->state = S10A_BOT_CBW;
	/* A whole block, so that a longer transfer is seen as one. */
	(void)b->port->xfer_out(b->port->ctx, b->buf, S10A_BLOCK);
}

static void send_csw(struct s10a_bot *b)
{
	wb_put_le32(b->csw, CSW_SIGNATURE);
	wb_put_le32(b->csw + 4, b->tag);
	wb_put_le32(b->csw + 8, b->residue);
	b->csw[12] = b->status;
	b->state = S10A_BOT_CSW;
	if (!b->port->xfer_in(b->port->ctx, b->csw, S10A_BOT_CSW_LEN))
		b->state = S10A_BOT_CSW_HALTED;	/* sent once the host clears it */
}

/* After an IN halt the CSW waits for the host's CLEAR_FEATURE. */
static void halt_in_then_csw(struct s10a_bot *b)
{
	b->port->stall_in(b->port->ctx);
	b->state = S10A_BOT_CSW_HALTED;
}

static void refuse(struct s10a_bot *b)
{
	/* S-10a.040: halted until a Bulk-Only Mass Storage Reset. */
	b->state = S10A_BOT_RESET_WAIT;
	b->port->stall_in(b->port->ctx);
	b->port->stall_out(b->port->ctx);
}

static uint32_t chunk_of(const struct s10a_bot *b)
{
	const uint32_t left = b->want - b->moved;

	return left < S10A_BOT_BUF ? left : S10A_BOT_BUF;
}

static uint32_t block_at(const struct s10a_bot *b)
{
	return b->cmd.lba + b->moved / S10A_BLOCK;
}

static void data_end(struct s10a_bot *b);

static void next_in(struct s10a_bot *b)
{
	const struct wb_blockdev *dev = b->lu->dev;
	const uint8_t *src = b->buf;

	b->chunk = chunk_of(b);
	if (b->cmd.blocks) {
		const uint32_t n = (b->chunk + S10A_BLOCK - 1u) / S10A_BLOCK;

		if (dev->read(dev->ctx, block_at(b), b->buf, n) != 0) {
			b->io_ok = false;
			data_end(b);
			return;
		}
	} else {
		src = b->cmd.data + b->moved;
	}
	(void)b->port->xfer_in(b->port->ctx, src, b->chunk);
}

static void next_out(struct s10a_bot *b)
{
	b->chunk = chunk_of(b);
	(void)b->port->xfer_out(b->port->ctx, b->buf, b->chunk);
}

/* The data phase is over (or cut by a block device error). */
static void data_end(struct s10a_bot *b)
{
	const bool in = b->state == S10A_BOT_DATA_IN;

	if (b->cmd.blocks && (b->store || in) &&
	    (!b->io_ok || b->moved == b->cmd.len)) {
		if (s10a_scsi_done(b->lu, &b->cmd, b->io_ok, !in) !=
		    S10A_SCSI_GOOD) {
			b->status = CSW_FAILED;
			b->residue = b->host_len - b->moved;
			b->stall_after = b->moved < b->host_len;
		}
	}
	if (!b->stall_after) {
		send_csw(b);
	} else if (in) {
		halt_in_then_csw(b);
	} else {
		b->port->stall_out(b->port->ctx);
		send_csw(b);
	}
}

/* BOT 1.0 6.7: the host's expectation against the device's intent. */
static void start_command(struct s10a_bot *b)
{
	const uint32_t hl = b->host_len;
	const uint32_t dl = b->cmd.dir == S10A_DIR_NONE ? 0 : b->cmd.len;
	const bool dev_in = b->cmd.dir == S10A_DIR_IN;
	const uint8_t ok = (uint8_t)(b->cmd.status == S10A_SCSI_GOOD ?
				     CSW_GOOD : CSW_FAILED);

	b->moved = 0;
	b->want = 0;
	b->residue = 0;
	b->status = ok;
	b->stall_after = false;
	b->store = false;
	b->io_ok = true;
	b->withdrawals = b->lu->withdrawals;
	if (hl == 0) {
		/* Case 1; cases 2 and 3 move nothing: phase error. */
		if (dl)
			b->status = CSW_PHASE;
		send_csw(b);
		return;
	}
	if (b->host_in) {
		if (dl == 0 || !dev_in) {
			/* Case 4; case 8 is a phase error. */
			if (dl)
				b->status = CSW_PHASE;
			b->residue = hl;
			halt_in_then_csw(b);
			return;
		}
		if (hl < dl) {
			b->want = hl;		/* case 7 */
			b->status = CSW_PHASE;
		} else {
			b->want = dl;		/* cases 5 and 6 */
			b->residue = hl - dl;
			b->stall_after = hl > dl;
		}
		b->state = S10A_BOT_DATA_IN;
		next_in(b);
		return;
	}
	if (dl == 0 || dev_in) {
		/* Case 9; case 10 is a phase error. */
		if (dl)
			b->status = CSW_PHASE;
		b->residue = hl;
		b->port->stall_out(b->port->ctx);
		send_csw(b);
		return;
	}
	if (hl < dl) {
		b->want = hl;		/* case 13: received and dropped */
		b->status = CSW_PHASE;
	} else {
		b->want = dl;		/* cases 11 and 12 */
		b->residue = hl - dl;
		b->stall_after = hl > dl;
		b->store = b->cmd.blocks;
	}
	b->state = S10A_BOT_DATA_OUT;
	next_out(b);
}

static void cbw_received(struct s10a_bot *b, uint32_t n)
{
	uint8_t cdb[16];
	unsigned len;

	if (n != S10A_BOT_CBW_LEN || wb_get_le32(b->buf) != CBW_SIGNATURE ||
	    b->buf[13] != 0) {
		refuse(b);
		return;
	}
	b->tag = wb_get_le32(b->buf + 4);
	b->host_len = wb_get_le32(b->buf + 8);
	b->host_in = (b->buf[12] & 0x80u) != 0;
	len = b->buf[14] & 0x1Fu;
	if (len > sizeof(cdb))
		len = sizeof(cdb);
	memcpy(cdb, b->buf + 15, sizeof(cdb));
	s10a_scsi_command(b->lu, cdb, len, &b->cmd);
	start_command(b);
}

void s10a_bot_start(struct s10a_bot *b)
{
	arm_cbw(b);
}

void s10a_bot_stop(struct s10a_bot *b)
{
	b->state = S10A_BOT_IDLE;
}

void s10a_bot_xfer_done(struct s10a_bot *b, bool in, uint32_t n)
{
	switch (b->state) {
	case S10A_BOT_CBW:
		if (!in)
			cbw_received(b, n);
		break;
	case S10A_BOT_DATA_IN:
		if (!in)
			break;
		b->moved += n;
		if (n == b->chunk && b->moved < b->want)
			next_in(b);
		else
			data_end(b);
		break;
	case S10A_BOT_DATA_OUT: {
		const struct wb_blockdev *dev = b->lu->dev;

		if (in)
			break;
		if (n > b->chunk)
			n = b->chunk;
		if (b->store && b->lu->withdrawals != b->withdrawals) {
			/*
			 * S-10a.049: the disk was withdrawn since the CBW, so
			 * this data and the rest are not stored, whatever is
			 * offered now. The command fails here; OUT is halted
			 * when the host had more to send.
			 */
			s10a_scsi_withdrawn(b->lu);
			b->store = false;
			b->status = CSW_FAILED;
			b->residue = b->host_len - b->moved;
			b->stall_after = b->moved + n < b->host_len;
			data_end(b);
			break;
		}
		if (b->store && n >= S10A_BLOCK &&
		    dev->write(dev->ctx, block_at(b), b->buf,
			       n / S10A_BLOCK) != 0)
			b->io_ok = false;
		b->moved += n;
		if (b->io_ok && n == b->chunk && b->moved < b->want)
			next_out(b);
		else
			data_end(b);
		break;
	}
	case S10A_BOT_CSW:
		if (in)
			arm_cbw(b);
		break;
	case S10A_BOT_IDLE:
	case S10A_BOT_CSW_HALTED:
	case S10A_BOT_RESET_WAIT:
	default:
		break;
	}
}

void s10a_bot_clear_halt(struct s10a_bot *b, bool in)
{
	switch (b->state) {
	case S10A_BOT_CSW_HALTED:
		if (in)
			send_csw(b);
		break;
	case S10A_BOT_CBW:
		/* The host's CLEAR_FEATURE ended the armed CBW read. */
		if (!in)
			arm_cbw(b);
		break;
	case S10A_BOT_RESET_WAIT:
		/* S-10a.033: still halted until the reset. */
		if (in)
			b->port->stall_in(b->port->ctx);
		else
			b->port->stall_out(b->port->ctx);
		break;
	case S10A_BOT_IDLE:
	case S10A_BOT_DATA_IN:
	case S10A_BOT_DATA_OUT:
	case S10A_BOT_CSW:
	default:
		break;
	}
}

bool s10a_bot_reset_owed(const struct s10a_bot *b)
{
	return b->state == S10A_BOT_RESET_WAIT;
}

bool s10a_bot_busy(const struct s10a_bot *b)
{
	switch (b->state) {
	case S10A_BOT_DATA_IN:
	case S10A_BOT_DATA_OUT:
	case S10A_BOT_CSW_HALTED:
	case S10A_BOT_CSW:
		return true;
	case S10A_BOT_IDLE:
	case S10A_BOT_CBW:
	case S10A_BOT_RESET_WAIT:
	default:
		return false;
	}
}

bool s10a_bot_class_request(struct s10a_bot *b, uint8_t bm, uint8_t req,
			    uint16_t value, uint16_t length, uint8_t *reply)
{
	if (bm == 0xA1u && req == 0xFEu && value == 0 && length == 1) {
		*reply = 0;	/* Get Max LUN: LUN 0 only */
		return true;
	}
	if (bm == 0x21u && req == 0xFFu && value == 0 && length == 0) {
		/*
		 * Bulk-Only Mass Storage Reset: a command in progress is
		 * abandoned; a CBW is expected. Halts are left for the host
		 * to clear (BOT 5.3.4); the CBW is armed once OUT is clear.
		 */
		if (b->state != S10A_BOT_IDLE)
			arm_cbw(b);
		return true;
	}
	return false;
}

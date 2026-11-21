/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10a.042 to S-10a.052: the disk as logical unit 0 (waffle/s10a_msc.h).
 */
#include "waffle/s10a_msc.h"

#include <string.h>

#include "waffle/bytes.h"
#include "waffle/device.h"

/* Operation codes (SPC-3, SBC-2) */
enum {
	OP_TEST_UNIT_READY = 0x00,
	OP_REQUEST_SENSE = 0x03,
	OP_READ6 = 0x08,
	OP_WRITE6 = 0x0A,
	OP_INQUIRY = 0x12,
	OP_MODE_SELECT6 = 0x15,
	OP_MODE_SENSE6 = 0x1A,
	OP_START_STOP = 0x1B,
	OP_PREVENT_ALLOW = 0x1E,
	OP_READ_FORMAT_CAPACITIES = 0x23,
	OP_READ_CAPACITY10 = 0x25,
	OP_READ10 = 0x28,
	OP_WRITE10 = 0x2A,
	OP_VERIFY10 = 0x2F,
	OP_SYNCHRONIZE_CACHE10 = 0x35,
	OP_MODE_SELECT10 = 0x55,
	OP_MODE_SENSE10 = 0x5A,
	OP_READ12 = 0xA8,
	OP_WRITE12 = 0xAA,
};

static const char vendor[] = "teenage ";
static const char disk[] = " Disk";
static const char designator_head[] = "teenage engineering ";

/* SPC-3 4.3.4.1: the CDB length of an operation code's group. */
static unsigned group_len(uint8_t op)
{
	switch (op >> 5) {
	case 0:
		return 6;
	case 1:
	case 2:
		return 10;
	case 4:
		return 16;
	case 5:
		return 12;
	default:
		return 1;
	}
}

static bool media_op(uint8_t op)
{
	switch (op) {
	case OP_TEST_UNIT_READY:
	case OP_READ6:
	case OP_WRITE6:
	case OP_READ10:
	case OP_WRITE10:
	case OP_READ12:
	case OP_WRITE12:
	case OP_READ_CAPACITY10:
	case OP_READ_FORMAT_CAPACITIES:
	case OP_VERIFY10:
	case OP_SYNCHRONIZE_CACHE10:
	case OP_MODE_SENSE6:
	case OP_MODE_SENSE10:
		return true;
	default:
		return false;
	}
}

static void release(struct s10a_lu *lu)
{
	if (!lu->acquired)
		return;
	lu->acquired = false;
	lu->releases++;
	if (lu->on_release)
		lu->on_release(lu->ctx);
}

void s10a_lu_init(struct s10a_lu *lu, const struct wb_blockdev *dev,
		  const char *serial, const struct wb_device *device)
{
	size_t n = serial ? strlen(serial) : 0;
	size_t p = strlen(device->ident.product_text);

	memset(lu, 0, sizeof(*lu));
	lu->dev = dev;
	if (n > S10A_SERIAL_MAX)
		n = S10A_SERIAL_MAX;
	if (n)
		memcpy(lu->serial, serial, n);
	/* S-10a.042: device.product_text and " Disk", at most 16 bytes */
	if (p > S10A_PRODUCT_MAX - (sizeof(disk) - 1u))
		p = S10A_PRODUCT_MAX - (sizeof(disk) - 1u);
	memcpy(lu->product, device->ident.product_text, p);
	memcpy(lu->product + p, disk, sizeof(disk) - 1u);
}

void s10a_lu_offer(struct s10a_lu *lu)
{
	/* Each offer owes one unit attention (S-10a.049). */
	if (!lu->offered)
		lu->ua = true;
	lu->offered = true;
	lu->stopped = false;
	lu->acquired = false;
}

void s10a_lu_withdraw(struct s10a_lu *lu, enum s10a_withdrawal why)
{
	/* Ends a hold, a release unless for an own write (S-10a.050); drops a
	 * unit attention owed (S-10a.049). */
	if (why != S10A_WITHDRAW_OWN_WRITE)
		release(lu);
	lu->offered = false;
	lu->ua = false;
	lu->acquired = false;
	lu->withdrawals++;	/* a WRITE under way stores nothing more */
}

void s10a_lu_bus_reset(struct s10a_lu *lu)
{
	release(lu);
	lu->stopped = false;
}

void s10a_lu_select_configuration(struct s10a_lu *lu)
{
	release(lu);
	lu->stopped = false;
}

void s10a_lu_deconfigure(struct s10a_lu *lu)
{
	release(lu);
}

/* --- Replies ------------------------------------------------------------ */

static void ok(struct s10a_lu *lu, struct s10a_scsi_cmd *cmd, uint32_t len,
	       enum s10a_dir dir)
{
	lu->key = lu->asc = lu->ascq = 0;
	lu->info = 0;
	cmd->status = S10A_SCSI_GOOD;
	cmd->len = len;
	cmd->dir = len ? dir : S10A_DIR_NONE;
	cmd->data = dir == S10A_DIR_IN ? lu->reply : NULL;
}

/* An inline data-in reply of n bytes, cut to the allocation length. */
static void ok_in(struct s10a_lu *lu, struct s10a_scsi_cmd *cmd, uint32_t n,
		  uint32_t alloc)
{
	ok(lu, cmd, n < alloc ? n : alloc, S10A_DIR_IN);
}

static void fail(struct s10a_lu *lu, struct s10a_scsi_cmd *cmd, uint8_t key,
		 uint8_t asc, uint32_t info)
{
	lu->key = key;
	lu->asc = asc;
	lu->ascq = 0;
	lu->info = info;
	cmd->status = S10A_SCSI_CHECK;
	cmd->len = 0;
	cmd->dir = S10A_DIR_NONE;
	cmd->data = NULL;
	cmd->blocks = false;
}

static void fail_field(struct s10a_lu *lu, struct s10a_scsi_cmd *cmd)
{
	fail(lu, cmd, S10A_KEY_ILLEGAL, 0x24, 0);
}

/* S-10a.043: fixed-format sense, 18 bytes. */
static void sense_bytes(const struct s10a_lu *lu, uint8_t *p)
{
	memset(p, 0, 18);
	p[0] = (uint8_t)(lu->info ? 0xF0 : 0x70);
	p[2] = lu->key;
	wb_put_be32(p + 3, lu->info);
	p[7] = 10;
	p[12] = lu->asc;
	p[13] = lu->ascq;
}

/* S-10a.042 */
static void inquiry(struct s10a_lu *lu, const uint8_t *cdb,
		    struct s10a_scsi_cmd *cmd)
{
	uint8_t *p = lu->reply;
	const uint32_t alloc = wb_get_be16(cdb + 3);
	const uint32_t sn = (uint32_t)strlen(lu->serial);
	uint32_t n;

	memset(p, 0, sizeof(lu->reply));
	if (cdb[1] & 1u) {
		switch (cdb[2]) {
		case 0x00:
			p[3] = 3;
			p[4] = 0x00;
			p[5] = 0x80;
			p[6] = 0x83;
			n = 7;
			break;
		case 0x80:
			p[1] = 0x80;
			p[3] = (uint8_t)sn;
			memcpy(p + 4, lu->serial, sn);
			n = 4 + sn;
			break;
		case 0x83: {
			/* k = 21 + the product's length + the serial's */
			const uint32_t h = (uint32_t)sizeof(designator_head) - 1u;
			const uint32_t pn = (uint32_t)strlen(lu->product);
			const uint32_t k = h + pn + 1u + sn;

			p[1] = 0x83;
			p[3] = (uint8_t)(k + 4u);
			p[4] = 0x02;	/* code set ASCII */
			p[5] = 0x01;	/* logical unit, T10 vendor ID */
			p[7] = (uint8_t)k;
			memcpy(p + 8, designator_head, h);
			memcpy(p + 8 + h, lu->product, pn);
			p[8 + h + pn] = ' ';
			memcpy(p + 9 + h + pn, lu->serial, sn);
			n = 8 + k;
			break;
		}
		default:
			fail_field(lu, cmd);
			return;
		}
	} else if (cdb[2]) {
		fail_field(lu, cmd);
		return;
	} else {
		p[1] = 0x80;	/* removable */
		p[2] = 0x02;
		p[3] = 0x02;
		p[4] = 31;
		memcpy(p + 8, vendor, sizeof(vendor) - 1u);
		memset(p + 16, ' ', 20);
		memcpy(p + 16, lu->product, strlen(lu->product));
		n = 36;
	}
	ok_in(lu, cmd, n, alloc);
}

static void request_sense(struct s10a_lu *lu, const uint8_t *cdb,
			  struct s10a_scsi_cmd *cmd)
{
	if (lu->ua) {
		/* S-10a.043: reported here, and consumed. */
		lu->ua = false;
		lu->key = S10A_KEY_UNIT_ATTENTION;
		lu->asc = 0x28;
		lu->ascq = 0;
		lu->info = 0;
	}
	sense_bytes(lu, lu->reply);
	ok_in(lu, cmd, 18, cdb[4]);
}

/* S-10a.044: the header and one short LBA block descriptor (SBC-2 6.4.2). */
static void mode_sense(struct s10a_lu *lu, const uint8_t *cdb, bool ten,
		       struct s10a_scsi_cmd *cmd)
{
	const unsigned pc = cdb[2] >> 6, page = cdb[2] & 0x3Fu;
	const bool dbd = (cdb[1] & 0x08u) != 0;
	uint8_t *p = lu->reply;
	uint8_t *bd;
	uint32_t n, alloc;

	if (cdb[3]) {
		fail_field(lu, cmd);
		return;
	}
	if (pc == 3) {
		fail(lu, cmd, S10A_KEY_ILLEGAL, 0x39, 0);
		return;
	}
	if (page != 0x3F) {
		fail_field(lu, cmd);
		return;
	}
	memset(p, 0, sizeof(lu->reply));
	if (ten) {
		p[1] = (uint8_t)(dbd ? 6 : 14);
		p[7] = (uint8_t)(dbd ? 0 : 8);
		bd = p + 8;
		alloc = wb_get_be16(cdb + 7);
	} else {
		p[0] = (uint8_t)(dbd ? 3 : 11);
		p[3] = (uint8_t)(dbd ? 0 : 8);
		bd = p + 4;
		alloc = cdb[4];
	}
	n = (uint32_t)(bd - p);
	if (!dbd) {
		wb_put_be32(bd, lu->dev->n_sectors);
		bd[6] = S10A_BLOCK >> 8;
		n += 8;
	}
	ok_in(lu, cmd, n, alloc);
}

static void read_capacity(struct s10a_lu *lu, const uint8_t *cdb,
			  struct s10a_scsi_cmd *cmd)
{
	if (!(cdb[8] & 1u) && wb_get_be32(cdb + 2)) {
		fail_field(lu, cmd);
		return;
	}
	wb_put_be32(lu->reply, lu->dev->n_sectors - 1u);
	wb_put_be32(lu->reply + 4, S10A_BLOCK);
	ok_in(lu, cmd, 8, 8);
}

static void read_format_capacities(struct s10a_lu *lu, const uint8_t *cdb,
				   struct s10a_scsi_cmd *cmd)
{
	uint8_t *p = lu->reply;

	memset(p, 0, 12);
	p[3] = 8;
	wb_put_be32(p + 4, lu->dev->n_sectors);
	p[8] = 0x02;	/* formatted medium */
	p[10] = S10A_BLOCK >> 8;
	ok_in(lu, cmd, 12, wb_get_be16(cdb + 7));
}

/* S-10a.048 */
static void start_stop(struct s10a_lu *lu, const uint8_t *cdb,
		       struct s10a_scsi_cmd *cmd)
{
	const uint8_t b = cdb[4];

	if (b >> 4) {
		ok(lu, cmd, 0, S10A_DIR_NONE);
		return;
	}
	if (b & 1u) {
		if (!lu->offered) {
			fail(lu, cmd, S10A_KEY_NOT_READY, 0x3A, 0);
			return;
		}
		lu->stopped = false;
	} else {
		lu->stopped = true;
		if (b & 2u)
			release(lu);
	}
	ok(lu, cmd, 0, S10A_DIR_NONE);
}

/* S-10a.045: READ and WRITE (6), (10), (12); VERIFY(10) with nothing read. */
static void read_write(struct s10a_lu *lu, const uint8_t *cdb, uint8_t op,
		       struct s10a_scsi_cmd *cmd)
{
	uint32_t lba, n;
	bool write = false, verify = false;

	switch (op) {
	case OP_WRITE6:
		write = true;
		/* fall through */
	case OP_READ6:
		lba = (uint32_t)(cdb[1] & 0x1Fu) << 16 | wb_get_be16(cdb + 2);
		n = cdb[4] ? cdb[4] : 256u;
		break;
	case OP_VERIFY10:
		if (cdb[1] & 0x02u) {	/* BYTCHK: no compare offered */
			fail_field(lu, cmd);
			return;
		}
		verify = true;
		lba = wb_get_be32(cdb + 2);
		n = wb_get_be16(cdb + 7);
		break;
	case OP_WRITE10:
		write = true;
		/* fall through */
	case OP_READ10:
		if (cdb[1] & 0xE0u) {
			fail_field(lu, cmd);
			return;
		}
		lba = wb_get_be32(cdb + 2);
		n = wb_get_be16(cdb + 7);
		break;
	case OP_WRITE12:
		write = true;
		/* fall through */
	default:
		if (cdb[1] & 0xE0u) {
			fail_field(lu, cmd);
			return;
		}
		lba = wb_get_be32(cdb + 2);
		n = wb_get_be32(cdb + 6);
		break;
	}
	if ((uint64_t)lba + n > lu->dev->n_sectors) {
		fail(lu, cmd, S10A_KEY_ILLEGAL, 0x21, lba);
		return;
	}
	if (verify || n == 0) {
		ok(lu, cmd, 0, S10A_DIR_NONE);
		return;
	}
	/* GOOD so far; s10a_scsi_done() settles it. */
	cmd->status = S10A_SCSI_GOOD;
	cmd->dir = write ? S10A_DIR_OUT : S10A_DIR_IN;
	cmd->len = n * S10A_BLOCK;
	cmd->data = NULL;
	cmd->blocks = true;
	cmd->lba = lba;
}

static void execute(struct s10a_lu *lu, const uint8_t *cdb, uint8_t op,
		    struct s10a_scsi_cmd *cmd)
{
	if (op == OP_REQUEST_SENSE) {
		request_sense(lu, cdb, cmd);
		return;
	}
	if (op == OP_INQUIRY) {
		inquiry(lu, cdb, cmd);
		return;
	}
	if (lu->ua) {
		/* S-10a.043: the next other command fails, once. */
		lu->ua = false;
		fail(lu, cmd, S10A_KEY_UNIT_ATTENTION, 0x28, 0);
		return;
	}
	switch (op) {
	case OP_TEST_UNIT_READY:
	case OP_PREVENT_ALLOW:
	case OP_SYNCHRONIZE_CACHE10:
		ok(lu, cmd, 0, S10A_DIR_NONE);
		break;
	case OP_START_STOP:
		start_stop(lu, cdb, cmd);
		break;
	case OP_MODE_SELECT6:
		ok(lu, cmd, cdb[4], S10A_DIR_OUT);
		break;
	case OP_MODE_SELECT10:
		ok(lu, cmd, wb_get_be16(cdb + 7), S10A_DIR_OUT);
		break;
	case OP_MODE_SENSE6:
		mode_sense(lu, cdb, false, cmd);
		break;
	case OP_MODE_SENSE10:
		mode_sense(lu, cdb, true, cmd);
		break;
	case OP_READ_CAPACITY10:
		read_capacity(lu, cdb, cmd);
		break;
	case OP_READ_FORMAT_CAPACITIES:
		read_format_capacities(lu, cdb, cmd);
		break;
	case OP_READ6:
	case OP_WRITE6:
	case OP_READ10:
	case OP_WRITE10:
	case OP_READ12:
	case OP_WRITE12:
	case OP_VERIFY10:
		read_write(lu, cdb, op, cmd);
		break;
	default:
		/* S-10a.046 */
		fail(lu, cmd, S10A_KEY_ILLEGAL, 0x20, 0);
		break;
	}
}

void s10a_scsi_command(struct s10a_lu *lu, const uint8_t *cdb,
		       unsigned cdb_len, struct s10a_scsi_cmd *cmd)
{
	uint8_t op;

	memset(cmd, 0, sizeof(*cmd));
	if (cdb_len == 0) {
		fail(lu, cmd, S10A_KEY_ILLEGAL, 0x20, 0);
		return;
	}
	op = cdb[0];
	if (cdb_len < group_len(op)) {
		/* S-10a.040: a CDB shorter than its group is refused. */
		fail_field(lu, cmd);
		return;
	}
	cmd->media = media_op(op);
	/* S-10a.045, S-10a.047: no medium, or stopped with nothing owed. */
	if (cmd->media && (!lu->offered || (lu->stopped && !lu->ua))) {
		fail(lu, cmd, S10A_KEY_NOT_READY, 0x3A, 0);
		return;
	}
	execute(lu, cdb, op, cmd);
	/* S-10a.050: a media command that completes GOOD acquires. */
	if (cmd->media && !cmd->blocks && cmd->status == S10A_SCSI_GOOD)
		lu->acquired = true;
}

uint8_t s10a_scsi_done(struct s10a_lu *lu, const struct s10a_scsi_cmd *cmd,
		       bool good, bool write)
{
	struct s10a_scsi_cmd c = *cmd;

	if (!good) {
		/* S-10a.043: 00h/00h for a read, 20h/00h for a write. */
		fail(lu, &c, S10A_KEY_MEDIUM_ERROR, (uint8_t)(write ? 0x20 : 0x00),
		     0);
		return S10A_SCSI_CHECK;
	}
	lu->key = lu->asc = lu->ascq = 0;
	lu->info = 0;
	/* A command that outlived a withdrawal completes but acquires
	 * nothing: the withdrawal ended the hold (S-10a.049). */
	if (lu->offered)
		lu->acquired = true;
	return S10A_SCSI_GOOD;
}

void s10a_scsi_withdrawn(struct s10a_lu *lu)
{
	struct s10a_scsi_cmd c = { 0 };

	fail(lu, &c, S10A_KEY_NOT_READY, 0x3A, 0);
}

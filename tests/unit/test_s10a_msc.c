/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10a mass storage. Checks S-10a.040 to S-10a.052 against the vectors of
 * tests/oracle/S-10a-usb-msc (scsi.json, medium.json) and the BOT 1.0 6.7 cases
 * the conformance package's run_bot gives (tests/vectors/s10a).
 */
#include <stdint.h>
#include <string.h>

#include "waffle/device.h"
#include "waffle/s10a_msc.h"

#include "support/check.h"
#include "s15_sha256.h"
#include "vectors/test_s10a_vectors.h"

#define B(r) (s10av_blob + (r).off)
#define BLOCKS 454656u			/* S-12.001 */

/* --- A sparse disk: every block reads 0 until written ------------------- */

#define SLOTS 64
static struct {
	uint32_t lba[SLOTS];
	uint8_t data[SLOTS][S10A_BLOCK];
	unsigned used;
	bool fail;
} disk;

static uint8_t *slot(uint32_t lba, bool make)
{
	unsigned i;

	for (i = 0; i < disk.used; i++)
		if (disk.lba[i] == lba)
			return disk.data[i];
	if (!make || disk.used == SLOTS)
		return NULL;
	disk.lba[disk.used] = lba;
	memset(disk.data[disk.used], 0, S10A_BLOCK);
	return disk.data[disk.used++];
}

static int disk_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	uint8_t *p = buf;

	(void)ctx;
	if (disk.fail || lba + count > BLOCKS)
		return -1;
	for (; count; count--, lba++, p += S10A_BLOCK) {
		const uint8_t *s = slot(lba, false);

		if (s)
			memcpy(p, s, S10A_BLOCK);
		else
			memset(p, 0, S10A_BLOCK);
	}
	return 0;
}

static int disk_write(void *ctx, uint32_t lba, const void *buf,
		      uint32_t count)
{
	const uint8_t *p = buf;

	(void)ctx;
	if (disk.fail || lba + count > BLOCKS)
		return -1;
	for (; count; count--, lba++, p += S10A_BLOCK) {
		uint8_t *s = slot(lba, true);

		if (!s)
			return -1;
		memcpy(s, p, S10A_BLOCK);
	}
	return 0;
}

static const struct wb_blockdev dev = {
	.n_sectors = BLOCKS, .read = disk_read, .write = disk_write,
};
/* S-10a.039: the OP-1F's disk, device.ramdisk_blocks */
static const struct wb_blockdev dev_1f = {
	.n_sectors = S10AV_BLOCKS_1F, .read = disk_read, .write = disk_write,
};

static struct s10a_lu lu;
static const struct wb_device *unit_dev = &wb_device_opxy;

static void new_unit(const char *serial, bool offered)
{
	memset(&disk, 0, sizeof(disk));
	s10a_lu_init(&lu, unit_dev == &wb_device_op1f ? &dev_1f : &dev, serial,
		     unit_dev);
	if (offered)
		s10a_lu_offer(&lu);
}

/* --- S-10a.042 to S-10a.052: the unit, command by command ---------------- */

static void hex_of(const uint8_t *b, size_t n, char *out)
{
	static const char hex[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < n; i++) {
		out[2 * i] = hex[b[i] >> 4];
		out[2 * i + 1] = hex[b[i] & 15];
	}
	out[2 * n] = '\0';
}

static uint8_t big[256 * S10A_BLOCK];

/* Runs one command on the unit as a host would see it, data moved whole. */
static void command_step(const struct s10av_step *s)
{
	struct s10a_scsi_cmd cmd;
	uint8_t status;
	const uint8_t *data = NULL;
	uint32_t n = 0;

	s10a_scsi_command(&lu, B(s->cdb), (unsigned)s->cdb.len, &cmd);
	status = cmd.status;
	if (cmd.blocks) {
		const uint32_t count = cmd.len / S10A_BLOCK;

		CHECK(cmd.len <= sizeof(big));
		if (cmd.dir == S10A_DIR_IN) {
			CHECK(disk_read(NULL, cmd.lba, big, count) == 0);
			data = big;
			n = cmd.len;
		} else {
			CHECK(s->data_out.len == cmd.len);
			CHECK(disk_write(NULL, cmd.lba, B(s->data_out),
					 count) == 0);
		}
		status = s10a_scsi_done(&lu, &cmd, true,
					cmd.dir == S10A_DIR_OUT);
	} else if (cmd.dir == S10A_DIR_IN) {
		data = cmd.data;
		n = cmd.len;
	} else if (cmd.dir == S10A_DIR_OUT) {
		CHECK(s->data_out.len == cmd.len);
	}
	CHECK(status == s->status);
	CHECK(n == s->data_in_len);
	if (s->data_in.present) {
		CHECK(n == s->data_in.len &&
		      (n == 0 || memcmp(data, B(s->data_in), n) == 0));
	} else if (data) {
		struct s15_sha256 h;
		char hash[65], want[65];
		uint32_t i;

		s15_sha256_init(&h);
		for (i = 0; i < n; i++)
			s15_sha256_byte(&h, data[i]);
		s15_sha256_hex(&h, hash);
		hex_of(B(s->data_in_sha256), 32, want);
		CHECK(strcmp(hash, want) == 0);
	}
	if (s->has_sense) {
		CHECK(lu.key == s->sense[0]);
		CHECK(lu.asc == s->sense[1]);
		CHECK(lu.ascq == s->sense[2]);
	}
}

static void run_steps(const struct s10av_step *steps, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		const struct s10av_step *s = &steps[i];
		const unsigned before = check_failures;

		switch (s->event) {
		case 0:
			command_step(s);
			break;
		case 1:
			s10a_lu_withdraw(&lu, S10A_WITHDRAW_SCAN);
			break;
		case 9:
			s10a_lu_withdraw(&lu, S10A_WITHDRAW_OWN_WRITE);
			break;
		case 2:
			s10a_lu_offer(&lu);
			break;
		case 3:
			s10a_lu_bus_reset(&lu);
			break;
		case 4:
			s10a_lu_select_configuration(&lu);
			break;
		case 5:
			s10a_lu_deconfigure(&lu);
			break;
		case 6:
			CHECK(lu.releases == (uint32_t)s->releases);
			break;
		case 7:
		case 8:
			new_unit(s->serial, s->event == 7);
			break;
		default:
			CHECK(0);
			break;
		}
		if (check_failures != before)
			printf("  %s step %s: %s\n", unit_dev->name, s->seq, s->name);
	}
}

/* Both devices: the OP-1F's product strings and capacity (scsi_1f.json). */
static void test_vectors(void)
{
	unit_dev = &wb_device_opxy;
	run_steps(s10av_steps, ARRAY_LEN(s10av_steps));
	unit_dev = &wb_device_op1f;
	run_steps(s10av_steps_1f, ARRAY_LEN(s10av_steps_1f));
	unit_dev = &wb_device_opxy;
}

/* S-10a.046: every operation code not listed fails 20h/00h. */
static void test_refused_opcodes(void)
{
	static const uint8_t listed[] = {
		0x00, 0x03, 0x08, 0x0A, 0x12, 0x15, 0x1A, 0x1B, 0x1E, 0x23,
		0x25, 0x28, 0x2A, 0x2F, 0x35, 0x55, 0x5A, 0xA8, 0xAA,
	};
	struct s10a_scsi_cmd cmd;
	uint8_t cdb[16] = { 0 };
	unsigned op;

	new_unit("XY0A12BC", true);
	cdb[0] = 0x00;
	s10a_scsi_command(&lu, cdb, 6, &cmd);	/* the unit attention */
	for (op = 0; op < 256; op++) {
		if (memchr(listed, (int)op, sizeof(listed)))
			continue;
		cdb[0] = (uint8_t)op;
		s10a_scsi_command(&lu, cdb, 16, &cmd);
		CHECK(cmd.status == S10A_SCSI_CHECK);
		CHECK(lu.key == S10A_KEY_ILLEGAL && lu.asc == 0x20);
		CHECK(cmd.dir == S10A_DIR_NONE);
	}
}

/* S-10a.040: a CDB shorter than its group: ILLEGAL REQUEST 24h/00h. */
static void test_short_cdb(void)
{
	static const uint8_t read10[10] = { 0x28 };
	struct s10a_scsi_cmd cmd;

	new_unit("", true);
	s10a_scsi_command(&lu, read10, 6, &cmd);
	CHECK(cmd.status == S10A_SCSI_CHECK);
	CHECK(lu.key == S10A_KEY_ILLEGAL && lu.asc == 0x24);
}

/* A block device that fails ends the command with CHECK CONDITION. */
static void test_block_device_error(void)
{
	static const uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
	static const uint8_t tur[6] = { 0 };
	struct s10a_scsi_cmd cmd;

	new_unit("", true);
	s10a_scsi_command(&lu, tur, 6, &cmd);
	s10a_scsi_command(&lu, read10, 10, &cmd);
	CHECK(cmd.status == S10A_SCSI_GOOD && cmd.blocks);
	CHECK(s10a_scsi_done(&lu, &cmd, false, false) == S10A_SCSI_CHECK);
	CHECK(lu.key == S10A_KEY_MEDIUM_ERROR && lu.asc == 0 && lu.ascq == 0);
	/* S-10a.043: a block that cannot be stored. */
	s10a_scsi_command(&lu, read10, 10, &cmd);
	CHECK(s10a_scsi_done(&lu, &cmd, false, true) == S10A_SCSI_CHECK);
	CHECK(lu.key == S10A_KEY_MEDIUM_ERROR && lu.asc == 0x20 &&
	      lu.ascq == 0);
	/* S-10a.049: a READ that outlives a withdrawal completes, GOOD, and
	 * acquires nothing; a scan's withdrawal of a held disk releases it. */
	s10a_scsi_command(&lu, read10, 10, &cmd);
	CHECK(s10a_scsi_done(&lu, &cmd, true, false) == S10A_SCSI_GOOD &&
	      lu.acquired);
	s10a_scsi_command(&lu, read10, 10, &cmd);
	s10a_lu_withdraw(&lu, S10A_WITHDRAW_SCAN);
	CHECK(lu.releases == 1u && !lu.acquired);
	CHECK(s10a_scsi_done(&lu, &cmd, true, false) == S10A_SCSI_GOOD &&
	      !lu.acquired);
	s10a_lu_offer(&lu);
	s10a_scsi_command(&lu, tur, 6, &cmd);	/* the unit attention */
	s10a_scsi_command(&lu, read10, 10, &cmd);
	CHECK(s10a_scsi_done(&lu, &cmd, true, false) == S10A_SCSI_GOOD &&
	      lu.acquired);
	s10a_lu_withdraw(&lu, S10A_WITHDRAW_OWN_WRITE);
	CHECK(lu.releases == 1u && !lu.acquired);
}

/* --- S-10a.040, S-10a.041: the Bulk-Only Transport ----------------------- */

/* The host's view of the two bulk endpoints. */
static struct {
	const uint8_t *in;
	uint32_t in_n;
	uint8_t *out;
	uint32_t out_n;
	bool in_armed, out_armed, in_halted, out_halted;
	bool saw_in_halt, saw_out_halt;
} ep;

static bool xfer_in(void *ctx, const uint8_t *buf, uint32_t n)
{
	(void)ctx;
	if (ep.in_halted || ep.in_armed)
		return false;
	ep.in = buf;
	ep.in_n = n;
	ep.in_armed = true;
	return true;
}

static bool xfer_out(void *ctx, uint8_t *buf, uint32_t n)
{
	(void)ctx;
	if (ep.out_halted || ep.out_armed)
		return false;
	ep.out = buf;
	ep.out_n = n;
	ep.out_armed = true;
	return true;
}

static void stall_in(void *ctx)
{
	(void)ctx;
	ep.in_halted = true;
	ep.in_armed = false;
}

static void stall_out(void *ctx)
{
	(void)ctx;
	ep.out_halted = true;
	ep.out_armed = false;
}

static const struct s10a_bot_port port = {
	.xfer_in = xfer_in, .xfer_out = xfer_out,
	.stall_in = stall_in, .stall_out = stall_out,
};

static struct s10a_bot bot;

/* The host sends n bytes on bulk OUT; false when the endpoint is halted. */
static bool host_send(const uint8_t *p, uint32_t n)
{
	uint32_t k;

	if (ep.out_halted) {
		ep.saw_out_halt = true;
		return false;
	}
	if (!ep.out_armed)
		return false;
	k = n < ep.out_n ? n : ep.out_n;
	memcpy(ep.out, p, k);
	ep.out_armed = false;
	s10a_bot_xfer_done(&bot, false, k);
	return true;
}

/* The host takes what bulk IN offers, up to `room` bytes. */
static uint32_t host_take(uint8_t *dst, uint32_t room)
{
	uint32_t n;

	if (ep.in_halted) {
		ep.saw_in_halt = true;
		return 0;
	}
	if (!ep.in_armed)
		return 0;
	n = ep.in_n;
	CHECK(n <= room);
	if (n > room)
		n = room;
	memcpy(dst, ep.in, n);
	ep.in_armed = false;
	s10a_bot_xfer_done(&bot, true, n);
	return n;
}

/* CLEAR_FEATURE(ENDPOINT_HALT) as TinyUSB carries it to the driver. */
static void host_clear(bool in)
{
	if (in)
		ep.in_halted = false;
	else
		ep.out_halted = false;
	s10a_bot_clear_halt(&bot, in);
}

static void bot_setup(void)
{
	static const uint8_t tur[6] = { 0 };
	struct s10a_scsi_cmd cmd;

	new_unit("XY0A12BC", true);
	s10a_scsi_command(&lu, tur, 6, &cmd);	/* the first unit attention */
	memset(&ep, 0, sizeof(ep));
	s10a_bot_init(&bot, &lu, &port);
	s10a_bot_start(&bot);
}

static uint8_t rx[4096];

/* One command as BOT 1.0 has the host run it. */
static void host_command(const uint8_t *cbw, const uint8_t *data_out,
			 uint32_t *moved, uint8_t csw[13])
{
	const uint32_t host_len = (uint32_t)cbw[8] | (uint32_t)cbw[9] << 8 |
				  (uint32_t)cbw[10] << 16 |
				  (uint32_t)cbw[11] << 24;
	const bool host_in = (cbw[12] & 0x80u) != 0;
	unsigned guard;

	*moved = 0;
	memset(csw, 0, 13);
	CHECK(host_send(cbw, 31));
	for (guard = 0; guard < 64 && host_len && *moved < host_len; guard++) {
		if (host_in) {
			const uint32_t n = host_take(rx + *moved,
						     host_len - *moved);

			if (!n)
				break;
			*moved += n;
		} else {
			const uint32_t before = *moved;
			uint32_t k = host_len - *moved;

			if (ep.out_armed && k > ep.out_n)
				k = ep.out_n;
			if (!host_send(data_out + *moved, k))
				break;
			*moved = before + k;
		}
	}
	/* BOT 5.3.3: a halt seen in the data phase is cleared first. */
	if (ep.out_halted && !host_in && host_len) {
		ep.saw_out_halt = true;
		host_clear(false);
	}
	if (ep.in_halted) {
		ep.saw_in_halt = true;
		host_clear(true);
	}
	CHECK(host_take(csw, 13) == 13);
}

static void test_bot_cases(void)
{
	size_t i;

	for (i = 0; i < ARRAY_LEN(s10av_bots); i++) {
		const struct s10av_bot *v = &s10av_bots[i];
		const unsigned before = check_failures;
		uint8_t csw[13], blk[S10A_BLOCK];
		uint32_t moved;
		int stall;

		bot_setup();
		host_command(B(v->cbw), B(v->data_out), &moved, csw);
		stall = ep.saw_in_halt ? 1 : ep.saw_out_halt ? 2 : 0;
		CHECK(moved == v->moved);
		if (v->data_in.len)
			CHECK(memcmp(rx, B(v->data_in), v->data_in.len) == 0);
		CHECK(memcmp(csw, B(v->csw), 13) == 0);
		CHECK(stall == v->stall);
		CHECK(disk_read(NULL, 5, blk, 1) == 0);
		CHECK(memcmp(blk, B(v->block5), S10A_BLOCK) == 0);
		/* One command at a time: the next CBW is armed after the CSW. */
		CHECK(bot.state == S10A_BOT_CBW && ep.out_armed);
		if (check_failures != before)
			printf("  BOT case %d: %s\n", v->bot_case, v->name);
	}
}

static void put_cbw(uint8_t cbw[31], uint32_t tag, uint32_t len, bool in,
		    const uint8_t *cdb, uint8_t cdb_len)
{
	memset(cbw, 0, 31);
	cbw[0] = 'U';
	cbw[1] = 'S';
	cbw[2] = 'B';
	cbw[3] = 'C';
	cbw[4] = (uint8_t)tag;
	cbw[8] = (uint8_t)len;
	cbw[9] = (uint8_t)(len >> 8);
	cbw[12] = in ? 0x80 : 0;
	cbw[14] = cdb_len;
	memcpy(cbw + 15, cdb, cdb_len);
}

/* S-10a.040, S-10a.033: a refused CBW halts both endpoints until a reset. */
static void test_cbw_refusals(void)
{
	static const uint8_t tur[6] = { 0 };
	uint8_t cbw[32], csw[13], reply;
	uint32_t moved;
	unsigned which;

	for (which = 0; which < 4; which++) {
		uint32_t len = 31;

		bot_setup();
		put_cbw(cbw, 7, 0, false, tur, 6);
		cbw[31] = 0;
		if (which == 0)
			len = 30;
		else if (which == 1)
			len = 32;
		else if (which == 2)
			cbw[3] = 'X';
		else
			cbw[13] = 1;	/* LUN 1 */
		CHECK(host_send(cbw, len));
		CHECK(ep.in_halted && ep.out_halted);
		CHECK(s10a_bot_reset_owed(&bot));
		/* CLEAR_FEATURE leaves both halted. */
		host_clear(true);
		host_clear(false);
		CHECK(ep.in_halted && ep.out_halted);
		/* The reset recovery: reset, then both halts cleared. */
		CHECK(s10a_bot_class_request(&bot, 0x21, 0xFF, 0, 0, &reply));
		CHECK(!s10a_bot_reset_owed(&bot));
		host_clear(true);
		host_clear(false);
		CHECK(!ep.in_halted && !ep.out_halted && ep.out_armed);
		put_cbw(cbw, 8, 0, false, tur, 6);
		host_command(cbw, rx, &moved, csw);
		CHECK(csw[4] == 8 && csw[12] == 0);
	}
}

/* S-10a.041 */
static void test_class_requests(void)
{
	uint8_t reply = 0xAA;

	bot_setup();
	CHECK(s10a_bot_class_request(&bot, 0xA1, 0xFE, 0, 1, &reply));
	CHECK(reply == 0);
	CHECK(!s10a_bot_class_request(&bot, 0xA1, 0xFE, 1, 1, &reply));
	CHECK(!s10a_bot_class_request(&bot, 0xA1, 0xFE, 0, 2, &reply));
	CHECK(!s10a_bot_class_request(&bot, 0x21, 0xFE, 0, 1, &reply));
	CHECK(s10a_bot_class_request(&bot, 0x21, 0xFF, 0, 0, &reply));
	CHECK(!s10a_bot_class_request(&bot, 0x21, 0xFF, 0, 1, &reply));
	CHECK(!s10a_bot_class_request(&bot, 0xA1, 0xFF, 0, 0, &reply));
	CHECK(!s10a_bot_class_request(&bot, 0x21, 0x00, 0, 0, &reply));
}

/* S-10a.032, S-10a.080: re-selecting abandons a command with no CSW. */
static void test_abandon(void)
{
	static const uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 16, 0 };
	uint8_t cbw[31];

	bot_setup();
	put_cbw(cbw, 9, 16 * 512, true, read10, 10);
	CHECK(host_send(cbw, 31));
	CHECK(bot.state == S10A_BOT_DATA_IN && ep.in_armed);
	CHECK(s10a_bot_busy(&bot));
	memset(&ep, 0, sizeof(ep));	/* the endpoints closed and opened */
	s10a_bot_start(&bot);
	CHECK(bot.state == S10A_BOT_CBW && ep.out_armed && !ep.in_armed);
	CHECK(!s10a_bot_busy(&bot));
}

/* A long READ is moved in S10A_BOT_BUF chunks, block contents intact. */
static void test_long_read(void)
{
	static const uint8_t write10[10] = { 0x2A, 0, 0, 0, 0, 40, 0, 0, 1, 0 };
	static const uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 48, 0 };
	uint8_t cbw[31], csw[13], blk[S10A_BLOCK];
	static uint8_t all[48 * S10A_BLOCK];
	uint32_t moved, i, off = 0;
	unsigned guard;

	bot_setup();
	for (i = 0; i < S10A_BLOCK; i++)
		blk[i] = (uint8_t)(i * 3u);
	put_cbw(cbw, 1, S10A_BLOCK, false, write10, 10);
	host_command(cbw, blk, &moved, csw);
	CHECK(csw[12] == 0 && moved == S10A_BLOCK);
	put_cbw(cbw, 2, 48 * S10A_BLOCK, true, read10, 10);
	CHECK(host_send(cbw, 31));
	for (guard = 0; guard < 16 && off < sizeof(all); guard++) {
		CHECK(ep.in_n <= S10A_BOT_BUF);
		off += host_take(all + off, (uint32_t)sizeof(all) - off);
	}
	CHECK(off == sizeof(all));
	CHECK(memcmp(all + 40 * S10A_BLOCK, blk, S10A_BLOCK) == 0);
	CHECK(host_take(csw, 13) == 13 && csw[12] == 0);
	CHECK(lu.acquired);
}

int main(void)
{
	RUN(test_vectors);
	RUN(test_refused_opcodes);
	RUN(test_short_cdb);
	RUN(test_block_device_error);
	RUN(test_bot_cases);
	RUN(test_cbw_refusals);
	RUN(test_class_requests);
	RUN(test_abandon);
	RUN(test_long_read);
	return check_summary("test_s10a_msc");
}

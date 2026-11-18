/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07.081-S-07.083: PSoC bootloader-host packets, the enter probe, the
 * .cyacd reader and a programming run.
 */
#include "waffle/s07_cypress.h"
#include "waffle/bytes.h"

#define REPLY_TIMEOUT_US 5000000u	/* S-07.083: 5 s per command */
#define MAX_ARRAYS 8u

const uint8_t s07_cy_exit_packet[7] = { 0x01, 0x3B, 0x00, 0x00, 0xC4, 0xFF,
					0x17 };

/* S-07.082. The sum variant is plain arithmetic, done here; the CRC
 * variant's CRC-16 comes from the caller: CRC-16/X-25, its two bytes
 * swapped (check of "123456789": 0x6E90, sent 90 6E). */
uint16_t s07_cy_checksum(const uint8_t *p, size_t n, int kind,
			 s07_crc16_fn crc)
{
	uint16_t sum = 0;

	if (kind == S07_CY_CRC) {
		uint16_t c = crc(p, n);

		return (uint16_t)((uint16_t)(c << 8) | (c >> 8));
	}
	for (size_t i = 0; i < n; i++)
		sum = (uint16_t)(sum + p[i]);
	return (uint16_t)(0u - sum);
}

size_t s07_cy_packet(uint8_t code, const uint8_t *data, size_t n, int kind,
		     s07_crc16_fn crc, uint8_t *out)
{
	out[0] = S07_CY_SOP;
	out[1] = code;
	wb_put_le16(out + 2, (uint16_t)n);
	if (n)
		wb_copy(out + 4, data, n);
	wb_put_le16(out + 4 + n, s07_cy_checksum(out, 4 + n, kind, crc));
	out[6 + n] = S07_CY_EOP;
	return 7 + n;
}

/* S-07.081, S-16.042: at most 15 bytes taken; the rest cannot matter. */
bool s07_cy_probe_ok(const uint8_t *rx, size_t n)
{
	size_t i = 0;

	while (i < n && rx[i] != S07_CY_SOP)
		i++;
	if (i + 15u > n)
		return false;	/* fewer than 15 bytes from the first 01 */
	rx += i;
	return rx[1] == 0x00 && rx[2] == 0x08 && rx[3] == 0x00 &&
	       rx[14] == S07_CY_EOP;
}

/* ---- .cyacd ---- */

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Hex digit pairs s[0..n) into out (max bytes); the count, or -1 for an odd
 * or non-hex run. */
static long hexbytes(const char *s, size_t n, uint8_t *out, size_t max)
{
	if (n % 2u || n / 2u > max)
		return -1;
	for (size_t i = 0; i < n; i += 2) {
		int h = hexval(s[i]), l = hexval(s[i + 1]);

		if (h < 0 || l < 0)
			return -1;
		out[i / 2u] = (uint8_t)(h << 4 | l);
	}
	return (long)(n / 2u);
}

/*
 * S-07.083: the next line at *pos that is not a comment, its 0D bytes left
 * out, into line (the first S07_CY_LINE_MAX + 1 characters): its length, or
 * -1 at the end of the text. A final 0A starts no empty line.
 */
static long next_line(const char *t, size_t len, size_t *pos, char *line)
{
	while (*pos < len) {
		size_t n = 0;
		bool lf;

		while (*pos < len && t[*pos] != '\n') {
			char c = t[(*pos)++];

			if (c == '\r')
				continue;
			if (n <= S07_CY_LINE_MAX)
				line[n] = c;
			n++;
		}
		lf = *pos < len;
		if (lf)
			(*pos)++;
		if (n == 0 && !lf)
			return -1;	/* only 0D bytes after the last 0A */
		if (n > 0 && line[0] == '#')
			continue;	/* a comment */
		return (long)n;
	}
	return -1;
}

static char line_buf[S07_CY_LINE_MAX + 1];

/* S-07.083: a header of 12 hex digits: silicon id (BE), revision, type. */
bool s07_cyacd_open(struct s07_cyacd *f, const char *text, size_t len)
{
	uint8_t h[6];
	size_t pos = 0;

	if (next_line(text, len, &pos, line_buf) != 12 ||
	    hexbytes(line_buf, 12, h, sizeof h) != 6)
		return false;
	f->text = text;
	f->len = len;
	f->silicon_id = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 |
			(uint32_t)h[2] << 8 | h[3];
	f->silicon_rev = h[4];
	f->checksum_type = h[5];
	f->rows_at = pos;
	/* S-07.083: type 01 too is bad; the enter packet's fixed sum check can
	 * enter only a sum-checking PSoC bootloader */
	return f->checksum_type == S07_CY_SUM;
}

/* A row line :AARRRRLLLL<data>CC, integers big-endian (S-07.083). */
int s07_cyacd_next(const struct s07_cyacd *f, size_t *pos,
		   struct s07_cyacd_row *row)
{
	static uint8_t b[(S07_CY_LINE_MAX - 1u) / 2u];
	long n = next_line(f->text, f->len, pos, line_buf);

	if (n < 0)
		return 0;
	if ((size_t)n > S07_CY_LINE_MAX || line_buf[0] != ':')
		return -1;	/* an empty line too */
	n = hexbytes(line_buf + 1, (size_t)n - 1u, b, sizeof b);
	if (n < 6)
		return -1;
	row->array = b[0];
	row->row = wb_get_be16(b + 1);
	row->len = wb_get_be16(b + 3);
	if ((size_t)n != (size_t)row->len + 6u)
		return -1;
	wb_copy(row->data, b + 5, row->len);
	row->check = b[5 + row->len];
	return 1;
}

bool s07_cyacd_check(const struct s07_cyacd *f)
{
	static struct s07_cyacd_row row;
	uint8_t arrays[MAX_ARRAYS];
	unsigned narrays = 0, rows = 0;
	size_t pos = f->rows_at;
	int got;

	while ((got = s07_cyacd_next(f, &pos, &row)) == 1) {
		unsigned a = 0;

		while (a < narrays && arrays[a] != row.array)
			a++;
		if (a == narrays) {
			if (narrays == MAX_ARRAYS)
				return false;
			arrays[narrays++] = row.array;
		}
		rows++;
	}
	return got == 0 && rows > 0;
}

/* S-07.083: (CC + AA + row bytes + length bytes) mod 256. */
uint8_t s07_cy_row_expected(const struct s07_cyacd_row *r)
{
	return (uint8_t)((unsigned)r->check + r->array + (r->row & 0xFFu) +
			 ((unsigned)r->row >> 8) + (r->len & 0xFFu) +
			 ((unsigned)r->len >> 8));
}

/* ---- a run (S-07.083) ---- */

struct run {
	const struct s07_cy_io *io;
	int kind;
	s07_crc16_fn crc;
	struct s07_cy_result *res;
	uint8_t reply[7 + 8];
};

static bool fail(struct run *r, uint8_t at, uint8_t why)
{
	r->res->ok = false;
	r->res->at = at;
	r->res->why = why;
	return false;
}

/* Send a command and take its reply of 7 + nreply bytes. */
static bool cmd(struct run *r, uint8_t code, const uint8_t *data, size_t n,
		size_t nreply)
{
	uint8_t p[S07_CY_MAX_PACKET];
	size_t len = s07_cy_packet(code, data, n, r->kind, r->crc, p);

	r->io->send(r->io->ctx, p, len);
	if (!r->io->recv(r->io->ctx, r->reply, 7 + nreply, REPLY_TIMEOUT_US))
		return fail(r, code, S07_CY_WHY_TIMEOUT);
	if (r->reply[1] != 0x00)
		return fail(r, code, S07_CY_WHY_STATUS);
	/* the checksum is not checked, which S-07.083 allows */
	if (r->reply[0] != S07_CY_SOP || wb_get_le16(r->reply + 2) != nreply ||
	    r->reply[6 + nreply] != S07_CY_EOP)
		return fail(r, code, S07_CY_WHY_MALFORMED);
	return true;
}

/* S-07.083 step 2: one row, its data split so that no packet exceeds 62. */
static bool program_row(struct run *r, const struct s07_cyacd_row *row)
{
	uint8_t d[S07_CY_MAX_PACKET];
	size_t off = 0, rest;

	while (row->len - off > S07_CY_MAX_PACKET - 10u) {
		size_t n = row->len - off;

		/* a full send data, or the 53 or 54 bytes left, which a
		 * program row could not carry */
		if (n > S07_CY_MAX_PACKET - 7u)
			n = S07_CY_MAX_PACKET - 7u;
		if (!cmd(r, S07_CY_SEND_DATA, row->data + off, n, 0))
			return false;
		off += n;
	}
	rest = row->len - off;
	d[0] = row->array;
	wb_put_le16(d + 1, row->row);
	wb_copy(d + 3, row->data + off, rest);
	if (!cmd(r, S07_CY_PROGRAM_ROW, d, 3 + rest, 0))
		return false;
	if (!cmd(r, S07_CY_VERIFY_ROW, d, 3, 1))
		return false;
	if (r->reply[4] != s07_cy_row_expected(row))
		return fail(r, S07_CY_VERIFY_ROW, S07_CY_WHY_ROWCHECK);
	return true;
}

void s07_cy_run(const struct s07_cy_io *io, const struct s07_cyacd *f,
		const uint8_t unlock[S07_CY_UNLOCK_LEN], s07_crc16_fn crc,
		struct s07_cy_result *res)
{
	static struct s07_cyacd_row row;
	struct run r = { io, S07_CY_SUM, crc, res, { 0 } };	/* S-07.082 */
	uint8_t arrays[MAX_ARRAYS];
	uint16_t lo[MAX_ARRAYS], hi[MAX_ARRAYS];
	unsigned narrays = 0;
	size_t pos = f->rows_at;
	uint8_t p[S07_CY_MAX_PACKET];
	bool valid;
	int got;

	res->ok = true;
	res->at = 0;
	res->why = S07_CY_WHY_NONE;

	/* step 1: enter; silicon id and revision; bootloader version byte */
	if (!cmd(&r, S07_CY_ENTER, unlock, S07_CY_UNLOCK_LEN, 8))
		return;
	if (wb_get_le32(r.reply + 4) != f->silicon_id ||
	    r.reply[8] != f->silicon_rev) {
		(void)fail(&r, S07_CY_ENTER, S07_CY_WHY_DEVICE);
		return;
	}
	if (r.reply[11] != 0x01) {
		(void)fail(&r, S07_CY_ENTER, S07_CY_WHY_VERSION);
		return;
	}

	/* step 2: every row line in file order */
	while ((got = s07_cyacd_next(f, &pos, &row)) == 1) {
		unsigned a = 0;

		if (row.array >= S07_CY_EEPROM) {
			/* S-07.083: the EEPROM arrays get neither */
			if (!program_row(&r, &row))
				return;
			continue;
		}
		while (a < narrays && arrays[a] != row.array)
			a++;
		if (a == narrays) {
			if (narrays == MAX_ARRAYS) {
				(void)fail(&r, S07_CY_FLASH_SIZE, S07_CY_WHY_FILE);
				return;
			}
			if (!cmd(&r, S07_CY_FLASH_SIZE, &row.array, 1, 4))
				return;
			arrays[a] = row.array;
			lo[a] = wb_get_le16(r.reply + 4);
			hi[a] = wb_get_le16(r.reply + 6);
			narrays++;
		}
		if (row.row < lo[a] || row.row > hi[a]) {
			(void)fail(&r, S07_CY_FLASH_SIZE, S07_CY_WHY_RANGE);
			return;
		}
		if (!program_row(&r, &row))
			return;
	}
	if (got < 0) {
		(void)fail(&r, 0, S07_CY_WHY_FILE);
		return;
	}

	/* step 3: verify the application, then exit whatever it said */
	if (!cmd(&r, S07_CY_VERIFY_APP, 0, 0, 1))
		return;
	valid = r.reply[4] != 0x00;
	io->send(io->ctx, p, s07_cy_packet(S07_CY_EXIT, 0, 0, r.kind, crc, p));
	if (!valid)
		(void)fail(&r, S07_CY_VERIFY_APP, S07_CY_WHY_APP);
}

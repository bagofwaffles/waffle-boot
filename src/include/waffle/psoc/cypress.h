/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07.081-S-07.083: the PSoC bootloader-host protocol (Cypress AN68272's
 * packet format) as S-07 states it, the enter probe, the .cyacd (format v0)
 * reader and one programming run over a byte transport.
 */
#ifndef WAFFLE_PSOC_CYPRESS_H
#define WAFFLE_PSOC_CYPRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define S07_CY_SOP 0x01u
#define S07_CY_EOP 0x17u
#define S07_CY_MAX_PACKET 62u		/* S-07.082 */
#define S07_CY_UNLOCK_LEN 6u		/* the PSoC unlock, S-07.081 */

#define S07_CY_VERIFY_APP 0x31u
#define S07_CY_FLASH_SIZE 0x32u
#define S07_CY_SEND_DATA 0x37u
#define S07_CY_ENTER 0x38u
#define S07_CY_PROGRAM_ROW 0x39u
#define S07_CY_VERIFY_ROW 0x3Au
#define S07_CY_EXIT 0x3Bu

/* Checksum types (the .cyacd header's byte 5). */
#define S07_CY_SUM 0
#define S07_CY_CRC 1

/*
 * The CRC variant's CRC through the caller: CRC-16 with the reflected
 * polynomial 0x8408, init 0xFFFF, result complemented (CRC-16/X-25; check
 * value of "123456789": 0x906E). S-07.082 swaps its two bytes.
 */
typedef uint16_t (*s07_crc16_fn)(const uint8_t *data, size_t len);

/* The exit packet with the sum variant, 01 3B 00 00 C4 FF 17 (S-07.082). */
extern const uint8_t s07_cy_exit_packet[7];

/*
 * Build a packet (S-07.082) into out[S07_CY_MAX_PACKET + ...]: returns its
 * length, 7 + n.
 */
size_t s07_cy_packet(uint8_t code, const uint8_t *data, size_t n, int kind,
		     s07_crc16_fn crc, uint8_t *out);

/* S-07.082: the check over SOP..last data byte. */
uint16_t s07_cy_checksum(const uint8_t *p, size_t n, int kind,
			 s07_crc16_fn crc);

/*
 * S-07.081: the verdict on the bytes received after the enter packet:
 * bytes before the first 01 skipped, at most 15 bytes taken, success when
 * they are 15 and begin 01 00 08 00 and end with 17. The checksum is not
 * checked (S-07.081 allows either).
 */
bool s07_cy_probe_ok(const uint8_t *rx, size_t n);

/* A .cyacd file (S-07.083), read lazily from its text. */
struct s07_cyacd {
	const char *text;
	size_t len;
	uint32_t silicon_id;
	uint8_t silicon_rev;
	uint8_t checksum_type;
	size_t rows_at;		/* offset of the first row line */
};

/*
 * S-07.083: a line is at most 1535 characters, so a row line carries up to
 * 761 data bytes; array ids from 0x40 are the PSoC's EEPROM arrays.
 */
#define S07_CY_LINE_MAX 1535u
#define S07_CY_ROW_MAX 761u
#define S07_CY_EEPROM 0x40u

/* A row line: array, row number, data, line check. */
struct s07_cyacd_row {
	uint8_t array;
	uint16_t row;
	uint16_t len;
	uint8_t check;
	uint8_t data[S07_CY_ROW_MAX];
};

/* Parse the header line; false if it is malformed. */
bool s07_cyacd_open(struct s07_cyacd *f, const char *text, size_t len);

/*
 * The next row line at *pos (start with f->rows_at): 1 and the row, 0 at the
 * end, -1 for a malformed line (an empty one too). Lines end with 0A, 0D
 * bytes are ignored and lines starting with # are skipped (S-07.083). The line check is not compared here: the
 * verify-row comparison carries it (S-07.083).
 */
int s07_cyacd_next(const struct s07_cyacd *f, size_t *pos,
		   struct s07_cyacd_row *row);

/*
 * S-07.083: the whole file checked before entry: true unless it is bad (a
 * malformed row line, an empty one included, or none) or its row lines name
 * more than the 8 arrays a run can track, which S-07.083 lets a master
 * refuse. Line checks are not compared (the verify-row comparison does it).
 */
bool s07_cyacd_check(const struct s07_cyacd *f);

/* S-07.083: the row check the master expects from verify row. */
uint8_t s07_cy_row_expected(const struct s07_cyacd_row *r);

/*
 * The byte transport of a run, in raw mode: send a packet, and receive
 * exactly n reply bytes within timeout_us (false if they did not come).
 */
struct s07_cy_io {
	void (*send)(void *ctx, const uint8_t *p, size_t n);
	bool (*recv)(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_us);
	void *ctx;
};

/* Where a run stopped. */
struct s07_cy_result {
	bool ok;
	uint8_t at;		/* the command that failed (0 when ok) */
	uint8_t why;		/* S07_CY_WHY_* */
};

#define S07_CY_WHY_NONE 0
#define S07_CY_WHY_TIMEOUT 1
#define S07_CY_WHY_MALFORMED 2
#define S07_CY_WHY_STATUS 3
#define S07_CY_WHY_DEVICE 4
#define S07_CY_WHY_VERSION 5
#define S07_CY_WHY_RANGE 6
#define S07_CY_WHY_ROWCHECK 7
#define S07_CY_WHY_APP 8
#define S07_CY_WHY_FILE 9
#define S07_CY_WHY_UNLOCK 10		/* no PSoC unlock (S-07.081) */

/*
 * One programming run of S-07.083: enter (with the unlock), every row,
 * verify the application, then the exit packet (also when the application
 * check failed). A run that fails at another exchange stops there and sends no
 * exit (S-07.084 adds it).
 */
void s07_cy_run(const struct s07_cy_io *io, const struct s07_cyacd *f,
		const uint8_t unlock[S07_CY_UNLOCK_LEN], s07_crc16_fn crc,
		struct s07_cy_result *res);

#endif

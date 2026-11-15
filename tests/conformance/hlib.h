/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Support for the conformance harness's host programs.
 *
 * Each program reads commands from stdin, one per line: a keyword and
 * space-separated fields.  Numbers are C literals (decimal or 0x hex), byte
 * strings are hex with "-" for empty.  It prints one result line per
 * command on stdout, in the same notation, and the Python driver checks it
 * against the oracle.  An unknown command or a malformed field prints
 * "error ..." and the program exits nonzero at the end.
 */
#ifndef WAFFLE_CONFORMANCE_HLIB_H
#define WAFFLE_CONFORMANCE_HLIB_H

#include <stddef.h>
#include <stdint.h>

#define H_MAXTOK 64
#define H_MAXLINE 65536

struct h_cmd {
	int n;			/* number of fields, the keyword included */
	char *tok[H_MAXTOK];
};

/* Reads the next command; false at the end of the input. */
int h_next(struct h_cmd *c);

/* Field i as a number, or as bytes into out (at most max); the length. */
unsigned long h_num(const struct h_cmd *c, int i);
size_t h_hex(const struct h_cmd *c, int i, uint8_t *out, size_t max);
/* Field i, or NULL when the command has fewer fields. */
const char *h_str(const struct h_cmd *c, int i);

/* Output: fields are separated by single spaces, h_end ends the line. */
void h_put(const char *s);
void h_putu(unsigned long v);
void h_putx(unsigned long v);
void h_puthex(const uint8_t *p, size_t n);
void h_end(void);

/* Reports a bad command; the program then exits with status 1. */
void h_error(const struct h_cmd *c, const char *why);

/*
 * CRC-32 with zlib's convention (crc32(0, buf, len); check value 0xCBF43926),
 * the CRC the S-xx operation structs take.  Test glue: the harness gives the
 * logic a known-good CRC so that what is checked is the logic.
 */
uint32_t h_crc32(uint32_t crc, const uint8_t *buf, uint32_t len);

/* The exit status: 0, or 1 after any h_error. */
int h_status(void);

#endif /* WAFFLE_CONFORMANCE_HLIB_H */

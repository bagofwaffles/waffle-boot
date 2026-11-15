/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The boot store (S-02.096 - S-02.099): a small record of the master's own,
 * at offset 0 of the 4 KiB flash sector S02_STORE_OFF, read before the boot
 * decision. Little-endian:
 *
 *     +0      magic   u32  57 42 53 54 ("WBST")
 *     +4      version u16  1
 *     +6      length  u16  N, the payload's bytes, 1 to S02_STORE_PAYLOAD_MAX
 *     +8      payload N bytes
 *     +8+N    crc     u32  S-02.015's CRC-32 over bytes 0 to 8+N
 *
 * Payload version 1: byte 0 holds the flags, bit 0 "start in boot menu"
 * (S-15.175); bits 1 to 7 are written 0 and ignored when read. A writer
 * writes N = 1; a reader takes any N from 1 up, uses the bytes it knows and
 * ignores the rest. Anything else (another version, a bad magic or length,
 * a bad CRC, an erased sector, a failed read) gives the defaults. Reading
 * never writes. The CRC is wire_crc32() (zlib's, as S-02.015).
 *
 * The sector lies inside slot 1, just below its last sector (the start
 * log's, S-02.095). The image always wins: the store is usable only while
 * slot 1's record names an image that ends below the sector
 * (S02_STORE_SLOT_MAX); otherwise a read gives the defaults without reading
 * the sector and a write is refused before any erase.
 */
#ifndef WAFFLE_S02_STORE_H
#define WAFFLE_S02_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s02_flash.h"

#define S02_STORE_OFF         0xFE000u
#define S02_STORE_SIZE        S02_SECTOR_SIZE
/* The slot the sector lies in, and its longest image that leaves the
 * sector alone: 0x76000 bytes. */
#define S02_STORE_SLOT        1u
#define S02_STORE_SLOT_MAX    (S02_STORE_OFF - S02_SLOT1_OFF)

#define S02_STORE_MAGIC       0x54534257u	/* "WBST" as little-endian */
#define S02_STORE_VERSION     1u
#define S02_STORE_HEADER      8u
#define S02_STORE_CRC         4u
#define S02_STORE_PAYLOAD_MAX 4080u
#define S02_STORE_FLAG_START_MENU 0x01u
/* The record a writer writes: one payload byte. */
#define S02_STORE_WRITTEN     (S02_STORE_HEADER + 1u + S02_STORE_CRC)

struct s02_store {
	bool start_menu;	/* S-15.175: start in the boot menu */
};

/* What a write did. */
enum s02_store_rc {
	S02_STORE_OK,		/* erased, programmed, read back the same */
	S02_STORE_UNUSABLE,	/* slot 1's image may reach the sector: nothing done */
	S02_STORE_IO,		/* an erase, program or read failed */
	S02_STORE_VERIFY	/* the read-back differed */
};

/* The defaults: every flag off. */
void s02_store_defaults(struct s02_store *s);
/* The record at p, n bytes of the sector from its offset 0. False, with
 * the defaults, when it is not a valid record. */
bool s02_store_decode(const uint8_t *p, size_t n, struct s02_store *out);
/* The record written for s, into buf; its length, or 0 when cap is too
 * small. */
size_t s02_store_encode(const struct s02_store *s, uint8_t *buf, size_t cap);
/* Slot 1's record, read (S-02.013, no CRC pass): its tag matches (S-02.014)
 * and its length is from 1 to S02_STORE_SLOT_MAX. A trial mark's record is
 * judged the same way. */
bool s02_store_usable(const struct s02_env *env);
/* S-02.097: usable, then the sector with one read into env->scratch; false,
 * with the defaults, otherwise or when the record is not valid. */
bool s02_store_read(const struct s02_env *env, struct s02_store *out);
/* S-02.098: usable, then the sector erased, the record programmed, read back
 * and compared. Every erase, program and read lies inside the sector, and
 * is checked to before it is made. */
enum s02_store_rc s02_store_write(const struct s02_env *env, const struct s02_store *s);

#endif /* WAFFLE_S02_STORE_H */

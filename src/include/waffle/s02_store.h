/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The settings sector (S-02.096 - S-02.099, S-02.103 - S-02.105): each slot
 * keeps the settings of its own image in the 4 KiB sector just below its
 * start log, at offset S02_STORE_SLOT_MAX of the slot. Records are appended
 * one 256-byte page at a time, each at a page's start. Little-endian:
 *
 *     +0      magic   u32  57 42 53 53 ("WBSS")
 *     +4      version u16  1
 *     +6      length  u16  N, the payload's bytes, 1 to S02_STORE_PAYLOAD_MAX
 *     +8      stamp   u32  the CRC-32 the slot's record holds for its image
 *     +12     payload N bytes
 *     +12+N   crc     u32  S-02.015's CRC-32 over bytes 0 to 12+N
 *
 * Payload version 1: byte 0 holds the flags, bit 0 developer mode, bit 1
 * color, bit 2 MTP target macOS, bit 3 start in boot menu; bits 4 to 7 are
 * written 0 and ignored when read. A writer writes N = 1; a reader takes any
 * N from 1 up, uses the bytes it knows and ignores the rest.
 *
 * A page is used when any of its bytes is not 0xFF. The sector reads valid
 * when its used pages are its first ones, with no erased page before a used
 * one. A read takes the highest page whose record is valid and whose stamp
 * is the running slot's record's CRC; none gives the defaults. A save
 * programs the first erased page after the used ones, and erases the sector
 * first when all 16 pages are used or it does not read valid.
 *
 * The sector lies inside its slot: the store is used only while the slot's
 * record has the tag, a length from 1 to S02_STORE_SLOT_MAX, and the image
 * was found good at this start; otherwise a read gives the defaults without
 * reading the sector, and a save or a clear is refused before any erase.
 */
#ifndef WAFFLE_S02_STORE_H
#define WAFFLE_S02_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s02_flash.h"

#define S02_STORE_SIZE        S02_SECTOR_SIZE
#define S02_STORE_PAGES       (S02_STORE_SIZE / S02_PAGE_SIZE)
/* The longest image that leaves a slot its settings sector: 0x76000. */
#define S02_STORE_SLOT_MAX    (S02_SLOT_CAPACITY - 2u * S02_SECTOR_SIZE)

#define S02_STORE_MAGIC       0x53534257u	/* "WBSS" as little-endian */
#define S02_STORE_VERSION     1u
#define S02_STORE_HEADER      12u
#define S02_STORE_CRC         4u
#define S02_STORE_PAYLOAD_MAX (S02_PAGE_SIZE - S02_STORE_HEADER - S02_STORE_CRC)
#define S02_STORE_FLAG_DEV        0x01u
#define S02_STORE_FLAG_COLOR      0x02u
#define S02_STORE_FLAG_MTP_MACOS  0x04u
#define S02_STORE_FLAG_START_MENU 0x08u
/* The record a writer writes: one payload byte. */
#define S02_STORE_WRITTEN     (S02_STORE_HEADER + 1u + S02_STORE_CRC)

struct s02_store {
	bool dev_mode;		/* developer mode */
	bool color;		/* S-15.173 */
	bool mtp_macos;		/* S-15.174 */
	bool start_menu;	/* S-15.175 */
};

/* The running slot as this start found it (S-01.002, S-02.054): its index,
 * its record and whether its image passed S-02.016. established false: the
 * running slot is not known, and the store is not used. */
struct s02_store_slot {
	bool established;
	unsigned slot;
	struct s02_record rec;
	bool good;
};

/* What a save or a clear did. */
enum s02_store_rc {
	S02_STORE_OK,		/* written and read back, or nothing to write */
	S02_STORE_UNUSABLE,	/* the slot keeps no settings: nothing done */
	S02_STORE_IO,		/* an erase, program or read failed */
	S02_STORE_VERIFY	/* a read-back differed */
};

/* The settings sector of slot `slot` (0 or 1). */
uint32_t s02_store_off(unsigned slot);
/* The defaults: developer mode off, color off, MTP for macOS, start in boot
 * menu off. */
void s02_store_defaults(struct s02_store *s);
/* The record at p, n bytes from a page's start, with the stamp it must
 * hold. False, with the defaults, when it is not a valid record. */
bool s02_store_decode(const uint8_t *p, size_t n, uint32_t stamp, struct s02_store *out);
/* The record written for s, into buf; its length, or 0 when cap is too
 * small. */
size_t s02_store_encode(const struct s02_store *s, uint32_t stamp, uint8_t *buf, size_t cap);
/* Whether a slot record leaves its slot the settings sector: the tag and a
 * length from 1 to S02_STORE_SLOT_MAX. */
bool s02_store_record_ok(const struct s02_record *rec);
/* The settings a sector's bytes give for this stamp (S-02.097): the
 * highest page that holds a valid record with it; false, with the
 * defaults, for none. */
bool s02_store_pick(const uint8_t sector[S02_STORE_SIZE], uint32_t stamp, struct s02_store *out);
/* S-02.097: the running slot usable, its record read again and the same,
 * then the sector with one read into env->scratch; false, with the
 * defaults, otherwise. Reading never writes. */
bool s02_store_read(const struct s02_env *env, const struct s02_store_slot *run,
		    struct s02_store *out);
/* S-02.098: usable as for a read, then one page programmed, or the sector
 * erased and its first page programmed, and read back. A save of the value
 * the sector already gives writes nothing. Every erase, program and read
 * lies inside the running slot's settings sector. */
enum s02_store_rc s02_store_write(const struct s02_env *env, const struct s02_store_slot *run,
				  const struct s02_store *s);
/* S-02.105: the factory reset's clear: usable as for a save, then the
 * sector erased and read back erased; an erased sector is left alone. */
enum s02_store_rc s02_store_clear(const struct s02_env *env, const struct s02_store_slot *run);

#endif /* WAFFLE_S02_STORE_H */

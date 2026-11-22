/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The packed block's check and inflate (waffle/pack.h), apart from the
 * target, so that the host's tests run the stub's own code.
 */
#ifndef WB_STUB_UNPACK_H
#define WB_STUB_UNPACK_H

#include <stdbool.h>
#include <stdint.h>

#include "waffle/pack.h"

enum wb_unpack_rc {
	WB_UNPACK_OK = 0,
	WB_UNPACK_HEAD = 1,		/* magic, version, counts or the head's CRC */
	WB_UNPACK_SIZE = 2,		/* longer than the bytes loaded, or streams out of it */
	WB_UNPACK_STREAMS_CRC = 3,	/* the deflate streams' CRC-32 */
	WB_UNPACK_DEST = 4,		/* a run where nothing may be written */
	WB_UNPACK_INFLATE = 5,		/* a stream that does not inflate to its run */
	WB_UNPACK_OUT_CRC = 6,		/* the bytes written, against out_crc */
};

struct wb_unpack_ops {
	/* Where run [addr, addr + len) is written, or NULL: refused. A run
	 * meets no other, in ascending order; the map decides the rest. */
	uint8_t *(*dest)(void *ctx, uint32_t addr, uint32_t len);
	void *ctx;
	/* inflate's state comes from here, one inflate at a time */
	void *arena;
	uint32_t arena_len;
};

/* Whether [addr, addr + len) is where the stub writes a run: the main
 * image's link regions but L1, which the stream loads as it is (bf703.ld):
 * its L2 handover reserve, up to the fatal record (S-16.104), and its DDR
 * region. The packed block, the stub, the front and the hand-off record lie
 * outside both. tools/mkldr.py keeps the same list (STUB_WRITABLE). */
bool wb_unpack_writable(uint32_t addr, uint32_t len);

/* The arena inflate needs (its state, aligned, with room to spare). */
#define WB_UNPACK_ARENA 0x2400u

/*
 * Checks the packed block pk, of which avail bytes are loaded, and inflates
 * each run to ops->dest's address; *entry is the main image's entry. Every
 * check before the first write: the head, its CRC, the size, the streams'
 * CRC and every run's destination. Then each run inflated whole, and the
 * CRC-32 of what was written read back. Anything else: a refusal, and no
 * byte of the main image is to be run.
 */
enum wb_unpack_rc wb_unpack(const uint8_t *pk, uint32_t avail, const struct wb_unpack_ops *ops,
			    uint32_t *entry);

#endif /* WB_STUB_UNPACK_H */

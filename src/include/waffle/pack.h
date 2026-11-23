/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_PACK_H
#define WAFFLE_PACK_H

/*
 * The packed main image and the stub that unpacks it (src/stub/).
 *
 * The bootloader's stream (tools/mkldr.py --front --stub) carries the frozen
 * front, the stub, the main image's L1 runs as they are, and every other run
 * of the main image deflated into one packed block, loaded as data at
 * WB_PACK_START. The front checks all of it against its descriptor and
 * enters the descriptor's entry, the stub's. The stub checks the packed
 * block, inflates each run to its own address, checks what it wrote and
 * jumps to the main image's entry. Nothing here touches the front's memory
 * or the hand-off record.
 *
 * SPEC-Q: S-01.023 has the front enter "the main image"; here it enters the
 * stub, which enters the main image. S-01.013's DDR reserve now holds the
 * main image's link region (WB_IMAGE_DDR_*) and, above it, the packed block
 * (WB_PACK_*), both inside what the front reads (fr_mem_readable()).
 *
 * The packed block, little-endian:
 *   0x00 u32 magic WB_PACK_MAGIC
 *   0x04 u16 version WB_PACK_VERSION
 *   0x06 u16 head: bytes before the first stream, the head's CRC included
 *   0x08 u32 n_runs, 1 to WB_PACK_RUNS
 *   0x0C u32 entry: the main image's entry
 *   0x10 u32 size: the block's bytes, the head and every stream
 *   0x14 u32 streams_crc: CRC-32 of the bytes from head to size
 *   0x18 u32 out_len: the bytes of every run together
 *   0x1C u32 out_crc: CRC-32 of the runs' bytes, in run order
 *   0x20 {u32 addr, u32 len, u32 zlen}[n_runs]
 *   then u32: the CRC-32 of the head's bytes before it
 *   then each run's raw deflate stream, zlen bytes, in run order
 * CRC-32 is zlib's. A run's bytes are its stream inflated whole; a stream
 * that ends early or late, or leaves input, fails the block.
 */

#include <stdint.h>

#define WB_PACK_MAGIC    0x4B504257u	/* "WBPK" */
#define WB_PACK_VERSION  1u
#define WB_PACK_RUNS     8u
#define WB_PACK_HEAD_FIXED 0x20u
#define WB_PACK_HEAD(n)  (WB_PACK_HEAD_FIXED + 12u * (n) + 4u)

#define WB_PACK_O_MAGIC   0x00u
#define WB_PACK_O_VERSION 0x04u
#define WB_PACK_O_HEAD    0x06u
#define WB_PACK_O_NRUNS   0x08u
#define WB_PACK_O_ENTRY   0x0Cu
#define WB_PACK_O_SIZE    0x10u
#define WB_PACK_O_ZCRC    0x14u
#define WB_PACK_O_OUTLEN  0x18u
#define WB_PACK_O_OUTCRC  0x1Cu
#define WB_PACK_O_RUNS    WB_PACK_HEAD_FIXED

/*
 * Memory (S-01.012, S-01.013). The DDR reserve's first 3 MiB, which the
 * front reads as the image's (0x8803D000-0x8833D000), in two: the main
 * image's link region (bf703.ld), then the packed block, which nothing
 * writes after the stream has loaded it. The stub's own code, data and
 * stack are in L2 between the shared block and the L2 handover reserve,
 * which the main image leaves unused; its record lies at the top.
 */
#define WB_IMAGE_DDR_START 0x8803D000u
#define WB_IMAGE_DDR_END   0x882BD000u
#define WB_PACK_START      0x882BD000u
#define WB_PACK_END        0x8833D000u
#define WB_STUB_START      0x08031000u
#define WB_STUB_END        0x0803C000u

/*
 * The stub's record, at WB_STUB_RECORD: what it did, for a timing build
 * and a debugger. Written as the stub goes; result is WB_UNPACK_OK once
 * the main image is in place, the refusal's code otherwise.
 */
#define WB_STUB_RECORD       0x0803BFE0u
#define WB_STUB_RECORD_MAGIC 0x42545357u	/* "WSTB" */
struct wb_stub_record {
	uint32_t magic;
	uint32_t result;	/* enum wb_unpack_rc, or WB_STUB_RUNNING */
	uint64_t entered;	/* the cycle counter at the stub's entry */
	uint64_t unpacked;	/* and once the main image was checked */
};
#define WB_STUB_RUNNING 0xFFFFFFFFu

_Static_assert(WB_STUB_RECORD + sizeof(struct wb_stub_record) <= WB_STUB_END,
	       "the stub's record lies in its region");
_Static_assert(WB_IMAGE_DDR_END == WB_PACK_START && WB_PACK_END - WB_PACK_START >= 0x76000u,
	       "the packed block holds any waffle-boot image (S-02.104)");

#endif /* WAFFLE_PACK_H */

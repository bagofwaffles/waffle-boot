/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DEVHOST_H
#define WAFFLE_DEVHOST_H

/*
 * Developer host requests (S-10c.110): register reads and writes, slave
 * peek and poke over the bridge, and a hand-over of the active-partition
 * OS with a chosen set of options. They are part of the standard image and
 * answer only while developer mode is on (S-15.172); with it off they are
 * refused and change nothing. A developer reaches them from held mode over
 * the USB serial port (tools/testos_mon.py). They are another way to boot
 * an OS and to recover a unit.
 *
 * The hand-over options are a bitmask (opxy_ho_flags). Zero is the normal
 * hand-over, exactly as a boot does it (contract XY-HANDOFF-2, S-05.023).
 * Each set bit moves one piece away from that, toward the environment of
 * S-05.031 or contract XY-HANDOFF-1's hand-over, so a developer can find
 * which piece an OS needs.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s09_bridge.h"

/* The hand-over options. 0 is the normal hand-over. */
#define OPXY_HO_MSI_ASFOUND   0x0001u	/* leave the slave's MSI controller as found */
#define OPXY_HO_SEC_ASFOUND   0x0002u	/* leave every SEC source as found */
#define OPXY_HO_NO_SPFP       0x0004u	/* leave SP and FP as the kernel left them */
#define OPXY_HO_NO_EVT        0x0008u	/* leave EVT2-EVT15 as they are */
#define OPXY_HO_DMA_KEEP      0x0010u	/* leave DMA20/DMA21 running (as found) */
#define OPXY_HO_CRC1_STOCK    0x0020u	/* set CRC1_CTL to S-05.031's value */
#define OPXY_HO_NO_USB_STOP   0x0040u	/* leave the USB controller running */
#define OPXY_HO_NO_TIMER_STOP 0x0080u	/* leave the timers running */
#define OPXY_HO_CACHE_OFF     0x0100u	/* the CPLBs and caches stay off at the jump */
#define OPXY_HO_NO_SLAVE_TEAR 0x0200u	/* the slave's teardown is interrupts off alone */
#define OPXY_HO_NO_SLAVE_GO   0x0400u	/* do not release the slave; it keeps serving */
#define OPXY_HO_TRACE         0x0800u	/* report each hand-over milestone over USB (keeps USB up) */
#define OPXY_HO_NO_PRELOAD    0x1000u	/* skip pre-load: no PMIC, 0x51 or logo redraw */
#define OPXY_HO_NO_OS_WAIT    0x2000u	/* no wait for the slave's OS (S-05.032) before the teardown */
#define OPXY_HO_RETIRED_4000  0x4000u	/* retired, ignored: the CPU copy into L1 code is the only one */
#define OPXY_HO_PA4_SPI2      0x8000u	/* the slave hands PA4 to its disabled SPI2 at its server stop */
#define OPXY_HO_STAY         0x10000u	/* the master watches the lines, then stays in held mode */
#define OPXY_HO_SLAVE_MARKS  0x20000u	/* the slave pulses PA4 at each hand-over step (OPXY_HO_MARK_*) */
#define OPXY_HO_MASTER_STOCK 0x40000u	/* the master's SPI0 set to the values below */
#define OPXY_HO_ALL          0x7BFFFu
/* The line window of OPXY_HO_TRACE, in ms, and that of OPXY_HO_STAY. */
#define OPXY_HO_PA6_MS        3000u
#define OPXY_HO_STAY_MS      10000u
/* S-05.031's CRC1_CTL. */
#define OPXY_HO_CRC1_CTL      0x000E0011u
/* OPXY_HO_MASTER_STOCK: the SPI0 registers it writes while SPI0 stays
 * disabled (SPI0_CTL 0). SPI2_CTL is S-05.016's 0x2 in every hand-over. */
#define OPXY_HO_SPI0_RXCTL    0x0001001Cu
#define OPXY_HO_SPI0_TXCTL    0x0001001Cu
#define OPXY_HO_SPI0_CLK      0x00000001u
#define OPXY_HO_SPI0_DLY      0x00000000u
#define OPXY_HO_SPI0_SLVSEL   0x00000808u

/* The hand-over options in force: 0 (the normal hand-over) but while a
 * developer hand-over request runs. Both CPUs read their own copy. */
extern volatile uint32_t opxy_ho_flags;
#define OPXY_HO_BIT(bit) ((opxy_ho_flags & (bit)) != 0u)

/*
 * A hand-over milestone, written on each CPU as the teardown and entry run
 * when OPXY_HO_TRACE is set (0 otherwise). The master reports its own and the
 * slave's (read over the bridge) over USB before it jumps, so the host sees
 * how far each got: the last report before the port dies is where it hung.
 */
extern volatile uint32_t opxy_ho_trace;
#define OPXY_HO_M_NONE      0u
#define OPXY_HO_M_TEARDOWN  1u	/* the teardown was entered */
#define OPXY_HO_M_PERIPH    2u	/* the peripheral rows are done (MSI on the slave) */
#define OPXY_HO_M_SEC       3u	/* the SEC row is done */
#define OPXY_HO_M_ENTRY     4u	/* the entry state is set; about to call the kernel */
/* The slave's jump delay in ms, poked by the master for OPXY_HO_TRACE runs. */
extern volatile uint32_t opxy_ho_slave_delay_ms;
/* The marker byte of a line report frame: OPXY_DEV_OK, this, the count, then
 * per change a 4-byte microsecond time from the release and a level byte:
 * bit 0 PA6 (the slave's ready line), bit 1 PC5 (the bridge attention). */
#define OPXY_HO_PA6_MARK    0x50u	/* 'P' */
/* OPXY_HO_SLAVE_MARKS: step k is a high pulse on the slave's PA4 of k x 2 ms,
 * then 2 ms low, PA4 being a GPIO driven low from stop_server on. */
#define OPXY_HO_MARK_TEARDOWN 1u	/* the teardown was entered */
#define OPXY_HO_MARK_MSI      2u	/* the MSI row is done */
#define OPXY_HO_MARK_SEC      3u	/* the SEC row is done */
#define OPXY_HO_MARK_ENTRY    4u	/* SP, FP, the vectors and the cache table are set */
#define OPXY_HO_MARK_DELAYED  5u	/* the jump delay is over; next the kernel call */
#define OPXY_HO_MARK_LOAD     6u	/* the load function's first call */
#define OPXY_HO_MARK_RETURNED 7u	/* the kernel returned (after its last load call) */
#define OPXY_HO_MARK_JUMP     8u	/* the last C before the jump to RCU0_SVECT0 */
#define OPXY_HO_MARK_MS       2u
/* A timing build's beacons: the slave's PB2 low this long at each of its
 * hand-over steps (opxy_handover.c), seen by the master on PC5. */
#define OPXY_HO_BEACON_US    20u
#define OPXY_HO_LINE_PA6    0x01u
#define OPXY_HO_LINE_PC5    0x02u

/*
 * The developer requests on the USB serial link (S-10c.110): an S-06 frame
 * of this id on the CDC link, served from held mode's loop while developer
 * mode is on. The opcodes share ping, rd and wr with the test OS's monitor
 * (tools/testos_mon.py), so one tool drives both.
 */
#define OPXY_DEV_REQ_ID 0x7Du
#define OPXY_DEV_MAGIC  0x44455631u	/* "DEV1" */
enum {
	OPXY_DEV_PING = 0, OPXY_DEV_READ = 1, OPXY_DEV_WRITE = 2,
	OPXY_DEV_SREAD = 20, OPXY_DEV_SWRITE = 21, OPXY_DEV_HANDOVER = 22,
	OPXY_DEV_FLAGS = 23, OPXY_DEV_L1READ = 24,
	OPXY_DEV_PUT = 25, OPXY_DEV_SHA = 26,
	OPXY_DEV_YTAGS = 27, OPXY_DEV_YMETA = 28,
	OPXY_DEV_SCREEN = 29, OPXY_DEV_KEY = 30, OPXY_DEV_FRAME = 31,
	OPXY_DEV_BENCH = 32, OPXY_DEV_CHECK = 33, OPXY_DEV_EXTRACT = 34,
	OPXY_DEV_LINK_RECORD = 35, OPXY_DEV_LINK_TEST = 36,
	OPXY_DEV_BOOT_START = 37, OPXY_DEV_CAL_START = 38,
	OPXY_DEV_QUAD_REPLY = 39, OPXY_DEV_STREAM = 40, OPXY_DEV_QUAD_DATA = 41,
	OPXY_DEV_CKPT = 42,
};
/* OPXY_DEV_PUT sub-ops: open (then the name), write (then the bytes), close. */
enum { OPXY_DEV_PUT_OPEN = 0, OPXY_DEV_PUT_DATA = 1, OPXY_DEV_PUT_CLOSE = 2 };
/* The reply's status byte, as other refused host requests carry one. */
#define OPXY_DEV_OK       0u
#define OPXY_DEV_REFUSED  1u	/* developer mode off, or the access was refused */
#define OPXY_DEV_BAD      2u	/* a malformed request */
/* A file request (PUT, SHA, YTAGS, YMETA) while the slave is not answering
 * (S-09.008): absent for the rest of the boot, so no request reaches its
 * volume, refused or not. */
#define OPXY_DEV_NO_SLAVE 3u

/*
 * OPXY_DEV_BOOT_START answers this boot's failed plain start, at any time:
 * opxy_start_report()'s OPXY_START_REPORT bytes after the status byte, laid
 * out in waffle/board/opxy_services.h; its first byte 0 when no start was
 * tried or none failed (held mode by its keys, its reason or the boot
 * prompt). What S-15.163 shows on the main screen and Device Info, and the
 * load's log line, come from the same account.
 */

/*
 * OPXY_DEV_STREAM answers how this start's slave boot stream went
 * (S-09.009), at any time: opxy_stream_report()'s OPXY_STREAM_REPORT bytes
 * after the status byte, laid out in waffle/board/opxy_boot.h. Only a build
 * with BOOT_LINK_FC=1 or 2 (2 is the default) has it; a BOOT_LINK_FC=0 build
 * answers OPXY_DEV_BAD.
 */

/*
 * OPXY_DEV_QUAD_DATA answers how this start's quad data replies went
 * (S-09.081, struct s09_qdata_rec), at any time, read only. After the
 * status byte, little-endian:
 *   state:1 (S09_QD_...) why:1 (S09_QW_...) rung:1 (the link's now)
 *   test_sent:2 test_whole:2 test_first:1 (S09_QX_..., 0x80 | the arm's
 *   value when refused) test_len:4 test_us:4, the quad reply test before
 *   the take-up
 *   replies:4 bytes:4 failed:4, the data sessions taken whole, their bytes,
 *   and those that failed
 *   fault: at:1 rc:4 status:4 left:4 stage:4, how the session that dropped
 *   them broke (S09_AT_...)
 */
#define OPXY_QDATA_REPLY 46u

/*
 * OPXY_DEV_CKPT (part:1) is the read-only checkpoint test (waffle/wire_ckpt.h)
 * of the slave's volume, run on the master over the bridge's sector reads:
 * a second yaffs2 device in the master's memory restores the checkpoint read
 * only, under a block device that refuses every write, so neither the slave's
 * mount nor the eMMC changes. Part 0 runs it, and answers, little-endian:
 *   outcome:1 (enum wire_ckpt_outcome) accepted:1 unmounted:1 cut:1 (bit 0
 *   the scanned tree, bit 1 the restored) alloc_used:1, then the words
 *   ckpt_blocks mount_reads mount_sectors mount_us writes_refused objs_scan
 *   objs_ckpt diffs ckpt_seq alloc_block alloc_page newer disagree test_us,
 *   then texts:1, the parts after it
 * Part 1 + i answers the last run's text i, ending in NUL: the reason a
 * checkpoint was not taken, then the differences kept, then the
 * disagreements kept. Refused while the volume is not mounted, refused, or
 * in use for a staging file or MTP (a cached write would differ from the
 * eMMC), and OPXY_DEV_NO_SLAVE without a slave.
 */
#define OPXY_CKPT_SUMMARY (5u + 14u * 4u + 1u)

/*
 * OPXY_DEV_CAL_START answers 0, then restarts as S-04's W10 (reason 70): the
 * next boot starts the OS with the calibration screen in the display frame
 * memory (S-04.012, S-05.016). One boot only: the boot consumes the reason.
 */

/* The bridge's developer requests, immediate: arg0 an address; 0x0D writes arg1. */
#define OPXY_DEV_PEEK_REQ 0x0Eu
#define OPXY_DEV_POKE_REQ 0x0Du

/*
 * OPXY_DEV_YTAGS and OPXY_DEV_YMETA read the yaffs2 region of the user area
 * raw, for comparing what each writer left on the media (S-12 section 2).
 * Read only, and only while the slave's volume is mounted on the user area.
 * YTAGS (first:4 stride:4 count:1) answers the 16-byte tags of count chunks
 * first, first + stride, ...; YMETA (chunk:4 off:2 len:1) answers bytes
 * off..off+len-1 of a chunk whose own tags make it an object header (the
 * first 0x200 bytes) or a block summary (the first 1,540). Every other chunk
 * is refused: no file data is ever returned.
 */
#define OPXY_YTAGS_BYTES 16u
#define OPXY_YTAGS_MAX   14u
#define OPXY_YMETA_MAX   232u

struct opxy_ydump_ops {
	void *ctx;
	/* User-area sectors, 0 on success (struct s03_emmc_ops' read). */
	int (*read)(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf);
};

uint8_t opxy_ydump_tags(const struct opxy_ydump_ops *io, uint32_t first,
			uint32_t stride, uint32_t count, uint8_t *out);
uint8_t opxy_ydump_meta(const struct opxy_ydump_ops *io, uint32_t chunk,
			uint32_t off, uint32_t len, uint8_t *out);

/*
 * OPXY_DEV_SCREEN (part:1) reports what held mode shows, so a host can drive
 * the unit without seeing it. Part 0: the parts' count (1 + the rows shown),
 * the menu's change counter (4), the screen (enum s15_screen), the flags
 * below, the MTP state (enum s15_mtp), the USB set presented (enum s10a_set,
 * 0xFF none), then the screen's name and its title, each ending in NUL.
 * Part 1 + i: the count, the counter, i, the row's style (enum s15_style),
 * the style's name ending in NUL, then the row's text (tabs and backquotes
 * as the console keeps them). A counter that differs between parts means
 * the screen changed meanwhile.
 */
#define OPXY_SCREEN_USB_UP  0x01u	/* the USB device stack runs */
#define OPXY_SCREEN_KEYTEST 0x02u	/* the key test is on */
#define OPXY_SCREEN_ARMED   0x04u	/* a hold to confirm is under way (S-15.015) */
#define OPXY_SCREEN_BUSY    0x08u	/* the developer menu is busy (S-15.092) */
#define OPXY_SCREEN_HELD    0x10u	/* a long operation holds the keys */
#define OPXY_SCREEN_SHIFT   0x20u	/* shift is held (S-15.012) */

/*
 * OPXY_DEV_KEY (key:1 action:1, up to OPXY_KEY_PAIRS times) queues key events
 * for the menus, which take them as they take the PSoC's (S-07.031): one per
 * pass of held mode's loop, none while a long operation holds the keys. The
 * reply is the events still waiting (1); no pairs only asks for that. A key
 * of S-07.030 (below S15_NKEYS) and an action below; a queue without room for
 * every event is refused and queues none.
 */
enum { OPXY_KEY_UP = 0, OPXY_KEY_DOWN = 1, OPXY_KEY_PRESS = 2 };
#define OPXY_KEY_PAIRS 32u
#define OPXY_KEY_QUEUE 64u

/* OPXY_DEV_FRAME (offset:4 length:1, 1 to OPXY_FRAME_CHUNK) reads the
 * display's frame buffer (S13C_FRAME_BYTES, S-13c.015's order). Refused
 * while the panel bus is down. */
#define OPXY_FRAME_CHUNK 240u

/*
 * OPXY_DEV_BENCH (stage:1 kib:2, 1 to OPXY_BENCH_KIB_MAX) times one stage of
 * the package check over kib KiB in 32 KiB pieces and answers stage:1
 * bytes:4 cycles:8 (core cycles) engine:1 crc16:1 crc32:1. READ copies from
 * the RAM disk's memory as a sector read does; CRC, SHA and AES are the
 * check's own functions over a buffer, on the packet engine when engine is
 * 1 and CRC on the CRC peripheral when crc16 is 1 (crc32: whether that
 * peripheral answered for CRC-32 too); FRAME draws the screen once;
 * SHA_SOFT and AES_SOFT are the software alone.
 */
enum {
	OPXY_BENCH_READ = 0, OPXY_BENCH_CRC = 1, OPXY_BENCH_SHA = 2,
	OPXY_BENCH_AES = 3, OPXY_BENCH_FRAME = 4, OPXY_BENCH_SHA_SOFT = 5,
	OPXY_BENCH_AES_SOFT = 6,
};
#define OPXY_BENCH_KIB_MAX 16384u
/*
 * OPXY_DEV_CHECK runs checks 1-15 (S-11.020) on the RAM disk's package as a
 * scan would, except that the zip is not written back and check 15 is not
 * run, so the package stays installable. It answers reason:1 size:4, then
 * the cycles of each stage (8 bytes each): the reads, CRC-16, SHA-256, AES,
 * the signature, USB service, and the whole check, then engine:1, whether
 * the packet engine served it to the end, crc16:1 crc32:1, whether the
 * CRC peripheral served each CRC to the end, and gap:4, the longest time in
 * microseconds USB went unserviced in the request. Refused when the disk holds no
 * package, and while a host-link transfer has the volume mounted.
 */
enum {
	OPXY_CHECK_READ = 0, OPXY_CHECK_CRC = 1, OPXY_CHECK_SHA = 2,
	OPXY_CHECK_AES = 3, OPXY_CHECK_SIG = 4, OPXY_CHECK_USB = 5,
	OPXY_CHECK_TOTAL = 6, OPXY_CHECK_STAGES = 7,
};

/*
 * OPXY_DEV_EXTRACT runs OPXY_DEV_CHECK's checks, then checks 16-18 (the
 * extraction, S-11.030-S-11.035) on the zip as the package holds it, an
 * encrypted zip decrypted as it is read (in software). Nothing is staged:
 * the staging writes are counted, not sent. Then it times two batches of
 * bridge requests that write nothing: OPXY_PROBE_N writes of S09_TUNNEL_MAX
 * bytes to descriptor OPXY_PROBE_FD, which the slave refuses (EBADF), and
 * OPXY_PROBE_N stats of /yaffs2. The reply, after the status byte,
 * little-endian:
 *   reason:1 flags:1
 *   counts, 4 bytes each (OPXY_EXTRACT_FILES ...)
 *   bytes extracted:8
 *   stage cycles, 8 bytes each (OPXY_EXTRACT_READ ...)
 *   per batch (OPXY_PROBE_WRITE ...): requests sent:4 cycles:8 errno:4, the
 *   errno of the batch's first request
 *   crc16:1 crc32:1, whether the CRC peripheral served each to the end
 *   rung:1 falls:1 kib:1, the link the probes ran on (S-09.075): its rung,
 *   the fall-backs of requests in the boot so far (OPXY_DEV_LINK_RECORD has
 *   them), and a file write's most KiB on the rung, the probes' write size
 *   gap:4, the longest time in microseconds USB went unserviced in the request
 *   tried:1, the rungs the last negotiation tried
 * The link is lowered to the plain mode after the probes (S-09.078).
 * A check that refuses the package answers its reason and zeros. Refused as
 * OPXY_DEV_CHECK is. All three timing requests are refused while a long
 * operation holds the menu's keys, a scan or an install runs, or the disk's
 * volume is mounted (a host-link transfer).
 */
enum {
	OPXY_EXTRACT_FILES = 0,		/* members staged as files */
	OPXY_EXTRACT_DIRS = 1,		/* directory entries */
	OPXY_EXTRACT_WRITES = 2,	/* staging writes */
	OPXY_EXTRACT_WRITE_REQS = 3,	/* the bridge writes they would send */
	OPXY_EXTRACT_MKDIRS = 4,	/* the mkdir requests the paths would send */
	OPXY_EXTRACT_STATS = 5,		/* the stats after a mkdir that fails */
	OPXY_EXTRACT_COUNTS = 6,
};
enum {
	OPXY_EXTRACT_READ = 0,		/* the zip's reads from the disk */
	OPXY_EXTRACT_DECRYPT = 1,	/* this request's own decryption */
	OPXY_EXTRACT_INFLATE = 2,	/* minizip and zlib, no CRC-32 */
	OPXY_EXTRACT_CRC = 3,		/* the extraction's CRC-32 of each member */
	OPXY_EXTRACT_MEMBER = 4,	/* the member listing, its reads left out */
	OPXY_EXTRACT_USB = 5,		/* USB service */
	OPXY_EXTRACT_TOTAL = 6,		/* the whole extraction */
	OPXY_EXTRACT_CHECK = 7,		/* checks 1-15 before it */
	OPXY_EXTRACT_STAGES = 8,
};
enum {
	OPXY_PROBE_WRITE = 0, OPXY_PROBE_STAT = 1, OPXY_PROBE_BATCHES = 2,
};
#define OPXY_PROBE_N       64u
#define OPXY_PROBE_FD      0x7FFFFFFFu
/* The flags byte. */
#define OPXY_EXTRACT_NO_SLAVE  0x01u	/* the slave is absent: no batch was sent */
#define OPXY_EXTRACT_WROTE     0x02u	/* a write succeeded: the batches stopped, zeros */
#define OPXY_EXTRACT_LOST      0x04u	/* the slave stopped answering: absent until a restart */
#define OPXY_EXTRACT_DIRS_FULL 0x08u	/* too many directories to tell apart: stats too high */
#define OPXY_EXTRACT_UNSETTLED 0x10u	/* the slave could not be told the link is plain: restart */
/* 141 bytes, within an S-06 payload */
#define OPXY_EXTRACT_REPLY (3u + 4u * OPXY_EXTRACT_COUNTS + 8u + \
			    8u * OPXY_EXTRACT_STAGES + 16u * OPXY_PROBE_BATCHES + 2u + 3u + 4u + 1u)

/*
 * OPXY_DEV_LINK_RECORD (part:1) answers part of the bridge link's record for
 * the boot (struct s09_record, S-09.075, S-09.079), at any time: part:1
 * parts:1 count:1, then, little-endian,
 *   part 0: falls:4 negotiations:4 tried:1 kept:1 rung:1 fell:4 settled:1
 *     bench:4: the fall-backs of requests, the negotiations, the rungs the
 *     last one tried and the rung it kept, the rung now, every fall-back the
 *     link made, a negotiation's and the
 *     link test's included, 0 when the slave may still be in a fall-back's
 *     state (S-09.074): restart the unit, and the last link test's faults;
 *   parts 1-4: count (up to 2) rungs the last negotiation tried, in order,
 *     72 bytes each: rung:1 result:1 (S09_RR_...) pattern:1 length:4 fault
 *     big_us:4 x 3 (the gate's three request sessions, 0 when not timed)
 *     plain_us:4 status;
 *   parts 5-10: count (up to 3) fall-backs of requests, oldest first, 73 bytes
 *     each: from:1 to:1 (0xFF: the slave absent) cmd:1 tries:1 (0: its first
 *     send) length:4 fault more status;
 *   parts 11-18: count (up to 2) of the last link test's faults the record
 *     keeps (its first S09_REC_BENCH_FIRST and its last ones, S09_REC_BENCH
 *     in all), in its order, 114 bytes each: no:2 (the run's fault number,
 *     from 0) kind:1 (S09_BF_...) rung:1 result:1 (an entry's S09_RR_..., 0xFF: lost before its
 *     trial) length:4 request:2 (the cell's, from 0) to:1 (where the link
 *     came back: the rung, 0 plain, 0xFF the slave absent) fall_us:4 (the
 *     fall-back's echoes, 0 none) fault more echo more status, echo the last
 *     recovery echo's failure when none was answered;
 * where fault is at:1 (S09_AT_...) and the driver's rc:4 status:4 left:4
 * stage:4, more the driver's second four words (zeros where it has none:
 * the DMA channel's DMA_STAT and DMA_CFG, then SPI_TXCTL and SPI_TWC after a
 * send, SPI_RXCTL and SPI_RWC after a receive),
 * and status the slave's 0x1B answer: told:1 fell:4 cause:1 cmd:1 mode:1
 * length:4 got:4 rc:4 status:4 left:4 stage:4.
 */
#define OPXY_RECORD_PARTS       19u
#define OPXY_RECORD_RUNG_PARTS  4u
#define OPXY_RECORD_RUNGS_PER   2u
#define OPXY_RECORD_FALL_PARTS  6u
#define OPXY_RECORD_FALLS_PER   3u
#define OPXY_RECORD_BENCH_PER   2u

/*
 * OPXY_DEV_LINK_TEST, the link bench (s09_master_link_bench()), from held
 * mode with no package: nothing is written to the eMMC and nothing but
 * echoes, 0x19, 0x1A and 0x1B is sent. Sub-ops:
 *   run (n:1): n link tests (1 to 255, OPXY_LINK_TEST_N when n is left out;
 *     0 is refused) of each length of s09_bench_len in each cell, in one
 *     pass (S-09.075): on each rung from 1 to the top, then in the plain mode
 *     at the lengths plain requests have, every session by DMA as every
 *     request's (S-13b.083). A rung that cannot be entered is left for the
 *     next; a length's requests stop after S09_BENCH_FAULTS
 *     faults (the rest not sent); the run stops when OPXY_LINK_TEST_LIMIT_S
 *     has passed or the slave stops answering. Each fault goes to the
 *     record (OPXY_DEV_LINK_RECORD parts 11-18). Answers parts:1 n:1 flags:1,
 *     entry:5 entries:5 (each rung's last entry, S09_RR_KEPT 0 when entered, S09_RR_NONE 0xFF when
 *     not tried, else why not, the plain rung's unused; and the times it
 *     was entered, again after each fault), cycles:8 (the run's core cycles),
 *     falls:2 (the run's own fall-backs, which the boot's count keeps too),
 *     limit:2 (OPXY_LINK_TEST_LIMIT_S), where it ended when cut or lost:
 *     stopped:1 (0 it ran to its end) rung:1 length:4 request:2 (from 0),
 *     and faults:4 (the run's, as the record counts them). Refused while
 *     a long operation holds the keys, a scan or an install runs, the disk's
 *     volume is mounted, or the slave is absent. It ends in the plain mode
 *     with the slave told so, the boot's struck rungs, gate verdicts and
 *     negotiation record as they were; an install after it negotiates as it
 *     would have.
 *   part (k:1): the last run's table, OPXY_LINK_TEST_ROWS rows of 33 bytes,
 *     7 a part: part:1 parts:1 count:1, then each row rung:1 length:4 sent:2
 *     passed:2 us:4 (the passed requests' time in all, each from its request
 *     session's start to its reply's end) and faults:2 for each S09_AT_...
 *     (S09_AT_COUNT of them). Rows: rungs 1 to 4, each length; then the
 *     plain rung's lengths.
 */
#define OPXY_LINK_TEST_REPLY 38u	/* a run's answer, its status byte included */
enum { OPXY_LINK_TEST_RUN = 0, OPXY_LINK_TEST_PART = 1 };
#define OPXY_LINK_TEST_N     16u
#define OPXY_LINK_TEST_ROWS  35u
#define OPXY_LINK_TEST_PER   7u
#define OPXY_LINK_TEST_PARTS 5u
#define OPXY_LINK_TEST_LOST  0x01u	/* flags: the slave stopped answering */
#define OPXY_LINK_TEST_CUT   0x02u	/* ... the time bound ended the run */
#define OPXY_LINK_TEST_UNSETTLED 0x04u	/* ... the slave may still be in a
					 * fall-back's state: restart */
/*
 * The run's bound. The default run takes about 6 s on a working link; with
 * the sessions failing on every rung each of its 28 lengths costs two
 * faults of about a second (a failed session of up to 0.5 s, S-09.013's
 * 0.55 s quiet wait, the echoes, 0x1B and the rung entered again), about
 * 60 s more: 90 s leaves that room.
 */
#define OPXY_LINK_TEST_LIMIT_S 90u

/*
 * OPXY_DEV_QUAD_REPLY (cand:1 ren:1 clock:1 len:4 n:2), the quad reply test
 * (S-09.080, S-13b.084, s09_master_quad_test()), from held mode: one cell, n
 * test replies (1 to OPXY_QUAD_N_MAX, OPXY_QUAD_N when n is left out or 0)
 * of len bytes (12 to 0x10000, whole words) under candidate cand (S09_QT_...),
 * with REN when ren is 1, the master at clock (S09_QT_FAST, S09_QT_FMODE). It
 * writes nothing to storage and sends nothing but 0x1C, and S-09.013 rule
 * 4's echoes, polls and 0x1B when a session fails; it ends with the link
 * plain and the slave answering, the boot's struck rungs and gate verdicts
 * as they were. Refused as OPXY_DEV_LINK_TEST is. The reply after the status
 * byte, little-endian:
 *   cand:1 ren:1 clock:1 len:4 n:2, as asked
 *   flags:1 (OPXY_QUAD_...) why:1 (refused: the arm's value, S09_QT_...)
 *   sent:2 whole:2 flag:2 missing:2 last_wrong:2 word_wrong:2 broke:2
 *   no_record:2, the trials (struct s09_qt_cell)
 *   ends:2 x S09_QE_COUNT, the slave's records by how each reply ended
 *   dmin:4 dmax:4, the flag's delay after the last feed in the slave's
 *   cycles (0xFFFFFFFF: none), lines:1, the data lines of every wrong bit
 *   m_or:4 m_and:4 s_or:4, SPI_STAT at the end of each reply, the master's
 *   or-ed and and-ed, and the slave's or-ed over the reply
 *   cycles:8, the cell's core cycles
 *   first:1 (S09_QX_...) first_no:2 first_word:4 (0xFFFFFFFF none), the
 *   first failed trial, then the master's four words for it and the
 *   slave's record of it (S09_QT_REC_WORDS words, zeros when none came)
 */
#define OPXY_QUAD_N 16u
#define OPXY_QUAD_N_MAX 4096u
#define OPXY_QUAD_LIMIT_S 120u
#define OPXY_QUAD_LOST 0x01u		/* flags: the slave stopped answering */
#define OPXY_QUAD_CUT 0x02u		/* ... the bound ended the cell */
#define OPXY_QUAD_UNSETTLED 0x04u	/* ... the slave not answering after it: restart */
#define OPXY_QUAD_REFUSED 0x08u		/* ... refused: nothing was sent but the arm */
#define OPXY_QUAD_REPLY (1u + 9u + 2u + 16u + 2u * S09_QE_COUNT + 9u + 12u + 8u + \
			 7u + 16u + S09_QT_REC_LEN)

/*
 * OPXY_DEV_EXTRACT's reader of the zip, a wire_source read at package
 * offsets: read's bytes as they are or, encrypted, decrypted as they are
 * read (AES-256-CBC in software, iv the chain value at the zip's start).
 * False outside the zip's len bytes from at.
 */
struct opxy_zip_reader {
	bool (*read)(void *ctx, uint32_t off, uint8_t *buf, size_t n);
	void *ctx;
	bool encrypted;
	uint8_t key[32];
	uint8_t iv[16];
	uint32_t at, len;
};
bool opxy_zip_read(void *ctx, uint32_t off, uint8_t *buf, size_t n);

struct opxy_master;
/* The timing requests (src/board/opxy_devtime.c): the reply's length, its
 * status byte at out[0] left OPXY_DEV_OK or set. */
size_t opxy_dev_bench(struct opxy_master *m, const uint8_t *req, size_t n,
		      uint8_t *out);
size_t opxy_dev_check(struct opxy_master *m, uint8_t *out);
size_t opxy_dev_extract(struct opxy_master *m, uint8_t *out);
size_t opxy_dev_link_test(struct opxy_master *m, const uint8_t *req, size_t n,
			  uint8_t *out);
size_t opxy_dev_quad_reply(struct opxy_master *m, const uint8_t *req, size_t n,
			   uint8_t *out);
/* OPXY_DEV_CKPT (src/board/opxy_dev_ckpt.c). */
size_t opxy_dev_ckpt(struct opxy_master *m, const uint8_t *req, size_t n, uint8_t *out);

/* From held mode's loop, once a pass: one queued key event to the menus;
 * with developer mode off, the hand-over options dropped. */
void opxy_dev_keys(struct opxy_master *m);

/* Registers on either CPU, and opxy_ho_flags: the OTP controller never,
 * the SPI flash controller not for a write. */
bool opxy_dev_mem_ok(uint32_t addr, bool write);

struct opxy_master;
struct s06_frame;
/* The held-mode USB handler (src/board/opxy_devhost.c); true when it was a
 * developer-request frame (served or refused). */
bool opxy_dev_host(struct opxy_master *m, const struct s06_frame *f);
#endif /* WAFFLE_DEVHOST_H */

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The frozen front's one interface with the rest of the image: its fixed
 * memory, the image descriptor the image tool writes (tools/mkldr.py) and
 * the hand-off record the front leaves for the full bootloader. The front
 * jumps to the descriptor's main entry with the record written; nothing
 * else of the front is reachable from the rest, and the front calls nothing
 * in it. S-01.023 states the contract; CONTRIBUTING.md says when this may
 * change.
 */
#ifndef FRONT_HANDOFF_H
#define FRONT_HANDOFF_H

#include <stdint.h>

/* The front's fixed memory (src/front/front.ld). */
#define FR_L1_CODE_START  0x11A04000u
#define FR_L1_CODE_END    0x11A0C000u
#define FR_L1_DATA_START  0x11802000u
#define FR_L1_DATA_END    0x11804000u
#define FR_L2_START       0x08002000u
#define FR_L2_END         0x08030000u
#define FR_SHARED_START   0x08030000u
#define FR_SHARED_END     0x08031000u
#define FR_DESC_ADDR      0x08030000u	/* loaded: the image tool's block */
#define FR_HANDOFF_ADDR   0x08030800u	/* written by the front */
#define FR_STAGING_START  0x88400000u	/* recovery's received image */
#define FR_STAGING_END    0x88478000u
#define FR_FRAME_START    0x8FE00000u	/* the front's frame, in bus order */
#define FR_FRAME_END      0x8FE34800u

/* --- The image descriptor (tools/mkldr.py writes it; little-endian) --- */

#define FR_DESC_MAGIC     0x4D494257u	/* "WBIM" */
#define FR_DESC_VERSION   1u
#define FR_DESC_REGIONS   24u
#define FR_DESC_HEAD      0x40u
#define FR_DESC_MAX       (FR_DESC_HEAD + 8u * FR_DESC_REGIONS + 4u)

#define FR_DESC_O_MAGIC       0x00u	/* u32 */
#define FR_DESC_O_VERSION     0x04u	/* u16 */
#define FR_DESC_O_SIZE        0x06u	/* u16: bytes, the final CRC included */
#define FR_DESC_O_MAIN_ENTRY  0x08u	/* u32 */
#define FR_DESC_O_NREGIONS    0x0Cu	/* u32 */
#define FR_DESC_O_MAIN_CRC    0x10u	/* u32: CRC-32 of the regions in order */
#define FR_DESC_O_FRONT_CRC   0x14u	/* u32: the front's block payloads */
#define FR_DESC_O_BL_VERSION  0x18u	/* u16[4]: major, minor, patch, build */
#define FR_DESC_O_FRONT_PIN   0x20u	/* u8[32]: the front's pin (SHA-256) */
#define FR_DESC_O_REGIONS     FR_DESC_HEAD	/* {u32 addr, u32 len}[n] */
/* Then a u32: the CRC-32 of every byte before it. */

/* --- The start log (S-02.095): two bits per start in a slot's last sector.
 * The front clears an entry's started bit before the jump; the main image
 * clears its finished bit once it is alive. --- */

#define FR_LOG_OFF(k)      (0x87000u + 0x78000u * (k))	/* slot k's last sector */
#define FR_LOG_ENTRIES     16384u		/* four to a byte, 4096 bytes */
#define FR_LOG_IMAGE_MAX   0x77000u		/* an image that leaves the sector free */
#define FR_LOG_BYTE(e)     ((e) >> 2)		/* entry e's byte in the sector */
#define FR_LOG_STARTED(e)  (1u << (2u * ((e) & 3u)))
#define FR_LOG_FINISHED(e) (2u << (2u * ((e) & 3u)))

/* --- The boot logo: a picture the main image stores in a slot and the
 * front draws in place of its own (tools/wblogo.py is the reference).
 * Little-endian. A header at FR_LOGO_SLOT_OFF of the slot, the sector below
 * the settings sector; its data, the palette and then the packed lines,
 * ends where the header's sector begins and starts no lower than the end of
 * the slot's image (its record's length rounded up to a sector). An erased
 * header means no logo.
 *
 * The picture is width x height pixels, centered on the screen
 * ((480 - width) / 2, (222 - height) / 2) over the background color. Its
 * pixels go in the panel's order: line j is the picture's column
 * width - 1 - j, top to bottom. A line is a sequence of runs, none
 * crossing into the next line: a byte c, n = (c & 0x7F) + 1 pixels; for
 * c & 0x80 one byte, the palette index of all n; otherwise the n indices,
 * bits each, packed from the high bit of each byte, ceil(n * bits / 8)
 * bytes. Colors are RGB565 values. --- */

#define FR_LOGO_SLOT_OFF   0x75000u	/* the header's sector, in each slot */
#define FR_LOGO_OFF(k)     (0x85000u + 0x78000u * (k))
#define FR_LOGO_MAGIC      0x474C4257u	/* "WBLG" */
#define FR_LOGO_VERSION    1u
#define FR_LOGO_HEAD       0x24u	/* the header's bytes */
#define FR_LOGO_W_MAX      480u		/* the largest picture */
#define FR_LOGO_H_MAX      222u
#define FR_LOGO_COLORS     256u
/* The most data any valid picture has: a run for every pixel. */
#define FR_LOGO_DATA_MAX   (2u * FR_LOGO_COLORS + 2u * FR_LOGO_W_MAX * FR_LOGO_H_MAX)

#define FR_LOGO_O_MAGIC    0x00u	/* u32 */
#define FR_LOGO_O_VERSION  0x04u	/* u16 */
#define FR_LOGO_O_SIZE     0x06u	/* u16: FR_LOGO_HEAD */
#define FR_LOGO_O_WIDTH    0x08u	/* u16: 1 to FR_LOGO_W_MAX */
#define FR_LOGO_O_HEIGHT   0x0Au	/* u16: 1 to FR_LOGO_H_MAX */
#define FR_LOGO_O_BITS     0x0Cu	/* u8: 1, 2, 4 or 8, then a u8 0 */
#define FR_LOGO_O_COLORS   0x0Eu	/* u16: 2 to 2^bits */
#define FR_LOGO_O_BG       0x10u	/* u16: the background, then a u16 0 */
#define FR_LOGO_O_LEN      0x14u	/* u32: the data's bytes, 2 x colors + lines */
#define FR_LOGO_O_CRC      0x18u	/* u32: CRC-32 of the data */
#define FR_LOGO_O_IMAGE    0x1Cu	/* u32: the slot record's CRC-32 when saved */
#define FR_LOGO_O_HCRC     0x20u	/* u32: CRC-32 of the header's bytes before it */

/* --- The hand-off record (written by the front, read by main) --- */

#define FR_HO_MAGIC       0x4F484257u	/* "WBHO" */
#define FR_HO_VERSION     3u
#define FR_HO_SIZE        160u	/* version 3's bytes, the CRC included */
#define FR_HO_KEYS_MAX    16u
#define FR_HO_BATTERY_MAX 16u
#define FR_HO_STAMPS      8u

/* flags */
#define FR_HO_GREETED       0x0001u	/* the PSoC greeted the link (S-07.020) */
#define FR_HO_GREET_WAITED  0x0002u	/* S-07.021's exit packet and wait ran */
#define FR_HO_KEYS_REPLIED  0x0004u	/* a 0x41 reply came (S-07.032) */
#define FR_HO_PANEL_UP      0x0008u	/* the panel started, the logo frame sent */
#define FR_HO_PANEL_LIT     0x0010u	/* 0x51 {3C FF 02} sent after it */
#define FR_HO_CHARGE_WAITED 0x0020u	/* the charge wait ran (S-07.054) */
#define FR_HO_POWER_KNOWN   0x0040u	/* power_state holds a reported state */
#define FR_HO_SUBSCRIBED    0x0080u	/* 0x43 {01 01} sent (S-07.025) */
#define FR_HO_CHORD_SHOWN   0x0100u	/* never set: kept for the layout */
#define FR_HO_START_LOGGED  0x0200u	/* log_slot and log_entry name this start's
					 * entry in the start log (S-02.095) */
#define FR_HO_RUNNING       0x0400u	/* version 2: run_slot is the running slot,
					 * established (S-01.002, S-02.054) */
#define FR_HO_TRIAL_MARKED  0x0800u	/* version 2: a trial start the front marked */
#define FR_HO_ROLE_BY_STRAP 0x1000u	/* version 3: the boot mode was not 1 and PA2
					 * read 1, so this CPU went on as a main CPU
					 * (fr_decide()) */
#define FR_HO_LOGO          0x2000u	/* the logo frame showed the stored logo of
					 * slot 0, or with FR_HO_LOGO_SLOT1 slot 1 */
#define FR_HO_LOGO_SLOT1    0x4000u

/* device (S-18.005): the device the front decided */
#define FR_HO_DEV_OPXY      0u
#define FR_HO_DEV_OP1F      1u
/* device_how: how it was decided */
#define FR_HO_HOW_TABLE     0u	/* by the first stage's table (S-18.003) */
#define FR_HO_HOW_SECOND    1u	/* the second CPU (S-18.002) */
#define FR_HO_HOW_UNKNOWN   2u	/* not known, the default taken (S-18.004) */

/* The core clock (S-13a.001): the time base of every front wait, and the
 * stamps' unit, FR_CCLK_MHZ cycles a microsecond. */
#define FR_CCLK_MHZ       400u

/* stamps: the cycle counter at each point; 0 when not reached */
enum fr_stamp {
	FR_STAMP_INIT_ENTRY,	/* the INIT stage entered (clocks not yet set) */
	FR_STAMP_INIT_CLOCKS,	/* the PLL set */
	FR_STAMP_INIT_DDR,	/* the DDR controller up */
	FR_STAMP_ENTRY,		/* the front's entry after the stream */
	FR_STAMP_GREETED,	/* the greeting done (or given up) */
	FR_STAMP_LOGO,		/* the logo lit (0x51 {3C FF 02} queued) */
	FR_STAMP_KEYS,		/* the key status read */
	FR_STAMP_HANDOFF	/* the image checked, just before the jump */
};

struct fr_handoff {
	uint32_t magic;
	uint16_t version;
	uint16_t size;			/* sizeof(struct fr_handoff) */
	uint32_t flags;
	uint16_t strap;			/* S-13b.001's sample */
	uint8_t role_master;		/* 1: the master */
	uint8_t psoc_seq;		/* the PSoC link's next seq (S-06.020) */
	uint32_t dma9_xmod, dma9_ymod;	/* as found at entry (S-01.006) */
	uint32_t init_sp, init_dctl, init_dflt;	/* S-01.007, S-01.021 */
	uint32_t panel_reset_ms;	/* S-13c.021's reset time */
	uint8_t power_state;		/* the last 0x31/0x32 state */
	uint8_t key_len;		/* bytes in keys[] */
	uint8_t battery_len;		/* bytes in battery[] */
	uint8_t chord;			/* 0: kept for the layout (S-01.023) */
	uint8_t chord_result;		/* 0, as chord */
	uint8_t log_slot;		/* with FR_HO_START_LOGGED: the log's slot */
	uint16_t log_entry;		/* and this start's entry in it */
	uint8_t keys[FR_HO_KEYS_MAX];	/* the 0x41 reply's payload */
	uint8_t battery[FR_HO_BATTERY_MAX];	/* the last 0x37 payload */
	uint64_t stamps[FR_HO_STAMPS];	/* enum fr_stamp */
	uint8_t run_slot;		/* with FR_HO_RUNNING: 0 or 1 */
	uint8_t te_misses;		/* frames sent without a TE edge, saturating */
	uint16_t link_overruns;		/* PSoC UART reads with the overrun flag set */
	uint32_t link_gap_max_us;	/* the longest time between two of them */
	/* version 3 (S-18.005) */
	uint8_t device;			/* FR_HO_DEV_* */
	uint8_t device_how;		/* FR_HO_HOW_* */
	uint16_t zero;			/* 0 */
	uint32_t crc32;			/* CRC-32 of the bytes before it */
};

/* S-18.005's 160 bytes, the same on the host and the target. */
_Static_assert(sizeof(struct fr_handoff) == FR_HO_SIZE, "S-18.005: the hand-off record is 160 bytes");
_Static_assert(__builtin_offsetof(struct fr_handoff, device) == 152u &&
	       __builtin_offsetof(struct fr_handoff, device_how) == 153u &&
	       __builtin_offsetof(struct fr_handoff, zero) == 154u &&
	       __builtin_offsetof(struct fr_handoff, crc32) == 156u,
	       "S-18.005: the hand-off record's version 3 fields");

#endif /* FRONT_HANDOFF_H */

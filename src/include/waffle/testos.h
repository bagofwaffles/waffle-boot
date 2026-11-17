/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_TESTOS_H
#define WAFFLE_TESTOS_H

/*
 * The test OS (wiki/Test-OS.md): the bootloader's held mode built as an
 * OS image, loaded by another bootloader's kernel call (S-05.008 to
 * S-05.014) on both CPUs and entered at S-05.014's core state. It is
 * `make test-os`, compiled with -DOPXY_TEST_OS=1; the normal build never
 * defines it, and these macros then expand to nothing or to their normal
 * operand, so the normal image is byte for byte the same.
 *
 * The test OS's own code is in src/testos/: its board files, its linker
 * script, and the SPI driver's test-OS parts, which spi.c includes in this
 * build only. The bootloader's files carry the rest, as these macros or as
 * blocks under OPXY_TEST_OS.
 *
 * The macros keep the test-OS variants on the same source lines as the
 * code they replace: S-14's tags carry __LINE__, so an added line in a
 * file that logs with S14_TAG would change the normal image.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(OPXY_TEST_OS)
#define OPXY_TESTOS(x)              x
#define OPXY_TESTOS_OR(testos, normal) (testos)
#define OPXY_TESTOS_UNUSED          __attribute__((unused))
#else
#define OPXY_TESTOS(x)
#define OPXY_TESTOS_OR(testos, normal) (normal)
#define OPXY_TESTOS_UNUSED
#endif

/* The check suite, in the installer's variant (`make installer`,
 * wiki/Installer-Reference.md), on the same terms: nothing in any other build. */
#if defined(OPXY_TESTOS_CHECK)
#define OPXY_TESTOS_CHECK_ONLY(x)          x
#define OPXY_TESTOS_CHECK_OR(check, other) (check)
#else
#define OPXY_TESTOS_CHECK_ONLY(x)
#define OPXY_TESTOS_CHECK_OR(check, other) (other)
#endif

struct opxy_master;
struct s09_bus_ops;
struct s06_frame;
struct s07_psoc;
struct s14_text;
struct wb_display;

/*
 * The panel byte path this build drives the panel with (Makefile,
 * TESTOS_PANEL; wiki/Test-OS.md): "smc" (src/drivers/bf70x/lcd.c, the
 * normal driver) or "gpio" (lcd_gpio.c, every byte bit-banged).
 */
#if defined(OPXY_TESTOS_MONITOR)
#define OPXY_TESTOS_PANEL_NAME "smc, monitor"
#elif defined(OPXY_TESTOS_INSTALLER)
#define OPXY_TESTOS_PANEL_NAME "smc, installer"
#elif defined(OPXY_TESTOS_PANEL_GPIO)
#define OPXY_TESTOS_PANEL_NAME "gpio"
#else
#define OPXY_TESTOS_PANEL_NAME "smc"
#endif

/*
 * The registers S-13a expects at OS entry (observation point OSENTRY), one
 * row per field, generated from
 * tests/oracle/S-13a-core-bring-up/vectors/register_states.tsv by
 * tools/testos_regs.py. The test OS reads each where the loading
 * bootloader left it and logs the fields that differ; it writes none.
 */
#define OPXY_TESTOS_RULE_EQ  0u	/* the field equals lo */
#define OPXY_TESTOS_RULE_NE  1u	/* differs from lo */
#define OPXY_TESTOS_RULE_GE  2u	/* at least lo */
#define OPXY_TESTOS_RULE_LE  3u	/* at most lo */
#define OPXY_TESTOS_RULE_IN  4u	/* lo <= field <= hi */
#define OPXY_TESTOS_RULE_ANY 5u	/* not checked, read and logged */
struct opxy_testos_reg {
	uint32_t addr;
	uint8_t msb, lsb;
	uint8_t rule;
	uint32_t lo, hi;
	const char *reg, *field, *req;
};
extern const struct opxy_testos_reg opxy_testos_regs[];
extern const unsigned opxy_testos_regs_n;

/*
 * Written by src/testos/opxy_testos_entry.S before anything else of the
 * image runs, once the CPLBs are off: SP, IPEND, IMASK and RETS at the
 * first instruction (S-05.014), in [0] to [3]. Then by the entry paint
 * (opxy_testos_entry_paint(), before wb_start_c()): [4] what the red paint
 * did (src/testos/opxy_testos_paint.c's PAINT_ values), [5] the role strap
 * sample of S-13b.001, PA2 alone (the full sample is opxy_main()'s). Loaded
 * data (.l1.data.testos, placed with .data in the DDR image), which the
 * stream loads and wb_start_c() does not clear.
 */
extern uint32_t opxy_testos_entry[8];

/*
 * The milestone paints (src/testos/opxy_testos_paint.c, which lists
 * each): the whole panel in one color at each milestone of
 * the master's start, through the build's panel byte path, held for
 * OPXY_TESTOS_PAINT_HOLD_US so a person sees it. Nothing on the slave.
 */
enum opxy_testos_paint {
	OPXY_PAINT_RED,		/* the entry shim handed over to C */
	OPXY_PAINT_ORANGE,	/* wb_start_c() and the read-out done; master */
	OPXY_PAINT_YELLOW,	/* our panel reset and initialization done */
	OPXY_PAINT_GREEN,	/* the S-07 start-up returned */
	OPXY_PAINT_CYAN,	/* the panel's start returned before the early logo frame */
	OPXY_PAINT_GRAY,	/* the early logo frame's bytes went out */
	OPXY_PAINT_VIOLET,	/* 0x51 {3C FF 02} sent after the frame */
	OPXY_PAINT_BLUE,	/* the slave found, the bridge up */
	OPXY_PAINT_MAGENTA,	/* a fatal */
	OPXY_PAINT_OLIVE,	/* the slave's start entered, after the keys */
	OPXY_PAINT_PINK,	/* the bridge's ready line (PA6) first seen low */
	OPXY_PAINT_TEAL,	/* the bridge's first session (the init) answered */
	OPXY_PAINT_BROWN,	/* the slave given up as absent */
	OPXY_PAINT_LIME,	/* held mode's start entered */
	OPXY_PAINT_NAVY,	/* held mode's services started, first screen next */
	OPXY_PAINT_COUNT
};
#define OPXY_TESTOS_PAINT_HOLD_US 300000u
/* From the entry shim, on the test OS's own stack with IVG15 alone enabled
 * and the CPLBs off, before wb_start_c(): the strap, then red on the
 * master when the display record says the panel is initialized. Touches
 * no static data of the image (.bss is not yet cleared). */
void opxy_testos_entry_paint(void);
/* A later milestone: through the display driver's bus once it is up, else
 * as the entry paint. Logs at once when the log has its sink, else when
 * opxy_testos_paint_log() flushes. */
void opxy_testos_paint(enum opxy_testos_paint which);
void opxy_testos_paint_log(struct opxy_master *m);
/* src/board/opxy_display.c: the display once its bus is up, else NULL. */
struct wb_display *opxy_testos_display(void);
/* src/board/opxy_display.c: the early logo of S-05.004 in the test OS, with
 * a paint after each sub-step (cyan, gray); true when the frame went out. */
bool opxy_testos_early_logo(void);

/* A line to the S-14 log (level 3, tag testos) and to testos.txt. */
void opxy_testos_emit(struct opxy_master *m, const struct s14_text *t);
/* s07_poll() hands every frame taken from the PSoC link here, so a wait
 * that times out can report the last one received. */
void opxy_testos_frame_seen(const struct s06_frame *f);
/* An S-07 wait the test OS cut short, as it bounds each one:
 * which, how long it ran, and the link's state and last frame. */
void opxy_testos_wait_timeout(const struct s07_psoc *p, const char *what,
			      uint32_t waited_us);
/* A display wait that ran out (S-13c.016's frame end, S-13c.017's TE):
 * logged the first few times, counted always; the frame goes on. */
void opxy_testos_display_timeout(const char *what, uint32_t waited_us);
/* S-07.051 in the test OS: every power state the PSoC reports is kept;
 * a power-off logs the pair and the states seen, then runs as usual.
 * True when s starts no power-off. */
bool opxy_testos_power_state(unsigned prev, unsigned s);
/* s07_startup()'s result on its way back: the green paint, 0x51 {3C FF 02}
 * so the paint is lit, and a line; returns rc unchanged. */
int opxy_testos_started(int rc);

/*
 * The slave's bring-up in the test OS, one step at a time and each
 * logged. The server comes up first and answers with the eMMC down; a
 * request the normal image does not have, immediate, arg0 a row of the
 * table below, arg1 nonzero to start the eMMC's bring-up once the reply
 * has gone out, with the ready line high until it ends.
 */
#define OPXY_TESTOS_ROW_REQ 0x0Fu
/* The monitor's reach into the slave: immediate, arg0 an address (0x0E reads
 * a word, 0x0D writes arg1), inside the limits of opxy_testos_mem_ok(). */
#define OPXY_TESTOS_PEEK_REQ 0x0Eu
#define OPXY_TESTOS_POKE_REQ 0x0Du
/* How long the master waits for the ready line, for the eMMC's bring-up,
 * for the 0x08 mount (the normal image's S-09.060 T_EXEC is 60 s - 120 s). */
#define OPXY_TESTOS_READY_CAP_US 5000000u
#define OPXY_TESTOS_EMMC_CAP_US  8000000u
#define OPXY_TESTOS_MOUNT_CAP_US 30000000u
enum opxy_testos_step {
	OPXY_TESTOS_STEP_SERVER,	/* the server is up */
	OPXY_TESTOS_STEP_MSI,		/* wb_msi_init(), the last try */
	OPXY_TESTOS_STEP_EMMC,		/* wb_msi_emmc_init(), the last try */
	OPXY_TESTOS_STEP_VOLUME,	/* S-12's volume added */
	OPXY_TESTOS_STEP_FEATURES,	/* S-03.009's switches, the last try */
	OPXY_TESTOS_STEP_MOUNT,		/* the 0x08 mount; 1.. its errno */
	OPXY_TESTOS_STEP_REQUEST,	/* the last request: its command, its len */
	OPXY_TESTOS_STEP_SEND,		/* the last reply: 1 not sent whole, its length */
	OPXY_TESTOS_STEP_QUEUE,		/* finished replies waiting, when read */
	OPXY_TESTOS_STEP_COUNT
};
/* A row: the step in bits 31-24, the outcome in 23-16 (0 done, 0xFF not
 * run, else the driver's code without its sign), bits 15-0 the milliseconds
 * after the server came up, at most 0xFFFF. 0xFFFFFFFF past the last. */
#define OPXY_TESTOS_ROW(step, code, ms) \
	((uint32_t)(step) << 24 | ((uint32_t)(code) & 0xFFu) << 16 | ((uint32_t)(ms) & 0xFFFFu))
#define OPXY_TESTOS_ROW_NOT_RUN 0xFFu
/* src/testos/spi_slave_records.inc: what the last reply's send saw. */
extern volatile uint32_t wb_spi_testos_send[12];
/* The reply set-up to try, poked by the USB monitor; 0 the test OS's default.
 * All four together are S-13b.044's reply set-up (S-13b Q-11's measurement 5). */
extern volatile uint32_t wb_spi_testos_mode;
#define OPXY_TESTOS_TRACE_MAX 40u
extern volatile uint32_t wb_spi_testos_trace[1u + 2u * OPXY_TESTOS_TRACE_MAX];
#define OPXY_TESTOS_REPLY_FCCH   0x01u	/* FCCH 1, FCWM 0 */
#define OPXY_TESTOS_REPLY_TXCTL  0x02u	/* SPI2_TXCTL 0x0001011D, with TTI */
#define OPXY_TESTOS_REPLY_NO_REN 0x04u	/* REN clear */
#define OPXY_TESTOS_REPLY_QUEUE  0x08u	/* two words queued past the last */
#define OPXY_TESTOS_REPLY_DMA    0x10u	/* DMA feed, not the core */
/*
 * The q11 armed replies (wiki/Test-OS.md, `q11`). While any of these bits is
 * set, the whole mode applies only to the next wb_spi_testos_arm replies, one
 * per count, and every other reply is the default one; without them the five
 * bits above apply to every reply. The armed count is poked with
 * OPXY_TESTOS_ARM_SKIP: the next reply, the poke's own, is the default one and
 * clears the bit. Every reply keeps the slave's bound, so each count ends.
 */
#define OPXY_TESTOS_REPLY_EN_LAST  0x020u	/* S-13b.044's order, EN set last */
#define OPXY_TESTOS_REPLY_QUAD     0x040u	/* MIOM 2, SPI2_RXCTL 0 */
#define OPXY_TESTOS_REPLY_TF_LATE  0x080u	/* SPI_IMSK.TF once the DMA is done */
#define OPXY_TESTOS_REPLY_NO_TWCEN 0x100u	/* TWCEN clear: S-13b Q-11 (1e) */
#define OPXY_TESTOS_REPLY_NO_TTI   0x200u	/* TTI clear: S-13b Q-11 (1e) */
#define OPXY_TESTOS_REPLY_TIGHT    0x400u	/* a loop reading SPI_STAT alone: S-13b Q-11 (1d) */
#define OPXY_TESTOS_REPLY_ARMED    0x7E0u
#define OPXY_TESTOS_ARM_SKIP       0x80000000u
extern volatile uint32_t wb_spi_testos_arm;
/* The last armed reply. Times are core cycles after PA4 was handed over,
 * as int32 (before it, negative); OPXY_TESTOS_Q11_NONE for not seen. */
#define OPXY_TESTOS_Q11_NONE 0x80000000u
enum opxy_testos_q11 {
	OPXY_Q11_MODE,		/* the mode used: a bit it could not use is clear */
	OPXY_Q11_RC,		/* the driver's code */
	OPXY_Q11_ENDED,		/* OPXY_Q11_END_* */
	OPXY_Q11_WORD0,		/* the reply's first word: the request it answers */
	OPXY_Q11_DMA_DONE,	/* DMA_STAT.IRQDONE first read set */
	OPXY_Q11_TF_UNMASK,	/* SPI_IMSK.TF set */
	OPXY_Q11_TF_STAT,	/* SPI_STAT.TF first read set */
	OPXY_Q11_TF_ILAT,	/* SPI_ILAT.TF first read set */
	OPXY_Q11_TWC0,		/* SPI_TWC first read 0 */
	OPXY_Q11_PB15_LOW,	/* PB15 first read low: the master selected */
	OPXY_Q11_PB15_HIGH,	/* PB15 high again: the master ended the session */
	OPXY_Q11_SPIF_COUNT,	/* the tight loop's SPIF falls, and rises << 16 */
	OPXY_Q11_SPIF_FIRST,	/* its first SPIF change */
	OPXY_Q11_SPIF_LAST,	/* its last */
	OPXY_Q11_STAT,		/* SPI_STAT at the end */
	OPXY_Q11_TWC,		/* SPI_TWC at the end */
	OPXY_Q11_SLOTS
};
enum {
	OPXY_Q11_END_NONE,
	OPXY_Q11_END_TF,	/* TF read set: rc OK */
	OPXY_Q11_END_COUNT,	/* single I/O: the words received, rc OK */
	OPXY_Q11_END_MASTER,	/* PB15 high after low */
	OPXY_Q11_END_BOUND,	/* the slave's bound */
	OPXY_Q11_END_ERROR,	/* TUR or TC */
	OPXY_Q11_END_NO_HANDOVER	/* PB15 never read high */
};
extern volatile uint32_t wb_spi_testos_q11[OPXY_Q11_SLOTS];
/* Armed replies sent, and those whose code was OK. */
extern volatile uint32_t wb_spi_testos_tally[2];
/*
 * S-13b Q-11 (1g), (1h): the next listens, as many as bits 8-15 count: a request in
 * quad I/O (SPI2_CTL 0x00222510), its set-up written with EN set last, or
 * both. A quad listen that hears nothing for 2 s goes back to single I/O and
 * clears this word. An armed request is served only when it is the echo or the
 * peek (src/board/opxy_slave.c).
 */
#define OPXY_TESTOS_REQ_QUAD        0x1u
#define OPXY_TESTOS_REQ_EN_LAST     0x2u
#define OPXY_TESTOS_REQ_COUNT_SHIFT 8u
extern volatile uint32_t wb_spi_testos_req;
enum opxy_testos_reqstat {
	OPXY_REQ_BITS,		/* the last armed listen's word; bit 31: quad refused */
	OPXY_REQ_RC,		/* its header receive's code */
	OPXY_REQ_HDR0,		/* its three header words as received */
	OPXY_REQ_HDR1,
	OPXY_REQ_HDR2,
	OPXY_REQ_STAT,		/* SPI_STAT or-ed over armed requests until cleared */
	OPXY_REQ_FALLBACKS,	/* quad listens given up after 2 s */
	OPXY_REQ_WHOLE,		/* quad requests whose header came whole */
	OPXY_REQ_SLOTS
};
extern volatile uint32_t wb_spi_testos_reqstat[OPXY_REQ_SLOTS];
/* A request header has come in (rc the driver's code): recorded, and true,
 * when an armed listen made it ready. */
bool wb_spi_testos_req_header(int rc, const uint8_t *hdr);
/* The master's ends of it, in src/testos/opxy_testos.c: the bounded wait for
 * the ready line; the first session answered (bus_configure()'s step 3);
 * after s09_master_start(): blue, the eMMC's bring-up, the table, or brown
 * and the slave absent. */
void opxy_testos_bridge_enter(void);
bool opxy_testos_bridge_ready(const struct s09_bus_ops *bus, uint64_t deadline);
void opxy_testos_bridge_session(void);
void opxy_testos_slave_bringup(struct opxy_master *m);
/* After held mode's services started: the read-outs if the bridge was lost. */
void opxy_testos_bridge_check(struct opxy_master *m);
/* The bridge's first bus operations, for the log's `bridge trace` lines
 * (opxy_ops.c): what ('S' select, 'E' end of select, 'T' send, 'R' receive),
 * the driver's code, the bytes. */
void opxy_testos_bus_note(char what, int rc, const uint8_t *d, size_t n);
/*
 * The slave's beacon on its attention line (PB2 to the master's PC5), until
 * the bridge is linked: bursts of `code` low pulses, 0.5 ms each way, 8 ms
 * apart. The code is how far the slave has come, the furthest so far. Before
 * the first burst the line is held low from the slave's first bring-up step.
 */
#define OPXY_TESTOS_BEACON_NOT_READY 1u	/* PB15 (the master's PC11) low */
#define OPXY_TESTOS_BEACON_NO_LISTEN 2u	/* PB15 high, the driver's listen failed */
#define OPXY_TESTOS_BEACON_LISTENING 3u	/* ready, nothing heard */
#define OPXY_TESTOS_BEACON_HEARD     4u	/* a word heard, no whole request yet */
#define OPXY_TESTOS_BEACON_REQUEST   5u	/* a whole request, no reply sent yet */
#define OPXY_TESTOS_BEACON_SENT      6u	/* a reply sent whole */
#define OPXY_TESTOS_BEACON_SEND_FAIL 7u	/* a reply not sent whole */
#define OPXY_TESTOS_BEACON_DDR_FAIL  8u	/* the DDR check failed: no server */

/*
 * The USB debug monitor (wiki/Test-OS.md, tools/testos_mon.py), in the `mon`
 * variant alone: S-06 frames of this id on the CDC link, served from held
 * mode's loop. Registers and our own memory only (opxy_testos_mem_ok()): no
 * write reaches the flash controller or the OTP, and no read reaches memory
 * outside this image, which may hold another bootloader's code.
 */
#define OPXY_TESTOS_MON_ID 0x7Du
#if defined(OPXY_TESTOS_MONITOR)
#define OPXY_TESTOS_MON_ON true
#else
#define OPXY_TESTOS_MON_ON false
#endif
/* The full-screen paint at each start-up stage (src/testos/opxy_testos_paint.c):
 * the panel variants have it; the monitor and the installer paint only a fatal,
 * and name each stage after the early logo in a line under it. */
#if defined(OPXY_TESTOS_MONITOR) || defined(OPXY_TESTOS_INSTALLER)
#define OPXY_TESTOS_STAGE_PAINTS false
#else
#define OPXY_TESTOS_STAGE_PAINTS true
#endif
/* Whether this address range may be read, or (write) written. */
bool opxy_testos_mem_ok(uint32_t addr, uint32_t len, bool write);
/*
 * The installer's write engine (src/testos/installer_engine.c), in the
 * installer variant: out gets OPXY_INSTALL_REPLY bytes, reporting whichever
 * form ran: 9 bytes of state, OPXY_INSTALL_CRUMBS of a breadcrumb build's
 * marks, then the selected slot's start log (S-02.095) summed up,
 * OPXY_INSTALL_LOG bytes:
 *   [0]    1 when the rest follows: the selected slot holds this image
 *   [1]    the selected slot
 *   [2]    OPXY_INSTALL_LOG_*
 *   [3..4] used entries, u16 little-endian
 *   [5..6] unfinished entries at the end of them, u16
 *   [7..8] the last eight used entries' two bits, the newest lowest, u16
 * Without the log the bytes after [0] are 0xFF. The 9 bytes of state: rc,
 * the flash's status register, the selector's two bytes, each slot's
 * OPXY_INSTALL_SLOT_* bits, the target slot, the writes the gate refused,
 * the metadata mirror (OPXY_INSTALL_MIRROR_*).
 */
enum { OPXY_INSTALL_STATUS, OPXY_INSTALL_TRIAL, OPXY_INSTALL_COMMIT };
#define OPXY_INSTALL_CRUMBS 64u
/* rc: commit refused, the carried image is a breadcrumb build (BOOT_CRUMBS=1) */
#define OPXY_INSTALL_REFUSED_CRUMBS 0x40
/* rc: commit refused, slot k's trial entry is not started (S-02.095) */
#define OPXY_INSTALL_REFUSED_UNSTARTED 0x41
/* rc: no longer given; an older installer refused the selected slot's own image with it */
#define OPXY_INSTALL_REFUSED_SAME 0x42
/* rc: refused, the carried image did not pass inst_image_check() */
#define OPXY_INSTALL_REFUSED_IMAGE 0x43
/* rc: commit refused, the trial start did not reach the boot menu */
#define OPXY_INSTALL_REFUSED_UNREACHED 0x44
#define OPXY_INSTALL_LOG 9u
#define OPXY_INSTALL_REPLY (9u + OPXY_INSTALL_CRUMBS + OPXY_INSTALL_LOG)
enum { OPXY_INSTALL_LOG_OK, OPXY_INSTALL_LOG_ODD, OPXY_INSTALL_LOG_FULL,
       OPXY_INSTALL_LOG_UNREAD, OPXY_INSTALL_LOG_NONE };
#define OPXY_INSTALL_SLOT_GOOD      0x01u	/* S-02.016 */
#define OPXY_INSTALL_SLOT_TRIAL     0x02u	/* this image, with the trial's record */
#define OPXY_INSTALL_SLOT_COMMITTED 0x04u	/* this image, with its right record */
/*
 * A slot with the trial's record, byte b = byte 0 of its log (S-02.095), as
 * the front reads it; at most one bit, none when b is 0xFF (no entry):
 *   PENDING  b == 0xFD
 *   STARTED  (b & 3) == 0, entry 0 finished
 *   SPENT    (b & 3) == 1 and b != 0xFD, entry 0 still 1, 0
 *   ODD      any other b but 0xFF: (b & 3) is 2, or 3 with other bits clear
 */
#define OPXY_INSTALL_SLOT_PENDING   0x08u
#define OPXY_INSTALL_SLOT_STARTED   0x10u
#define OPXY_INSTALL_SLOT_SPENT     0x20u
#define OPXY_INSTALL_SLOT_ODD       0x40u
/* With TRIAL: byte 0 of the log reads OPXY_TRIAL_REACHED_BYTE, the trial
 * start's main image reached the boot menu (waffle/board/opxy_boot.h). */
#define OPXY_INSTALL_SLOT_REACHED   0x80u
enum { OPXY_INSTALL_MIRROR_INVALID, OPXY_INSTALL_MIRROR_SAME, OPXY_INSTALL_MIRROR_DIFFERS,
       OPXY_INSTALL_MIRROR_UNREAD };
/* A write form runs only with image_ok (inst_image_check() passed). */
size_t opxy_installer_write(struct opxy_master *m, unsigned form, bool image_ok, uint8_t *out);

/* The display's DMA as the loading bootloader left it, stopped (S-05.016). */
void opxy_testos_display_dma_stop(void);

/* True when the frame was the monitor's; it has then been answered. */
bool opxy_testos_monitor(struct opxy_master *m, const struct s06_frame *f);

/*
 * src/testos/opxy_testos.c (compiled to nothing outside the test OS).
 * Each is called from the board's normal flow through the macros above.
 */
/* At opxy_main()'s start, before any bring-up: the display record of
 * S-13c.021 as the loading bootloader left it (S-01.019), DMA9's handshake
 * pair (S-01.006) and the registers S-13a expects at OS entry. */
void opxy_testos_capture(void);
/* Once the S-14 log has its PSoC sink: the entry state, the record, the
 * register differences against S-13a, the OTP reads (S-11's notion). */
void opxy_testos_log(struct opxy_master *m);
/* After S-07.032's key status: its raw reply bytes (S-07 Q-CHORD). */
void opxy_testos_log_keys(struct opxy_master *m);
/* s07_key_status() hands the reply's payload here before it is decoded. */
void opxy_testos_key_raw(const uint8_t *payload, size_t len);
/* On the RAM disk, just formatted and still mounted (S-12.002): flash.bin,
 * a copy of the 1 MiB SPI flash read without a programmer, and testos.txt,
 * the lines logged above. */
void opxy_testos_disk_files(struct opxy_master *m);
/* Once held mode's first screen is shown: the frame timing below, on the
 * log (scenario HW-FRAME). */
void opxy_testos_frame_timing(struct opxy_master *m);

/*
 * src/testos/opxy_testos_frame.c: TE's period and high time over
 * OPXY_FT_PERIODS periods, then OPXY_FT_FRAMES frames, each started on a
 * TE rising edge through held mode's frame path and timed from its start
 * to the end of its transfer, and from there to the next rising edge (a
 * frame still going out at that edge counts as an overrun). Times are
 * cycle counts.
 */
#define OPXY_FT_PERIODS       8u
#define OPXY_FT_FRAMES        4u
#define OPXY_FT_TE_WAIT_US    100000u	/* a TE edge (S-13c.018: 15.4-20 ms periods) */
#define OPXY_FT_FRAME_WAIT_US 40000u	/* a transfer's end (S13C_FRAME_TIMEOUT_US) */
#define OPXY_FT_OK        0
#define OPXY_FT_NO_TE     1	/* no TE edge within OPXY_FT_TE_WAIT_US */
#define OPXY_FT_NO_FRAME  2	/* a frame did not start */
#define OPXY_FT_STUCK     3	/* a transfer did not end within OPXY_FT_FRAME_WAIT_US */
struct opxy_testos_frame_ops {
	uint64_t (*cycles)(void *ctx);	/* the cycle counter */
	bool (*te)(void *ctx);		/* TE's level */
	int (*start)(void *ctx);	/* a frame of the screen as it is; 0: it runs */
	bool (*busy)(void *ctx);	/* its transfer still runs */
	uint32_t cycles_per_us;
	void *ctx;
};
struct opxy_testos_span {
	uint64_t min, max, sum;
	uint32_t n;
};
struct opxy_testos_frame_time {
	struct opxy_testos_span period, high;	/* TE */
	/* TE edge to the frame's start, start to the transfer's end, end to
	 * the next TE rising edge */
	struct opxy_testos_span latency, frame, left;
	uint32_t overruns;
};
int opxy_testos_frame_measure(const struct opxy_testos_frame_ops *o,
			      struct opxy_testos_frame_time *ft);
/* The two log lines of a measure's result rc. */
void opxy_testos_frame_lines(int rc, const struct opxy_testos_frame_time *ft,
			     uint32_t cycles_per_us, struct s14_text *te,
			     struct s14_text *frame);

/* The S-02 flash ops' erase and program in the test OS: refuse, count. */
int opxy_testos_flash_erase(void *ctx, uint32_t sector_off);
int opxy_testos_flash_program(void *ctx, uint32_t off, const uint8_t *buf,
			      uint32_t len);

/*
 * The check suite (src/testos/opxy_testos_check.c, waffle/board/testos_check.h),
 * in the installer variant alone. check_start(): before held mode's entry
 * steps, the Nordic link's gate and the USB set; check_service(): held
 * mode's loop in place of the menus; check_disk(): on the RAM disk, just
 * formatted and still mounted, check.txt in place of testos_disk_files().
 */
struct boot_held;
void opxy_testos_check_start(struct opxy_master *m);
void opxy_testos_check_service(void *ctx, const struct boot_held *h);
void opxy_testos_check_disk(struct opxy_master *m);
/* What src/testos/opxy_testos.c kept for the suite: the registers read at
 * entry (opxy_testos_regs' order), whether the key status was answered, and
 * the display waits cut short. */
void opxy_testos_check_kept(const uint32_t **regs, unsigned *n, bool *key_replied,
			    uint32_t *te_misses, uint32_t *frame_timeouts);

#endif /* WAFFLE_TESTOS_H */

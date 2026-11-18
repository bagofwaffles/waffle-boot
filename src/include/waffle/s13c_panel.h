/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-13c the display panel as the panel sees it: the reset decision and
 * pulse (S-13c.004 - S-13c.006), the initialization (S-13c.007 -
 * S-13c.013), frames and their pacing (S-13c.014 - S-13c.018), power-off
 * (S-13c.022) and the hand-over state and record (S-13c.020, S-13c.021).
 *
 * Hardware-independent: the bus comes in as struct s13c_bus. The PSoC's
 * brightness messages (S-13c.019) are S-07's (s07_report); the caller
 * sends them at the points s13c_start(), s13c_handover() and
 * s13c_poweroff() document.
 */
#ifndef WAFFLE_S13C_PANEL_H
#define WAFFLE_S13C_PANEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Panel commands (ST7796S datasheet names). */
#define S13C_RDDID   0x04u
#define S13C_SLPOUT  0x11u
#define S13C_INVON   0x21u
#define S13C_DISPOFF 0x28u
#define S13C_DISPON  0x29u
#define S13C_CASET   0x2Au
#define S13C_RASET   0x2Bu
#define S13C_RAMWR   0x2Cu
#define S13C_TEON    0x35u
#define S13C_MADCTL  0x36u
#define S13C_COLMOD  0x3Au
#define S13C_B9      0xB9u
#define S13C_RDID4   0xD3u
#define S13C_DGR     0xE2u		/* digital gamma, red table */
#define S13C_DGB     0xE3u		/* digital gamma, blue table */
#define S13C_CSCON   0xF0u

/* S-13c terms: the visible window, and the frame window of S-13c.014. */
#define S13C_VIS_X0    49u
#define S13C_VIS_W     222u
#define S13C_ROWS      480u
#define S13C_FRAME_X0  49u
#define S13C_FRAME_W   224u		/* columns 49-272 */
#define S13C_FRAME_BYTES (2u * S13C_FRAME_W * S13C_ROWS)	/* 215 040 */
#define S13C_ROW_BYTES   (2u * S13C_FRAME_W)

/* S-13c.005, .006, .007 (timing.tsv), with the margins this code adds. */
#define S13C_RESET_LOW_US     20u	/* >= 10 us */
#define S13C_RESET_WAIT_US    120000u	/* release to the first transaction */
#define S13C_SLPOUT_WAIT_US   5100u	/* >= 5 ms after SLPOUT */
#define S13C_TE_TIMEOUT_US    50000u	/* T_TE_MISS, 40-100 ms (S-13c.017) */
#define S13C_FRAME_TIMEOUT_US 40000u
/* S-13c.022: at a power-off, three frame times at the longest: a frame
 * ends before the next TE edge (S-13c.018), at most 20 ms apart. */
#define S13C_POWEROFF_FRAME_US 60000u

/* S-01.006: the first stage's handshake value in DMA9_XMOD. */
#define S13C_HANDSHAKE_MAGIC  0x12345678u

/* S-13c.021 */
#define S13C_RECORD_SIZE      44u
#define S13C_RECORD_VERSION   2u

struct s13c_bus {
	void (*reset)(void *ctx, bool high);		/* drive RESET */
	void (*select)(void *ctx, bool on);		/* CS low when on */
	void (*cmd)(void *ctx, uint8_t c);		/* D/C low */
	void (*data)(void *ctx, const uint8_t *p, size_t n);	/* D/C high */
	void (*repeat)(void *ctx, const uint8_t *group, size_t len,
		       uint32_t count);
	/* CS high, low, cmd, n read strobes (the first the dummy), CS
	 * high; 0 when done. */
	int (*read)(void *ctx, uint8_t cmd, uint8_t *out, size_t n);
	/* A frame's parameter bytes after its RAMWR, sent in the
	 * background; 0 when it runs. */
	int (*frame_start)(void *ctx, const uint8_t *buf, uint32_t n);
	bool (*frame_busy)(void *ctx);
	/* The frame going out given up: its transfer stopped, its buffer not
	 * read after the return. NULL: a bus whose frames end before
	 * frame_start() returns. */
	void (*frame_stop)(void *ctx);
	/* 0 after a TE rising edge, nonzero after timeout_us without. */
	int (*te_wait)(void *ctx, uint32_t timeout_us);
	/* Microseconds on S-13a's time base: ms time is now_us / 1000. */
	uint64_t (*now_us)(void *ctx);
	void (*delay_us)(void *ctx, uint32_t us);
	void *ctx;
};

/* S-13c.004's three cases. */
enum s13c_reset_path {
	S13C_RESET_HANDSHAKE,	/* the first stage's pulse and time */
	S13C_RESET_PULSE,	/* this bootloader's pulse */
	S13C_RESET_STRAP,	/* PA8 strapped low: the release ends it */
};

/*
 * S-13c.004: which case holds, from DMA9_XMOD, DMA9_YMOD, the PA8 strap
 * level and the ms time at the check; *handshake_ms is the first stage's
 * time (0 read as 1) when the case is S13C_RESET_HANDSHAKE.
 */
enum s13c_reset_path s13c_reset_decide(uint32_t xmod, uint32_t ymod,
				       unsigned pa8, uint32_t now_ms,
				       uint32_t *handshake_ms);

/* S-13c.021: the record for a panel reset at reset_ms. */
void s13c_record(uint32_t reset_ms, uint8_t rec[S13C_RECORD_SIZE]);

enum s13c_state {
	S13C_IDLE,	/* nothing driven yet */
	S13C_RESET,	/* reset decided; waiting to initialize */
	S13C_ON,	/* initialized, display on */
	S13C_HANDED,	/* the hand-over frame sent: nothing more */
	S13C_OFF,	/* powered off: nothing more is sent (S-13c.022) */
};

struct s13c_panel {
	const struct s13c_bus *bus;
	enum s13c_state state;
	enum s13c_reset_path path;
	uint32_t reset_ms;	/* the panel's reset time, ms time */
	uint64_t ready_us;	/* the first transaction not before */
	bool gamma_loaded;	/* E2 and E3 sent */
	bool gamma_on;		/* the last B9 is 42 E0 */
	bool frame;		/* a frame may be in flight */
	bool boot_frame;	/* the last frame is the hand-over frame */
	bool lined;		/* a line under the logo written since (S-13c.025) */
	bool select_due;	/* adopted: CS low before the next transaction */
	/* S-13c.011: the identification reads, informative only. */
	bool id_valid;
	uint8_t rddid[3], rdid4[3];
	unsigned te_misses;	/* paced frames sent after a TE timeout */
};

#define S13C_OK         0
#define S13C_ERR_STATE (-1)
#define S13C_ERR_BUS   (-2)

/*
 * S-13c.004, S-13c.005: the first drive of RESET (high), then the case's
 * reset: none, one pulse, or the strap interval's end. Call it before any
 * DMA9 write (S-13b.081) and, on the boot path, early: s13c_start()
 * waits out what is left of the 120 ms.
 */
void s13c_begin(struct s13c_panel *p, const struct s13c_bus *bus,
		uint32_t xmod, uint32_t ymod, unsigned pa8);

/*
 * S-13c.013 across the front's hand-off (S-01.023): the front reset and
 * initialized the panel, with the gamma load, and sent the logo frame;
 * this run's driver takes the panel on as it is, at reset_ms (S-13c.021),
 * and sends nothing. This run's bus set-up leaves CS high (S-13c.001), so
 * the next transaction selects the panel first. The digital gamma counts as
 * not loaded: s13c_gamma(p, true) loads it again (S-13c.012).
 */
void s13c_adopt(struct s13c_panel *p, const struct s13c_bus *bus,
		uint32_t reset_ms);

/*
 * S-13c.006 - S-13c.011, S-13c.013: wait for the reset to complete, read
 * the identification registers, initialize (with the digital-gamma load
 * when gamma), write the visible window black and turn the display on. The
 * second and later calls send nothing (one initialization per run). PSoC
 * message 0x51 {00 00 00} must precede it; {3C FF 02} for held mode
 * follows it (S-13c.019).
 */
int s13c_start(struct s13c_panel *p, bool gamma);

/* S-13c.012: digital gamma on (loading the tables first if needed) or
 * off, inside one command-set-2 window. */
int s13c_gamma(struct s13c_panel *p, bool on);

/*
 * S-13c.015 - S-13c.018: one frame of S13C_FRAME_BYTES from buf, after the
 * previous frame has ended. paced: after a TE rising edge (held mode);
 * otherwise at once (the fatal-error frame). buf must stay unchanged
 * until s13c_frame_busy() is false.
 */
int s13c_frame(struct s13c_panel *p, const uint8_t *buf, bool paced);
bool s13c_frame_busy(struct s13c_panel *p);
int s13c_frame_wait(struct s13c_panel *p);
/* S-13c.024: a frame in flight given up, its transfer stopped (a frame
 * that did not end within its timeout). True when no frame is in flight
 * on the return, false when the bus cannot stop one. */
bool s13c_frame_stop(struct s13c_panel *p);

/*
 * S-05.004, S-13c.012, S-13c.020: the hand-over frame, before the OS is
 * read. The digital-gamma tables and B9 42 E0 unless on, then a frame
 * sent at once whose rows row(ctx, r, out) gives (S13C_ROW_BYTES each,
 * r = 0..479), ended before it returns. 0x51 {3C FF 32} follows it
 * (S-13c.019). A load that fails goes on to held mode's frames.
 */
int s13c_boot_frame(struct s13c_panel *p,
		    void (*row)(void *ctx, unsigned r, uint8_t *out),
		    void *ctx);

/*
 * S-13c.025: a line under the logo while the panel shows the early logo or
 * the hand-over frame (the last frame s13c_boot_frame()'s or the front's):
 * columns 49 + y to 49 + y + h - 1 (screen y to y + h - 1, S-13c.023) and
 * rows 0-479, whose rows row(ctx, r, out) gives (2 * h bytes each,
 * r = 0..479), written at once, then S-13c.014's window again. The panel
 * no longer shows the hand-over frame alone: s13c_handover() refuses until
 * the next s13c_boot_frame(). S13C_ERR_STATE when the panel shows anything
 * else, or for a band outside the visible window or taller than
 * S13C_LINE_MAX_H.
 */
#define S13C_LINE_MAX_H 16u
int s13c_logo_line(struct s13c_panel *p, unsigned y, unsigned h,
		   void (*row)(void *ctx, unsigned r, uint8_t *out), void *ctx);

/*
 * S-13c.020, S-13c.021: at the kernel call, after s13c_boot_frame() and
 * no frame since: rec holds the record; no transaction follows.
 */
int s13c_handover(struct s13c_panel *p, uint8_t rec[S13C_RECORD_SIZE]);

/*
 * S-13c.022: no new frame; the one in progress ends, waited for at most
 * S13C_POWEROFF_FRAME_US, and is stopped where it is when it has not;
 * nothing is sent, no DISPOFF: the panel keeps its last frame.
 * 0x51 {00 00 02} follows the return (S-13c.019). S13C_ERR_BUS when a
 * frame is still in flight (a bus that cannot stop one).
 */
int s13c_poweroff(struct s13c_panel *p);

#endif /* WAFFLE_S13C_PANEL_H */

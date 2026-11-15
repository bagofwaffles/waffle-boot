/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_SPI_H
#define WAFFLE_DRIVERS_BF70X_SPI_H

/*
 * SPI controllers, core-driven and DMA transfers (ADSP-BF70x Hardware
 * Reference chapter "Serial Peripheral Interface (SPI)").
 *
 * The board passes whole SPI_CTL, SPI_CLK and SPI_DLY values (S-13b.010,
 * .020, .040-.042 give this board's). The driver adds SPI_CTL.EN itself.
 *
 * Every transfer moves whole words of SPI_CTL.SIZE. A word is taken from
 * and stored to the buffer in memory order (as DMA moves it), so with
 * 16- or 32-bit words on this little-endian core the buffer's last byte
 * of each word goes out first when SPI_CTL.LSBF is clear.
 *
 * Transfers are half duplex (send or recv) or full duplex (xfer); in dual
 * or quad I/O mode (SPI_CTL.MIOM) only send and recv apply. The HRM gives
 * quad I/O for SPI2 only.
 *
 * The memory-mapped read window (SPI_CTL.MMSE, SPI_MMRDH, SPI_MMTOP) is not
 * set up: the bootloader must not use it (S-13b.019).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_time.h"

#define WB_SPI_COUNT 3u

#define SPI_BASE(n)        (0x20055000u + 0x1000u * (n))
#define SPI_CTL(n)         (SPI_BASE(n) + 0x04u)
#define SPI_RXCTL(n)       (SPI_BASE(n) + 0x08u)
#define SPI_TXCTL(n)       (SPI_BASE(n) + 0x0Cu)
#define SPI_CLK(n)         (SPI_BASE(n) + 0x10u)
#define SPI_DLY(n)         (SPI_BASE(n) + 0x14u)
#define SPI_SLVSEL(n)      (SPI_BASE(n) + 0x18u)
#define SPI_RWC(n)         (SPI_BASE(n) + 0x1Cu)
#define SPI_RWCR(n)        (SPI_BASE(n) + 0x20u)
#define SPI_TWC(n)         (SPI_BASE(n) + 0x24u)
#define SPI_TWCR(n)        (SPI_BASE(n) + 0x28u)
#define SPI_TWC_MAX        0xFFFFu	/* a 16-bit word count */
#define SPI_IMSK_CLR(n)    (SPI_BASE(n) + 0x34u)
#define SPI_IMSK_SET(n)    (SPI_BASE(n) + 0x38u)
#define SPI_STAT(n)        (SPI_BASE(n) + 0x40u)
#define SPI_ILAT(n)        (SPI_BASE(n) + 0x44u)
#define SPI_ILAT_CLR(n)    (SPI_BASE(n) + 0x48u)
#define SPI_RFIFO(n)       (SPI_BASE(n) + 0x50u)
#define SPI_TFIFO(n)       (SPI_BASE(n) + 0x58u)
#define SPI_MMRDH(n)       (SPI_BASE(n) + 0x60u)
#define SPI_MMTOP(n)       (SPI_BASE(n) + 0x64u)

/* S-13b.044: a reply's TXCTL: TEN, TTI, TWCEN, TDR 1, TDU 1, TUWM 1. */
#define SPI_TXCTL_REPLY    0x0001011Du

/* SPI_CTL. */
#define SPI_CTL_EN         (1u << 0)
#define SPI_CTL_MSTR       (1u << 1)
#define SPI_CTL_ASSEL      (1u << 6)
#define SPI_CTL_SELST      (1u << 7)
#define SPI_CTL_EMISO      (1u << 8)
#define SPI_CTL_SIZE_SHIFT 9u
#define SPI_CTL_SIZE_MASK  (3u << 9)
#define SPI_CTL_SIZE8      (0u << 9)
#define SPI_CTL_SIZE16     (1u << 9)
#define SPI_CTL_SIZE32     (2u << 9)
#define SPI_CTL_LSBF       (1u << 12)
#define SPI_CTL_FCEN       (1u << 13)
#define SPI_CTL_FCCH       (1u << 14)	/* slave: 1 flow control on TX */
#define SPI_CTL_FCPL       (1u << 15)	/* 1: RDY active high */
#define SPI_CTL_FCWM_MASK  (3u << 16)
#define SPI_CTL_FCWM_TX_EMPTY (0u << 16)	/* FCCH 1: TX FIFO empty */
#define SPI_CTL_FCWM_RX_HALF  (2u << 16)	/* FCCH 0: RX FIFO half full */
#define SPI_CTL_FMODE      (1u << 18)
#define SPI_CTL_MIOM_MASK  (3u << 20)
#define SPI_CTL_MIOM_DUAL  (1u << 20)
#define SPI_CTL_MIOM_QUAD  (2u << 20)
#define SPI_CTL_MMSE       (1u << 31)
#define SPI_CTL_CPHA       (1u << 4)
#define SPI_CTL_CPOL       (1u << 5)

/* SPI_RXCTL and SPI_TXCTL. */
#define SPI_XCTL_EN        (1u << 0)	/* REN, TEN */
#define SPI_XCTL_TI        (1u << 2)	/* RTI, TTI */
#define SPI_XCTL_WCEN      (1u << 3)	/* RWCEN, TWCEN */
#define SPI_XCTL_DR_NE     (1u << 4)	/* RDR not empty, TDR not full */

/* SPI_STAT. The W1C bits are ROR..TF and the memory-mapped errors. */
#define SPI_STAT_SPIF      (1u << 0)
#define SPI_STAT_ROR       (1u << 4)
#define SPI_STAT_TUR       (1u << 5)
#define SPI_STAT_TC        (1u << 6)
#define SPI_STAT_MF        (1u << 7)
#define SPI_STAT_RS        (1u << 8)
#define SPI_STAT_TS        (1u << 9)
#define SPI_STAT_RF        (1u << 10)
#define SPI_STAT_TF        (1u << 11)
#define SPI_STAT_RFS_MASK  (7u << 12)
#define SPI_STAT_RFS_FULL  (4u << 12)	/* two 32-bit words */
#define SPI_STAT_TFS_MASK  (7u << 16)
#define SPI_STAT_FCS       (1u << 20)	/* master: the slave's ready line holds it */
#define SPI_STAT_TFS_EMPTY (4u << 16)
#define SPI_STAT_RFE       (1u << 22)
#define SPI_STAT_TFF       (1u << 23)
#define SPI_STAT_W1C       0xB0000FF0u

/* SPI_SLVSEL: SSEn (bit n) enables SPI_SELn, SSELn (bit 8 + n) drives it. */
#define SPI_SLVSEL_SSE(s)  (1u << (s))
#define SPI_SLVSEL_SSEL(s) (1u << (8u + (s)))
#define SPI_SLVSEL_IDLE    0xFE00u	/* every SSELn high, none enabled */

struct wb_spi_config {
	uint32_t ctl;		/* SPI_CTL without EN */
	uint32_t clk;		/* BAUD: SCK = SCLK0 / (clk + 1) */
	uint32_t dly;
	uint8_t sel;		/* SPI_SELn driven by software, 1-7; 0 none */
};

struct wb_spi {
	unsigned n;
	uint32_t ctl;		/* as configured, without EN */
	uint32_t clk, dly;	/* as configured */
	uint8_t sel;
	const struct wb_time *t;
	/*
	 * The longest a single word may take; with flow control this is the
	 * longest the other end may hold its ready line (S-09 T_REPLY).
	 */
	uint32_t word_timeout_us;
	/*
	 * Called while a master's operation waits longer than about a
	 * millisecond, at most about once a millisecond, never between words
	 * that move; NULL for none, as wb_spi_init() leaves it.
	 */
	void (*idle)(void);
};

#define WB_SPI_OK         0
#define WB_SPI_ERR_ARG   (-1)
#define WB_SPI_TIMEOUT   (-2)
#define WB_SPI_ERR_HW    (-3)	/* receive overrun or mode fault */

/*
 * The last failure of an operation on a controller: the return code, SPI_STAT
 * as read at the failure, the words not moved (for a send the transmit word
 * count or the DMA's current count, for a receive the words still expected)
 * and where it failed. All zero when no operation has failed since the
 * session's first operation began (the master: the first after
 * wb_spi_init(), or a bridge session's first; the slave: a reply or a
 * listen).
 */
struct wb_spi_fault {
	int32_t rc;
	uint32_t stat;
	uint32_t left;
	uint32_t stage;
};

#define WB_SPI_STAGE_NONE         0u
#define WB_SPI_STAGE_ARG          1u	/* the call's arguments refused */
#define WB_SPI_STAGE_TX_ROOM      2u	/* waiting for room in the transmit FIFO */
#define WB_SPI_STAGE_TX_END       3u	/* waiting for the transmit to finish (TF) */
#define WB_SPI_STAGE_RX_WORD      4u	/* the master waiting for a received word */
#define WB_SPI_STAGE_DMA_START    5u	/* the DMA channel refused the transfer */
#define WB_SPI_STAGE_DMA_STALL    6u	/* the DMA's count stopped moving */
#define WB_SPI_STAGE_DMA_ERR      7u	/* the DMA reported an error */
#define WB_SPI_STAGE_OVERRUN      8u	/* receive overrun or mode fault at the end */
#define WB_SPI_STAGE_MASTER_READY 9u	/* slave: the master-ready pin never read high */
#define WB_SPI_STAGE_FIRST_WORD  10u	/* slave: the call's first word never came */
#define WB_SPI_STAGE_NEXT_WORD   11u	/* slave: a later word never came */
#define WB_SPI_STAGE_MASTER_END  12u	/* slave: PB15 rose with words to go */
#define WB_SPI_STAGE_TX_ERR      13u	/* slave: transmit underrun or collision */
#define WB_SPI_STAGE_REPLY_BOUND 14u	/* slave: a reply ran out its bound */
#define WB_SPI_STAGE_REFUSED     15u	/* slave: the session callback refused the header */
/*
 * Slave: a reply that ended after a receive overrun, from the transmit side
 * (wb_spi_slave_send()). The send returned WB_SPI_OK; the record has rc
 * WB_SPI_ERR_HW, SPI_STAT with ROR, and as `left` the words received in
 * bits 16-31 and the reply's words in bits 0-15.
 */
#define WB_SPI_STAGE_REPLY_OVERRUN 16u
#define WB_SPI_STAGE_MASK        0xFFu
/* With WB_SPI_STAGE_DMA_ERR: DMA_STAT.ERRC in bits 8-10. */
#define WB_SPI_STAGE_ERRC_SHIFT  8u

int wb_spi_check(unsigned n, const struct wb_spi_config *c);

/*
 * Disable the controller, write SLVSEL (every select high, sel enabled),
 * CLK, DLY and CTL, then enable it with both channels off. Both word count
 * reloads are written 0, so that a count that runs out is not loaded again
 * with whatever was left there before this image (HRM, SPI_RWCR, SPI_TWCR);
 * nothing else in the driver writes them but wb_spi_restart(), also 0.
 */
int wb_spi_init(struct wb_spi *s, unsigned n, const struct wb_spi_config *c,
		const struct wb_time *t, uint32_t word_timeout_us);

/*
 * Change CTL and CLK between transfers (e.g. single to quad I/O,
 * S-13b.041): disable, write, enable. sel and DLY stay.
 */
int wb_spi_reconfigure(struct wb_spi *s, uint32_t ctl, uint32_t clk);

/*
 * The clock divider, between transfers: disable, write CLK, enable with CTL
 * as configured, which also empties the FIFOs. With `fast`, CTL also has
 * SPI_CTL.FMODE: a master's received bits are sampled at the next transmit
 * edge, a full cycle after they were driven, which the HRM asks for at
 * SCLK0 / 2 ("Fast Mode"; what is sent is the same). The configured divider
 * and control value are kept: wb_spi_clock(s, s->clk, false) puts them back.
 */
int wb_spi_clock(const struct wb_spi *s, uint32_t clk, bool fast);

void wb_spi_disable(unsigned n);

/* The idle hook (struct wb_spi's idle). */
void wb_spi_set_idle(struct wb_spi *s, void (*idle)(void));

/* The controller's last failure (struct wb_spi_fault). */
void wb_spi_last_fault(const struct wb_spi *s, struct wb_spi_fault *out);
/*
 * With it, as read at that failure (before the channel was stopped or the
 * controller cleared): out[0] the DMA channel's DMA_STAT (RUN, ERRC,
 * IRQDONE, FIFOFILL, PBWID), out[1] its DMA_CFG, out[2] SPI_TXCTL and out[3]
 * SPI_TWC, the transmit channel's, or for a failed receive the receive
 * channel's DMA_STAT and DMA_CFG, SPI_RXCTL and SPI_RWC. All zero for a
 * refused call and when none has failed.
 */
void wb_spi_last_fault_more(const struct wb_spi *s, uint32_t out[4]);

/*
 * Everything a transfer, DMA-fed or not, may have left, put back as a fresh
 * wb_spi_init() leaves it, at this control value and clock: the controller
 * disabled, both its DMA channels (SPIn TX and RX) stopped once idle and
 * cleared, SLVSEL, CLK, DLY, both channels off, the four word counts and
 * reloads 0, the interrupt masks and latches clear, the status cleared,
 * the controller enabled again. The fault record and the idle hook are
 * kept (the next operation clears the record). Not for SPI2 on the master
 * before the display set-up (S-13b.081: it writes DMA9).
 */
int wb_spi_restart(struct wb_spi *s, uint32_t ctl, uint32_t clk);
/* Whether the controller may hold anything of a failed transfer, so that
 * wb_spi_restart() is due: an operation has failed since its session's
 * first operation began, or one of its DMA channels is still enabled. */
bool wb_spi_spoiled(const struct wb_spi *s);

/* Drive the configured SPI_SELn: on is low. */
void wb_spi_select(const struct wb_spi *s, bool on);

/* Bytes per word. */
unsigned wb_spi_word_bytes(const struct wb_spi *s);

/*
 * Core-driven transfers of n bytes, a whole number of words. xfer() sends
 * tx (zeros if NULL) and stores what comes back in rx (dropped if NULL).
 * send() and recv() take at most SPI_TWC_MAX words
 * (the word counters are 16 bits). Each word goes as soon as SPI_STAT
 * shows room (or a word); only a FIFO found full (or empty) starts the
 * word's bound, and only such a wait calls the idle hook.
 */
int wb_spi_xfer(const struct wb_spi *s, const uint8_t *tx, uint8_t *rx,
		size_t n);
int wb_spi_send(const struct wb_spi *s, const uint8_t *tx, size_t n);
int wb_spi_recv(const struct wb_spi *s, uint8_t *rx, size_t n);

/*
 * A run of single words on a master in single I/O, each started by the
 * caller, one in flight at a time (the slave boot link's bytes, S-13b.022).
 * words_begin() opens both channels once for the run; word() sends one
 * word and returns what came back once the word has been clocked in full,
 * so that the caller's next look at the slave's ready line follows the
 * word; words_end() closes the run as wb_spi_xfer() ends. A word touches
 * the transmit FIFO, the status and the receive FIFO and nothing else: no
 * clock is read unless the word is late, and then its bound is the word
 * timeout. A word that never comes in returns all ones, and words_end()
 * returns the run's first failure.
 */
int wb_spi_words_begin(const struct wb_spi *s);
uint32_t wb_spi_word(const struct wb_spi *s, uint32_t tx);
int wb_spi_words_end(const struct wb_spi *s);
/* Whether a word of the run open now, or of the last one, never came in. */
bool wb_spi_words_failed(const struct wb_spi *s);

/*
 * The bridge's sessions on the master (S-13b.083, S-13b.040): every request
 * session sent through the controller's transmit DMA (SPIn TX = DMA4 + 2n)
 * and every reply session received through its receive DMA (DMA5 + 2n), from
 * a descriptor array in L1, with SPI_CTL.EN set as the set-up's last write.
 * The caller drives the chip select and waits for the slave's ready line
 * before and after (S-09.010); the controller's flow control holds each word.
 * Every wait is a word's bound (word_timeout_us) from the last movement of
 * the word count or the DMA's count, and only such a wait calls the idle
 * hook. A failed transfer leaves its DMA channel disabled with its status
 * acknowledged, SPI_IMSK.TF masked, both channels off and EN clear, and is
 * recorded (wb_spi_last_fault()).
 *
 * dma_send(): one request session of n bytes (a whole number of words, fewer
 * than WB_SPI_BRIDGE_MAX) from buf, its data cache lines written back first.
 * With `rung`, under that control value (without EN; the same word size) and
 * clock, set up as S-13b.040 gives for a request session on a rung, and
 * afterwards SPI_CTL 0 and the clock put back. Its end is the transmit-finished
 * interrupt, SPI_IMSK.TF unmasked once the DMA's done status has been seen and
 * SPI_ILAT.TF read (polled: no handler); then TEN and EN are cleared.
 *
 * dma_recv(): one part of a reply session, n bytes into buf. The session's
 * first part is set up with EN clear and EN set last; a later one (S-13b.040's
 * reply taken in parts) gives the DMA its new array, SPI_RWC and REN with EN
 * set and SPI_CTL untouched. A part ends at the DMA's done status: it is
 * acknowledged and REN cleared. A buffer outside L1 has its whole cache lines
 * invalidated first, and its partial first and last lines go through
 * separate 32-byte buffers in L1, copied out at the part's end.
 *
 * dma_close(): the session's last part has moved: EN cleared and the receive
 * DMA disabled, before the caller waits for the ready line to rise. Nothing
 * after a send, which ends so itself, or after a failure.
 */
#define WB_SPI_BRIDGE_MAX 0x40000u
int wb_spi_dma_send(const struct wb_spi *s, const struct wb_spi_config *rung,
		    const uint8_t *buf, size_t n);
int wb_spi_dma_recv(const struct wb_spi *s, uint8_t *buf, size_t n);
void wb_spi_dma_close(const struct wb_spi *s);

/*
 * The slave end of a link whose ready line is a GPIO between transfers:
 * the slave end of the bridge (S-13b.042, S-13b.043, S-09.011), SPI2 with
 * PA4 as ready and PB15 as master-ready. (The slave's end of the boot
 * stream is its boot ROM's SPI slave boot, S-09; no code here.)
 *
 * The ready pin is a GPIO output driven high (not ready) except while a
 * transfer runs: it is handed to the controller (FER set; the pin's MUX
 * must select SPIn_RDY, S-13b.003) once the transfer is set up and the
 * last read of the master-ready pin was high, and taken back (FER clear)
 * when the slave has moved the last word, on a timeout and on an error.
 *
 * Every session's bytes move by DMA (S-13b.083): a request is received
 * through the controller's receive DMA (DMA5 + 2n: DMA9 for SPI2), a reply
 * sent through its transmit DMA (DMA8), from descriptor arrays in L1. The
 * master clocks the words; SPI_RDY with flow control holds it while the
 * slave is not ready for the next word (S-13b.044).
 */
struct wb_spi_slave {
	struct wb_spi spi;	/* ctl without MSTR, with FCEN */
	uint8_t ready_pin;	/* PA4 */
	uint8_t master_pin;	/* PB15, high = the master is ready */
};

/* A reply session's first part: wait for the master-ready pin to read high
 * before the hand-over. */
#define WB_SPI_SLAVE_SESSION 1u

/*
 * How long a reply whose received-word count fell short (a receive overrun)
 * waits, its last word gone from the transmit FIFO, for any further word
 * before it takes the end as passed (wb_spi_slave_send(), S-13b.045): far
 * above a word's time at any rung, so that the take-back stays within 1 ms
 * of the last word, far below the master's T_REPLY for PA6 to rise
 * (S-09.060).
 */
#define SPI_REPLY_QUIET_US 500u

/*
 * Makes the ready pin a GPIO output driven high and the master-ready pin
 * an input, then wb_spi_init(). c->ctl must be a slave's (no MSTR) with
 * FCEN; c->sel 0.
 */
int wb_spi_slave_init(struct wb_spi_slave *s, unsigned n,
		      const struct wb_spi_config *c, uint8_t ready_pin,
		      uint8_t master_pin, const struct wb_time *t,
		      uint32_t word_timeout_us);

/*
 * A request session made ready and left ready (S-13b.043, S-13b.083): SPI2
 * set for a request (S-13b.044; with `quad`, in quad I/O as S-13b.042 gives),
 * the receive DMA given the first part's array, `head` bytes into rx (the
 * 12-byte header, S-09.020), and, once the master-ready pin reads high (at
 * most start_us), the ready pin handed to SPI2. It stays handed over with no
 * bound of time, so the caller can do other work while no session has begun;
 * wb_spi_slave_heard() says when the master's first word is in. WB_SPI_TIMEOUT
 * when the pin never read high: nothing is handed over. rx stays the caller's
 * until the request has ended (wb_spi_slave_request()) or the listen is given
 * up (wb_spi_slave_unlisten()).
 */
int wb_spi_slave_listen(const struct wb_spi_slave *s, uint8_t *rx, size_t head,
			bool quad, uint32_t start_us);
bool wb_spi_slave_heard(const struct wb_spi_slave *s);
/* A listen given up before a word came: the ready pin taken back, the
 * receive DMA disabled, SPI2 disabled. */
void wb_spi_slave_unlisten(const struct wb_spi_slave *s);

/*
 * The request session a heard listen began, in its two parts (S-13b.083,
 * S-09.011). The first part, the header, within start_us; then len(ctx, rx,
 * &tail) gives the session's length in bytes, the header included, and the
 * bytes after it a master may leave out (S-09.021's further bytes); it runs
 * while the master is held and must be short. The rest's array is given to
 * the DMA and it is enabled, and nothing else of SPI2 is written: the first
 * `room` bytes of the session stay in rx, the rest is received into one
 * place and dropped (S-09.037), the further bytes last, in parts of less
 * than WB_SPI_BRIDGE_MAX bytes each (S-13b.083). A part has failed
 * when its DMA's count has not moved for word_timeout_us, or, with no word
 * waiting, the master-ready pin reads high (the master ended the session,
 * S-09.013 rule 1); a session so ended once only the further bytes are left
 * carries a whole request (S-09.021) and ends well. After the last part the
 * ready pin is taken back, then EN and then REN cleared. *kept is the bytes in
 * rx, after a failure too: those of the parts that came whole. A length
 * shorter than the header, or not whole words, fails with WB_SPI_ERR_ARG
 * (WB_SPI_STAGE_REFUSED); a receive overrun fails with WB_SPI_ERR_HW.
 */
typedef uint64_t (*wb_spi_session_len)(void *ctx, const uint8_t *head,
				       uint32_t *tail);
int wb_spi_slave_request(const struct wb_spi_slave *s, size_t room,
			 wb_spi_session_len len, void *ctx, size_t *kept,
			 uint32_t start_us);

/*
 * S-13b.083's two ends of a single-I/O reply fed by DMA8, one in use at a
 * time. The TF end, the default: REN clear for the whole reply,
 * SPI_IMSK.TF unmasked once DMA8's done status has been seen, the end
 * SPI_ILAT.TF. The count end: REN set, the received words counted. A build
 * made with REPLY_END=count (OPXY_REPLY_END_COUNT) starts on the count end.
 */
enum wb_spi_reply_end { WB_SPI_END_TF = 0, WB_SPI_END_COUNT = 1 };

#if defined(OPXY_REPLY_END_COUNT) && OPXY_REPLY_END_COUNT
#define WB_SPI_REPLY_END_BUILD WB_SPI_END_COUNT
#else
#define WB_SPI_REPLY_END_BUILD WB_SPI_END_TF
#endif

/*
 * The end s's replies use now; *changed (may be NULL) says whether a reply
 * on the TF end ran out with every word fed and no TF, so that the
 * rest of the run is on the count end (wb_spi_slave_send()). wb_spi_slave_init()
 * puts the build's end back. The set refuses the TF end after a
 * change: it never flips back within a run.
 */
enum wb_spi_reply_end wb_spi_slave_reply_end(const struct wb_spi_slave *s,
					     bool *changed);
bool wb_spi_slave_reply_end_set(const struct wb_spi_slave *s,
				enum wb_spi_reply_end end);

/*
 * A reply session of n bytes (S-13b.044, S-13b.045), its feed the transmit
 * DMA: an array of the n bytes and then the two queued words, started at step
 * 2 of S-13b.044's order, EN set last, then the ready pin handed over (with
 * WB_SPI_SLAVE_SESSION once the master-ready pin reads high).
 *
 * On the TF end (S-13b.083), and in quad I/O, no received word is
 * read: once the DMA's done status has been seen, SPI_IMSK.TF is unmasked,
 * and the reply has ended when SPI_ILAT.TF is read set (polled: no handler).
 * The ready pin is then taken back first, TF masked, TEN cleared, TF
 * acknowledged and the DMA disabled; EN stays set.
 *
 * On the count end the received words are read and counted, two at once
 * when the receive FIFO is full, and the reply has ended when as many have
 * come in as were sent. When a received word was lost to an overrun, so
 * that the count cannot reach the length, it ends once SPI_TWC reads 0 and
 * no word has come for SPI_REPLY_QUIET_US (WB_SPI_OK, recorded as
 * WB_SPI_STAGE_REPLY_OVERRUN).
 *
 * Either end gives up at once on a transmit underrun or collision
 * (WB_SPI_ERR_HW) and when PB15 reads high after it has read low
 * (WB_SPI_TIMEOUT, S-09.013); otherwise only after twice word_timeout_us with
 * nothing moved. Then the ready pin is taken back, TF masked, TEN cleared,
 * the DMA disabled and EN cleared, which empties both FIFOs.
 */
int wb_spi_slave_send(const struct wb_spi_slave *s, const uint8_t *tx,
		      size_t n, unsigned flags, uint32_t start_us);
/* The slave's last failure (struct wb_spi_fault). */
void wb_spi_slave_last_fault(const struct wb_spi_slave *s,
			     struct wb_spi_fault *out);

/*
 * The bridge's descriptor arrays (S-13b.083), in L1: what each channel is
 * given, and where each descriptor's bytes are in memory (mem) and, for a
 * receive through a separate 32-byte buffer, where they are copied to at
 * the part's end (to, else NULL). One array per direction: a CPU has one
 * end of the bridge.
 */
#define WB_SPI_DESC_MAX 40u
struct wb_spi_array {
	struct wb_dma_desc d[WB_SPI_DESC_MAX];
	uint8_t *mem[WB_SPI_DESC_MAX];
	uint8_t *to[WB_SPI_DESC_MAX];
	uint64_t bytes[WB_SPI_DESC_MAX];
	unsigned n;
};
extern struct wb_spi_array wb_spi_tx_array, wb_spi_rx_array;

#if defined(OPXY_TEST_OS)
/*
 * The test OS's q11 measurements (docs/test-os.md, `q11`). One whole transfer
 * of n bytes under ctl (without EN) and clk, in S-13b.040's enable-last order:
 * SPI_CTL 0, SPI_CLK, SPI_CTL = ctl, SPI_STAT cleared, the word count, the
 * other channel off, this channel, then SPI_CTL with EN; then the words, and
 * for a send the wait for TF. Then SPI_CTL = ctl, both channels off, SPI_STAT
 * cleared, SPI_CTL 0 and SPI_CLK as it was, so that the next bridge session's
 * set-up sets EN on the configured value. seen: the or of SPI_STAT, the last
 * SPI_STAT, then core cycles from the write that set EN to the last word moved
 * and to TF (send) or RF (receive) first read set, 0x80000000 if not seen.
 */
int wb_spi_testos_send_last(const struct wb_spi *s, uint32_t ctl, uint32_t clk,
			    const uint8_t *tx, size_t n, uint32_t *seen);
int wb_spi_testos_recv_last(const struct wb_spi *s, uint32_t ctl, uint32_t clk,
			    uint8_t *rx, size_t n, uint32_t *seen);
/* An armed quad listen that has heard nothing for 2 s: PA4 taken back, the
 * controller in single I/O, the listen's bits cleared. True when it was. */
bool wb_spi_testos_listen_lapsed(const struct wb_spi_slave *s);
#endif

#endif /* WAFFLE_DRIVERS_BF70X_SPI_H */

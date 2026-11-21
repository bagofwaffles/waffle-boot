/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09, the master's side: the slave boot stream (S-09.001-S-09.008) and
 * the bridge client (S-09.010-S-09.060). The SPI and GPIO signaling stay
 * behind the ops structs; this module decides what to send and when.
 */
#ifndef WAFFLE_S09_MASTER_H
#define WAFFLE_S09_MASTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s06_clock.h"
#include "waffle/s06_link.h"
#include "waffle/s09_bridge.h"

/* The bounds of S-09.060, as this implementation sets them. */
#define S09_T_BOOT_READY_US S06_MS(500)		/* 200 ms - 1 s */
#define S09_T_INIT_US S06_MS(8000)		/* 6 s - 10 s, from the stream's last
						 * byte to the init request session */
#define S09_T_REPLY_US S06_MS(500)		/* 100 ms - 1 s */
#define S09_T_EXEC_MOUNT_US S06_MS(90000)	/* 60 s - 120 s for 0x08: the middle
						 * while the mount time is unmeasured (Q17) */
#define S09_T_EXEC_US S06_MS(10000)		/* 10 s - 30 s for the others */
#define S09_T_UNMOUNT_US S06_MS(60000)		/* 30 s - 60 s: T_EXEC for 0x09, the
						 * top, above the slave's bound on
						 * the eMMC's shutdown (S-03.011) */
#define S09_T_STREAM_US S06_MS(2000)		/* 1 s - 5 s per stream byte */
#define S09_T_RELEASE_US S06_MS(20000)		/* 10 s - 30 s */
#define S09_T_POWER_US S06_MS(2000)		/* 1 s - 5 s */
/* S-09.013: PC11 high after a session that failed, before the next one:
 * longer than the slave's T_REPLY, by which it gives up a request it was
 * still receiving and a reply it offered */
#define S09_T_QUIET_US (S09_T_REPLY_US + S06_MS(50))

/* ---- the boot link (S-09.001-S-09.007) ---- */

/*
 * ready() and xfer() run for every byte of the stream, so they do nothing
 * but sample the line and move the byte: what a chip-select session needs
 * set up is set up in select(). xfer() returns once its byte has been
 * clocked in full and the link's settle time, if it has one, has passed:
 * the next ready() is the sample that decides the next byte.
 */
struct s09_boot_ops {
	void *ctx;
	bool (*ready)(void *ctx);		/* PA6 reads low */
	void (*select)(void *ctx, bool on);	/* the boot link's chip select */
	uint8_t (*xfer)(void *ctx, uint8_t tx);	/* one byte, full duplex */
	/*
	 * NULL, or with OPXY_BOOT_LINK_FC a stream session's bytes in one
	 * call, each held off by the SPI controller's own ready input on PA6
	 * (S-13b.022); false when a byte was held for longer than T_STREAM.
	 * The opening byte and S-09.007's bytes still go through xfer().
	 */
	bool (*send)(void *ctx, const uint8_t *d, size_t n);
	/*
	 * NULL, or whether the link's driver has failed to move a byte of the
	 * session open now. The stream ends at the first such byte, the slave
	 * absent (S-09.008): a controller that has stopped clocking must not
	 * cost its word timeout for every byte that is left.
	 */
	bool (*failed)(void *ctx);
};

/*
 * How the stream goes, a property of the build: OPXY_BOOT_LINK_FC=2 by DMA
 * under the controller's flow control (waffle/s09_fast.h), the Makefile's
 * default; 1 under flow control, fed by the core; 0 byte by byte, where
 * send() is never called and its code is not in the image. A compile that
 * defines nothing has 0.
 */
#ifndef OPXY_BOOT_LINK_FC
#define OPXY_BOOT_LINK_FC 0
#endif

/* Outcomes of the stream. */
#define S09_BOOT_STREAMED 0	/* streamed to its end */
#define S09_BOOT_BAD_CRC 1	/* S-09.002: absent, no byte sent */
#define S09_BOOT_NOT_READY 2	/* S-09.003 */
#define S09_BOOT_NO_OPEN 3	/* S-09.007 */
#define S09_BOOT_STUCK 4	/* PA6 high past T_STREAM mid-stream (S-09.060), or
				 * the link's driver failed to move a byte */

/* How the stream ended (S-09.006). */
#define S09_END_FINAL 0
#define S09_END_BADSIG 1
#define S09_END_SLOT 2

/*
 * S-09.001-S-09.007: boot the slave from the running slot's stored stream.
 * crc_ok is S-02.016's verdict on the slot. *end tells how a stream ended.
 */
int s09_boot_slave(const struct s09_boot_ops *ops, const struct s06_clock *clk,
		   const uint8_t *slot, size_t len, bool crc_ok, int *end);

/* ---- the bridge (S-09.010-S-09.060) ---- */

struct s09_bus_ops {
	void *ctx;
	bool (*ready)(void *ctx);		/* PA6 reads low */
	bool (*attention)(void *ctx);		/* PC5 reads low */
	void (*select)(void *ctx, bool on);	/* PC11 driven low (on) / high */
	/*
	 * Clock n bytes (a whole number of words) out or in, inside a session,
	 * with S-13b.040's flow control (no word starts while PA6 is high): a
	 * request session's bytes in one send, a reply session's in one or more
	 * receives, its parts (S-09.031-S-09.035). A false return: the transfer
	 * did not move for T_REPLY.
	 */
	bool (*send)(void *ctx, const uint8_t *d, size_t n);
	bool (*recv)(void *ctx, uint8_t *d, size_t n);
	/*
	 * S-13b.040-S-13b.042: 0 the pre-link configuration, 1 the same after
	 * the link. S-09.075: S09_PHASE_RUNG(r), the sends that follow are a
	 * request session's on rung r, until phase 0 again.
	 */
	void (*configure)(void *ctx, int phase);
	/*
	 * The session's last word has moved, before the wait for PA6 to rise
	 * (S-13b.083: a reply session's controller disabled), or NULL.
	 */
	void (*end)(void *ctx);
};

/* S-09.072: an engine for s09_check_word() (s09_master_check_engine()). */
typedef uint32_t (*s09_check_fn)(void *ctx, const uint8_t *d, size_t n);

/*
 * S-09.080, S-13b.084: a quad reply test's reply session on the bus, its n
 * bytes into d in quad I/O at `clock` (S09_QT_FAST, S09_QT_FMODE), the
 * controller in single I/O again after it, as after any reply; out the
 * master's own words at its end (SPI_STAT, the word count, the cycles from
 * EN to the DMA's done status, the DMA's status). False as recv's.
 */
typedef bool (*s09_quad_recv_fn)(void *ctx, uint8_t *d, size_t n, unsigned clock,
				 uint32_t out[4]);

/*
 * S-09.075: the rungs of the link, slowest first. A rung is a link mode
 * (S-09.070) and the master's clock for request sessions.
 */
#define S09_RUNG_PLAIN 0u
#define S09_RUNG_CHECKED 1u		/* single I/O with the check */
#define S09_RUNG_CHECKED_FAST 2u	/* ... at the faster clock */
#define S09_RUNG_QUAD 3u		/* quad I/O */
#define S09_RUNG_QUAD_FAST 4u		/* ... at the faster clock */
#define S09_RUNG_TOP S09_RUNG_QUAD_FAST
#define S09_PHASE_RUNG(rung) (1 + (int)(rung))
/* S-09.075's gate: the plain wire's time per word, 32 bits in single I/O at
 * S-13b.041's pre-link clock (SCLK0 / 4, 25 MHz), unless the board sets its
 * own (s09_master_plain_word_ns()). */
#define S09_PLAIN_WORD_NS 1280u
/* S-09.073: the slave's bound on a trial in which no request comes. */
#define S09_T_TRIAL_US S06_MS(2000)

#define S09_TUNNEL_RX 0x2000u
#define S09_TUNNEL_TX 0x2000u

/* What a request's slot holds (S-09.022). */
#define S09_SLOT_FREE 0
#define S09_SLOT_WAIT 1		/* a deferred request waiting for its reply */
#define S09_SLOT_PULL 2		/* the parked pull */
#define S09_SLOT_DONE 3		/* a reply collected, not yet taken */
#define S09_SLOT_READ 4		/* a chunked read with pieces to come */

struct s09_slot_state {
	uint8_t state;
	uint8_t cmd;
	uint32_t arg1;
	/* the collected reply: result, errno and data (fixed shapes) */
	int32_t result;
	uint32_t err;
	uint8_t *data;		/* where the data goes (caller's buffer) */
	uint32_t data_max;
	uint32_t data_len;
};

/*
 * The link's record for the boot, kept by the master to be read after the
 * fact: how the last negotiation went, rung by rung, and the last
 * S09_REC_FALLS fall-backs of requests, each with where it broke and what
 * the slave reported (S-09.079).
 */
/* Where an exchange broke. */
#define S09_AT_NONE 0u
#define S09_AT_SEND 1u		/* the bus's send failed: the fault words */
#define S09_AT_READY 2u		/* the ready line never came, or never rose */
#define S09_AT_REPLY_START 3u	/* the reply session never started */
#define S09_AT_REPLY_BROKE 4u	/* the reply broke off: the fault words */
#define S09_AT_FIRST_WORD 5u	/* a reply's first word wrong */
#define S09_AT_CHECK 6u		/* a reply's check word wrong */
#define S09_AT_EXEC 7u		/* no reply within T_EXEC */
#define S09_AT_SLAVE 8u		/* the slave's line said it fell back */
#define S09_AT_VALUE 9u		/* a whole reply's value wrong */
#define S09_AT_COUNT 10u

struct s09_fault {
	uint8_t at;		/* S09_AT_... */
	uint32_t words[4];	/* the driver's: rc, status, words left, stage */
	uint32_t more[4];	/* and its second four, where it has them */
};

/* S-09.079: 0x1B's reply. */
struct s09_link_status {
	bool told;		/* it came */
	uint32_t fell;		/* the slave's fall-backs so far */
	uint8_t why, cmd, mode;	/* the last one's cause (S09_WHY_...), ... */
	uint32_t len, got;
	uint32_t words[4];
};

/* A rung's result in a negotiation. */
#define S09_RR_KEPT 0u		/* passed its tests and its gate: kept */
#define S09_RR_ASK 1u		/* 0x19 on trial did not get it */
#define S09_RR_TEST 2u		/* a test failed: pat and len say which */
#define S09_RR_KEEP 3u		/* 0x19 keep did not get it */
#define S09_RR_GATE 4u		/* no gain over the plain wire: left */
#define S09_RR_CHECK 5u		/* the check failed its known answers */
#define S09_RR_NONE 0xFFu	/* the link bench: not tried */
/* S-09.075's gate: the largest test request timed so many times */
#define S09_GATE_TIMES 3u

struct s09_rec_rung {
	uint8_t rung, result;
	uint8_t pat;		/* the failed test: its pattern and length */
	uint32_t len;
	struct s09_fault fault;	/* how its session broke; S09_AT_NONE: its value */
	/* the largest test request's session, three times, as requests will
	 * go, against the plain wire's time for the same words (S-09.075's
	 * gate); zeros when not timed (rung 1, or a verdict kept) */
	uint32_t big_us[S09_GATE_TIMES], plain_us;
	struct s09_link_status st;	/* after a fall-back in it */
};

struct s09_rec_fall {
	uint8_t from, to;	/* the rungs; to S09_REC_ABSENT: the slave absent */
	uint8_t cmd;
	uint32_t len;		/* its payload's bytes */
	uint8_t tries;		/* 0: its first send; else the repeat it was */
	struct s09_fault fault;
	struct s09_link_status st;
};
#define S09_REC_ABSENT 0xFFu
#define S09_REC_RUNGS 8u
#define S09_REC_FALLS 16u

/*
 * A fault of the last link bench's run: a cell's request (kind
 * S09_BF_REQUEST) or a rung's entry (S09_BF_ENTRY, result S09_RR_...),
 * with where it broke, how the link came back (S-09.074's echoes: their
 * time, the last one's failure when none was answered, `to` the plain rung
 * or S09_REC_ABSENT) and what the slave reported (S-09.079).
 */
#define S09_BF_REQUEST 0u
#define S09_BF_ENTRY 1u
struct s09_rec_bench {
	uint16_t no;		/* the run's fault number, from 0 */
	uint8_t kind, rung, result;
	uint32_t len;		/* the request's payload, or the failed test's */
	uint16_t req;		/* the cell's request, from 0 */
	uint8_t to;
	uint32_t fall_us;
	struct s09_fault fault;
	struct s09_fault echo;
	struct s09_link_status st;
};
/* The run's first S09_REC_BENCH_FIRST faults, and its last ones in the rest. */
#define S09_REC_BENCH 16u
#define S09_REC_BENCH_FIRST 12u

struct s09_record {
	uint32_t falls;		/* fall-backs of requests in the boot */
	struct s09_rec_fall fall[S09_REC_FALLS];	/* the last, by falls */
	uint32_t negotiations;
	uint8_t nrungs;		/* rungs tried in the last one, in order */
	struct s09_rec_rung rung[S09_REC_RUNGS];
	uint8_t kept;		/* the rung it ended on */
	uint32_t bench_faults;	/* the last link bench run's faults */
	struct s09_rec_bench bench[S09_REC_BENCH];
	/* how far a reader (the board's log) has shown it; the master leaves
	 * these alone */
	uint32_t shown_negotiations, shown_falls;
};

/*
 * Why the slave was marked absent (S-09.008), in any mode: the request the
 * bridge was serving when it happened (the caller's, or the start-up's
 * init, echo or pull, the service's push or poll, the release) and its
 * arg0; where its exchange broke (S09_AT_..., S09_AT_NONE when no exchange
 * broke: a bound or a value), with S09_LOST_IN_POLL set when it broke in a
 * poll or a pull made while serving it; and the driver's four fault words
 * for a send or receive that failed (S09_AT_SEND, S09_AT_REPLY_BROKE),
 * zeros else. cmd 0: marked absent from outside (s09_master_set_absent()).
 */
#define S09_LOST_IN_POLL 0x80u
struct s09_lost {
	uint8_t cmd;
	uint8_t at;
	uint32_t arg0;
	uint32_t fault[4];
};

/*
 * S-09.081: the quad data replies of this start: where they stand, why
 * they were not taken up or were dropped, the quad reply test that came
 * first, and the data sessions taken whole.
 */
#define S09_QD_OFF 0u		/* not tried */
#define S09_QD_NOT 1u		/* tried, not taken up: why says why */
#define S09_QD_ON 2u		/* taken up: the slave in mode 3 */
#define S09_QD_DROPPED 3u	/* taken up, then dropped for the start: why */
#define S09_QD_ENDED 4u		/* taken up, then ended with the link's lowering */
#define S09_QW_NONE 0u
#define S09_QW_NO_RECV 1u	/* the bus has no quad receive */
#define S09_QW_TEST 2u		/* the quad reply test failed */
#define S09_QW_NO_RUNG 3u	/* the link is on no checked rung */
#define S09_QW_REFUSED 4u	/* 0x19 answered another mode: a slave without it */
#define S09_QW_LOST 5u		/* 0x19's exchange did not get through */
#define S09_QW_DATA 6u		/* a data session broke, or was short */
#define S09_QW_DATA_CHECK 7u	/* a data session's check word was wrong */
#define S09_QW_FELL 8u		/* another fall-back while it was on */
#define S09_QW_ASKED 9u		/* dropped by the caller (s09_master_qdata_off()) */
#define S09_QW_STRUCK 10u	/* dropped earlier in this start */
#define S09_QW_COUNT 11u
/* S-09.081: the quad reply test's cells before a take-up */
#define S09_QD_CELLS 2u
struct s09_qdata_rec {
	uint8_t state, why;		/* S09_QD_..., S09_QW_... */
	/* the test: trials sent and whole, the first failure (S09_QX_...)
	 * and its cell's length, and its time */
	uint16_t test_sent, test_whole;
	uint8_t test_first;
	uint32_t test_len, test_us;
	/* data sessions taken whole, their bytes, and those that failed */
	uint32_t replies, bytes, failed;
	struct s09_fault fault;		/* how the one that dropped it broke */
};

struct s09_store_ops;

struct s09_master {
	const struct s09_bus_ops *bus;
	const struct s06_clock *clock;
	/* s09_master_local(): the store on this CPU, NULL for the bridge */
	const struct s09_store_ops *local;
	bool absent;		/* S-09.008 */
	/* a session let go before its end, and the wait before the next one
	 * then (S-09.013; S09_T_QUIET_US) */
	bool cut;
	uint32_t t_quiet_us;
	/* S-09.013 rule 4: the request being served failed in the plain mode
	 * with the slave kept (EIO); the last poll's reply's first word, when
	 * it came */
	bool io_failed;
	bool poll_seen;
	uint32_t poll_word;
	/* the request being served and whether a poll for it is under way,
	 * and what s09_master_lost() reports */
	uint8_t cur_cmd;
	uint32_t cur_arg0;
	bool in_poll;
	struct s09_lost lost;
	bool linked;		/* S-09.015 */
	bool released;		/* S-09.052, value 0 */
	bool shutdown;		/* s09_master_shutdown() */
	struct s09_slot_state slot[S09_SLOTS];
	bool pull_out;		/* S-09.045: a pull outstanding */
	bool pull_parked;
	uint8_t pull_slot;
	bool overflow_seen;	/* a pull reply reported dropped bytes */
	bool busy;		/* inside s09_master_service() */
	/* channel 0, slave to master */
	uint8_t rx[S09_TUNNEL_RX];
	size_t rx_r, rx_n;
	/* channel 0, master to slave, not yet pushed */
	uint8_t tx[S09_TUNNEL_TX];
	size_t tx_n;
	/* A file write left running (s09_master_write_begin()). */
	struct {
		bool active;	/* its reply has not been collected */
		struct s09_req req;
		uint32_t seq;	/* its number (S-09.076) */
		int32_t result;	/* the last one's, once collected */
		uint32_t err;
	} bg;
	/* S-09.075: the rung requests go on and the best one still to try */
	uint8_t rung, ceiling;
	/* S-09.075's gate: the rungs that passed it in this boot (a bit each),
	 * the plain wire's time per word in ns, and the last request
	 * session's own time in us */
	uint8_t gate_ok;
	uint32_t plain_word_ns;
	uint32_t session_us;
	/* S-09.074: a fall-back since the last 0x19 answered: the slave may
	 * take nothing but the link's own requests and 0x1B */
	bool unsettled;
	uint32_t fell;		/* fall-backs to the plain rung */
	/* the link's record; where the next fall-back's note goes, and what
	 * broke last */
	struct s09_record rec;
	struct s09_rec_fall *falling;
	struct s09_rec_rung *trying;
	bool negotiating, quiet;
	struct s09_fault why;
	/* the last fall-back's echoes: their time, the last one's failure */
	uint32_t fall_us;
	struct s09_fault echo_why;
	bool echo_busy;
	/* the link bench's 0x1B answer after a fall-back, NULL outside it */
	struct s09_link_status *bench_st;
	/* where a send's fault words come from (the board's driver) */
	void (*fault_words)(void *ctx, uint32_t out[4]);
	void (*fault_more)(void *ctx, uint32_t out[4]);
	void *fault_ctx, *more_ctx;
	_Alignas(4) uint8_t stat[S09_STATUS_LEN + 4u];	/* 0x1B's reply */
	/* S-09.076: the last number given */
	uint32_t seq;
	/* S-09.080: the bus's quad reply test receive, NULL for none */
	s09_quad_recv_fn quad_recv;
	void *quad_ctx;
	/* S-09.081: quad data replies taken up (the slave in mode 3), not to
	 * be taken up again in this start, a data session's failure for the
	 * fall-back to come (S09_QW_...), the data session the reply being
	 * taken announced (its bytes and check word), and the record */
	bool qdata, qdata_struck;
	uint8_t qd_fault;
	bool qd_on;
	uint32_t qd_len, qd_want;
	struct s09_qdata_rec qd;
	/* S-09.072: the check's engine, NULL for the table; given up */
	s09_check_fn check;
	void *check_ctx;
	bool soft_check;
	bool check_bad;		/* its known answers were wrong at the last raise */
	/* S-09.079 cause 9 read in a status; and told (s09_master_end_changed()) */
	bool end_seen, end_told;
	/* one request session, as the bus sends it (S-13b.083) */
	_Alignas(4) uint8_t buf[S09_HDR + 128u * S09_SECTOR + 16u];
	/* a request with no payload: buf keeps the last request with a
	 * payload until the next one, for a repeat (S-09.076) */
	_Alignas(4) uint8_t small[S09_HDR + S09_CHECK_LEN + 4u];
	/* every reply, whole, before any of it is used: an immediate one in
	 * imm, so that a link test built in rep is there after its reply */
	_Alignas(4) uint8_t rep[S09_HDR + 128u * S09_SECTOR + 16u];
	_Alignas(4) uint8_t imm[12];	/* and its check (S-09.072) */
};

/* S-09.023, S-09.072: a session of any request this master sends fits buf,
 * a reply rep. */
_Static_assert(sizeof(((struct s09_master *)0)->buf) >=
	       S09_HDR + S09_MAX_SECTORS * S09_SECTOR + S09_CHECK_LEN &&
	       sizeof(((struct s09_master *)0)->buf) >= S09_HDR + S09_WRITE_MAX + S09_CHECK_LEN &&
	       sizeof(((struct s09_master *)0)->buf) >= S09_HDR + S09_TEST_MAX + S09_CHECK_LEN &&
	       sizeof(((struct s09_master *)0)->buf) >= S09_HDR + 0x4Cu,
	       "a request session overruns the master's buffer");
_Static_assert(sizeof(((struct s09_master *)0)->rep) >=
	       S09_HDR + S09_MAX_SECTORS * S09_SECTOR + 4u &&
	       sizeof(((struct s09_master *)0)->rep) >= 8u + S09_TUNNEL_MAX + 4u &&
	       sizeof(((struct s09_master *)0)->rep) >= 8u + S09_READ_CHUNK + 4u &&
	       sizeof(((struct s09_master *)0)->imm) >= 8u + 4u,
	       "a reply session and its check overrun the master's buffer");

void s09_master_init(struct s09_master *m, const struct s09_bus_ops *bus,
		     const struct s06_clock *clock);

/*
 * S-09.072: the check of the master's requests computed by fn (the board's
 * engine) instead of the table. When a request whose check came from it
 * does not get through and the table gives another check, the engine is
 * not used again in this boot.
 */
void s09_master_check_engine(struct s09_master *m, s09_check_fn fn, void *ctx);

/*
 * S-13b.060: the client of a store on its own CPU (a device without
 * second-cpu, whose main CPU drives the eMMC). Every request then goes
 * straight to ops through s09_store_call(), with no bus, framing,
 * numbering, polls, rungs or tunnel; the store is never absent by the
 * link. The link's own operations do nothing and succeed (start, the
 * rungs, quad data, service, settled), the link tests fail as for an
 * absent slave (bench, quad test), a release is not made (-1) and nothing
 * is sent to any CPU (the tunnel has no room, s09_power_off() sends
 * nothing).
 */
void s09_master_local(struct s09_master *m, const struct s09_store_ops *ops);

/* The slave is absent from now on (S-09.008), e.g. after s09_boot_slave(). */
void s09_master_set_absent(struct s09_master *m);

/*
 * A shutdown has begun (S-04.063). From now on a 0x09 whose T_UNMOUNT runs
 * out fails alone with EIO (S-09.060): no recovery, and the slave is not
 * absent, so the power-off request still goes. Its slot stays taken, for
 * a reply the slave may still give.
 */
void s09_master_shutdown(struct s09_master *m);
/* Why the slave is absent (struct s09_lost); false, *out zeros, when it is
 * not. */
bool s09_master_lost(const struct s09_master *m, struct s09_lost *out);

/* S-09.015, S-09.017: init, speed switch, echo, the first pull. False: the
 * slave is absent. */
bool s09_master_start(struct s09_master *m);

/*
 * One bridge request and its reply (immediate or deferred, S-09.030-
 * S-09.032). payload/plen: the request's payload. data/max: where the reply's
 * data goes (S-09.032), *got its length. Returns the reply's result (for an
 * immediate request its value) and sets *err; with the slave absent, or when
 * a bound of S-09.060 runs out, -1 and ENODEV (S-09.008).
 */
int32_t s09_master_call(struct s09_master *m, const struct s09_req *req,
			const uint8_t *payload, uint32_t plen, uint8_t *data,
			uint32_t max, uint32_t *got, uint32_t *err);

/*
 * S-09.035: read n bytes from fd in pieces of 0x1000. Returns the sum of the
 * pieces, or a failed piece's result and *err.
 */
int32_t s09_master_read(struct s09_master *m, uint32_t fd, uint8_t *buf,
			uint32_t n, uint32_t *err);

/*
 * S-09.075: bring the link to the best rung above rung 1, not above
 * `ceiling`, that passes its tests (S-09.071) and its gate (S-09.073,
 * S-09.075). With none, rung 1 (checked at plain's
 * wire, no gate); the plain mode only when rung 1's own tests fail. Nothing
 * when it is there already. False: the slave is absent.
 */
bool s09_master_link_fast(struct s09_master *m, unsigned ceiling);
/* S-09.072: the check failed its known answers at the last attempt to
 * leave the plain mode, so the link stayed where it was. */
bool s09_master_check_bad(const struct s09_master *m);
/*
 * S-09.078: the link lowered to the plain mode with 0x19, or by S-09.074's
 * echoes when that does not get through. Nothing when it is plain. The
 * rungs not failed may be raised to again. False: the slave is absent.
 */
bool s09_master_link_plain(struct s09_master *m);
/*
 * Where a failed send's or receive's four fault words come from (the
 * board's driver: rc, status, words left, stage), for the link's record;
 * NULL, zeros.
 */
void s09_master_fault_source(struct s09_master *m,
			     void (*fn)(void *ctx, uint32_t out[4]), void *ctx);
/* ... and the driver's second four words, where it has them. */
void s09_master_fault_more(struct s09_master *m,
			   void (*fn)(void *ctx, uint32_t out[4]), void *ctx);
/* S-09.075's gate: the plain wire's time per word on this board. */
void s09_master_plain_word_ns(struct s09_master *m, uint32_t ns);
/* The link's record for the boot. */
const struct s09_record *s09_master_record(const struct s09_master *m);
/*
 * S-09.074: no fall-back since the last 0x19 the slave answered, so the
 * slave takes any plain request. A plain request of a caller's settles it
 * first; false only while nothing has yet, or when settling failed and the
 * slave is absent.
 */
bool s09_master_settled(const struct s09_master *m);
/*
 * S-09.079 cause 9, S-13b.083: true once, the first time a link status
 * showed that the slave changed to ending its single-I/O replies by their
 * word count. It does not change back within a run, so later statuses
 * that still show it, or another cause after it, tell nothing new.
 */
bool s09_master_end_changed(struct s09_master *m);

/*
 * The link bench, a developer's link test (S-09.075): n link tests of each
 * length of s09_bench_len in each cell, in one pass. On each rung from 1 to
 * the top, entered as a negotiation enters it (a 0x19 trial, its tests, the
 * keep; no gate), the tests; then, lowered and settled, the plain mode at
 * the lengths plain requests have. Every session goes as every request's
 * does (S-13b.083). Nothing else is sent but the fall-back's echoes, 0x19
 * and 0x1B. A fault is counted
 * where it broke and kept in the record (struct s09_rec_bench), the link
 * brought back as S-09.074 brings it (the bus's plain set-up first, which
 * resets its controller), and the length goes on, entering the rung again,
 * until S09_BENCH_FAULTS faults: its other requests are then not sent. A
 * rung that cannot be entered is left for the next. limit_us bounds the run
 * (cut then set); a lost slave ends it; either way end_* says where. Ends
 * in the plain mode with the slave told so (settled says whether that
 * held), the boot's struck rungs and gate verdicts as they were; fell is
 * the run's own fall-backs, which the boot's count keeps too. False: the
 * slave is absent.
 */
#define S09_BENCH_LENS 7u
#define S09_BENCH_FAULTS 2u
/* S-09.023: the plain mode's largest request, a 128-sector write */
#define S09_BENCH_PLAIN_MAX (S09_MAX_SECTORS * S09_SECTOR)
extern const uint32_t s09_bench_len[S09_BENCH_LENS];
struct s09_bench_cell {
	uint16_t sent, passed;
	uint32_t us;			/* the passed requests' time in all */
	uint16_t at[S09_AT_COUNT];	/* faults by where (S09_AT_...) */
};
struct s09_bench {
	uint16_t n;			/* requests per length and rung */
	uint32_t limit_us;		/* the run's bound; 0 none */
	void (*between)(void *ctx);	/* after each request (USB) */
	void *ctx;
	/* by rung: the last entry's result (S09_RR_KEPT: entered,
	 * S09_RR_NONE: not tried) and the times entered */
	uint8_t entry[S09_RUNG_TOP + 1u];
	uint8_t entries[S09_RUNG_TOP + 1u];
	struct s09_bench_cell cell[S09_RUNG_TOP + 1u][S09_BENCH_LENS];
	uint32_t t0, fell;
	bool cut, settled;
	/* where a cut or a lost slave ended it (stopped; clear when it ran to
	 * its end): the rung, the length and the request */
	bool stopped;
	uint8_t end_rung;
	uint32_t end_len;
	uint16_t end_req;
};
bool s09_master_link_bench(struct s09_master *m, struct s09_bench *b);
/*
 * S-09.080, S-13b.084: a developer's quad reply test, one cell: n test
 * replies of len bytes, each armed (0x1C ARM), sent by the slave in quad I/O
 * under candidate cand (with REN when ren) and taken by the master at clock
 * (S09_QT_FAST: SCLK0 / 2, S09_QT_FMODE: fast mode) through the receive
 * s09_master_quad_recv() gave, then the slave's record fetched (0x1C RECORD), all in the plain mode with nothing else sent but
 * S-09.013 rule 4's echoes, polls and 0x1B. Trial i has pattern i mod
 * S09_QT_PATS and seed i. The link is lowered and the slave settled first
 * (S-09.078, S-09.074); a trial whose session fails is S-09.013 rule 4's,
 * and the cell goes on. limit_us bounds the cell (cut). False: the slave is
 * absent. A bus or slave with no test, a refused arm, or arguments out of
 * range: refused, nothing sent but the arm.
 */
#define S09_QT_FAST 0x1u
#define S09_QT_FMODE 0x2u
#define S09_QT_CLOCKS 4u
/* What failed first in a cell. */
#define S09_QX_OK 0u
#define S09_QX_BROKE 1u		/* the session did not complete */
#define S09_QX_LAST 2u		/* only the last word was wrong */
#define S09_QX_WORD 3u		/* another word was wrong */
#define S09_QX_NO_FLAG 4u	/* whole, but the candidate's flag missing */
#define S09_QX_NO_RECORD 5u	/* the slave's record did not come */
struct s09_qt_cell {
	uint8_t cand, clock;
	bool ren;
	uint32_t len;
	uint16_t n;
	uint32_t limit_us;
	void (*between)(void *ctx);	/* after each trial (USB) */
	void *ctx;
	/* the trials */
	uint16_t sent, whole, flag, missing, last_wrong, word_wrong, broke;
	uint16_t no_record;
	uint16_t ends[S09_QE_COUNT];	/* by the slave's end (S09_QE_...) */
	uint32_t dmin, dmax;		/* the flag's delay after the last feed */
	uint8_t lines;			/* the data lines of every wrong bit */
	/* SPI_STAT at the end, the master's or-ed and and-ed over the
	 * replies that came whole, the slave's or-ed over its records */
	uint32_t m_stat_or, m_stat_and, s_stat_or;
	bool refused, cut, settled;
	uint8_t why;			/* refused: the arm's value */
	/* the first trial that failed: which, how, the slave's record and
	 * the master's own words */
	uint8_t first;			/* S09_QX_... */
	uint16_t first_no;
	uint32_t first_rec[S09_QT_REC_WORDS];
	uint32_t first_m[4];
	uint32_t first_word;		/* its first wrong word's index */
};
bool s09_master_quad_test(struct s09_master *m, struct s09_qt_cell *c);
/* The bus's receive for it; without one every cell is refused. */
void s09_master_quad_recv(struct s09_master *m, s09_quad_recv_fn fn, void *ctx);

/*
 * S-09.081: quad data replies for the requests to come. In the plain mode,
 * the quad reply test (S-09.080) of each length a sector read's data takes
 * at the clock the data sessions use (S09_QD_CLOCK), candidate TF fed by
 * DMA, REN clear; only when every trial came whole with the candidate's
 * flag, the link raised as s09_master_link_fast() raises it, and on a
 * checked rung (mode 1, kept) 0x19 asks to keep mode 3. Taken up when its
 * value is 3. Nothing when it was dropped earlier in this start. False:
 * the slave is absent.
 */
#define S09_QD_CLOCK (S09_QT_FAST | S09_QT_FMODE)
bool s09_master_link_qdata(struct s09_master *m, unsigned ceiling);
/* S-09.081: quad data replies dropped for the rest of the start, the link
 * left on its rung in mode 1 (0x19 keep mode 1); when that does not get
 * through, by the fall-back and a raise to ceiling. False: the slave is
 * absent. */
bool s09_master_qdata_off(struct s09_master *m, unsigned ceiling);
/* S-09.081: the record of this start's quad data replies. */
const struct s09_qdata_rec *s09_master_qdata(const struct s09_master *m);

/* The rung, the fall-backs so far, and a file write's most bytes on this
 * rung (S-09.023). */
unsigned s09_master_rung(const struct s09_master *m);
uint32_t s09_master_fell(const struct s09_master *m);
uint32_t s09_master_write_max(const struct s09_master *m);

/*
 * A file write (0x2B, n bytes of p to fd) sent and left running: the slave
 * executes it while the caller works. p is copied. One at a time: the one before
 * is collected first, and so it is by every other request. False, with
 * *err, when nothing was started.
 */
bool s09_master_write_begin(struct s09_master *m, uint32_t fd,
			    const uint8_t *p, uint32_t n, uint32_t *err);
/* The result of the write left running, as s09_master_call() gives one;
 * with none running, the last one's. */
int32_t s09_master_write_end(struct s09_master *m, uint32_t *err);

/*
 * Keep the bridge going between requests: push queued channel-0 bytes,
 * and poll while PC5 is low (collecting the parked pull's data, S-09.045).
 */
void s09_master_service(struct s09_master *m);

/*
 * S-09.050-S-09.052: the release, always in the plain mode: the link is
 * lowered first (S-09.078). Returns the reply's value: 0 (the
 * master makes no further request), 1 (no OS: the bridge stays), or -1 for
 * any other value or no reply within T_RELEASE (then the slave is absent).
 */
int s09_master_release(struct s09_master *m);
/* S-09.082: a file start's release, with the file's length (not 0) and the
 * CRC-32 of the master's copy as its arguments; returns as above. */
int s09_master_release_file(struct s09_master *m, uint32_t len, uint32_t crc);

/* The CPU link's port (S-09.045): an S-06 link over channel 0 of the tunnel. */
extern const struct s06_port_ops s09_tunnel_port;

/* ---- CPU-link messages (S-09.046, S-09.047) ---- */

/* The 0xBD clock message: sent on the first call and when secs changes. */
struct s09_clock_msg {
	bool sent;
	uint32_t last;
};

void s09_clock_message(struct s09_clock_msg *c, struct s06_node *node,
		       unsigned link, uint32_t secs);

/*
 * S-09.046: the seconds of an RTC count (S-04.070's encoding, its two
 * skipped days restored) since 1970-01-01 00:00, with no offset
 * (S-16.063). S-09.046: counted from the decoded date,
 * not the register's days field; the register's last date (2059) fits in
 * 32 bits.
 */
uint32_t s09_clock_seconds(uint32_t clk);

/*
 * s09_clock_message() of an RTC count. The board calls it when held mode
 * starts and on each pass of held mode's loop (S-09.046), and right after
 * an MTP DateTime set has written the register (S-10b.063), with the
 * count read back: the slave is told the new time at once.
 */
void s09_clock_rtc(struct s09_clock_msg *c, struct s06_node *node,
		   unsigned link, uint32_t clk);

/*
 * S-09.047: send 0x31 {14} and wait for the slave's 0x32 at most T_POWER,
 * servicing the bridge. Returns true when it came. With the slave absent
 * nothing is sent (S-09.008).
 */
bool s09_power_off(struct s09_master *m, struct s06_node *node, unsigned link);

#endif

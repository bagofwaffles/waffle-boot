/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09, the slave's side: the bridge server (S-09.011, S-09.016,
 * S-09.020-S-09.043, S-09.051-S-09.053) and its CPU-link answers
 * (S-09.046).
 *
 * The server is driven by sessions: the bus driver receives a request
 * session of s09_slave_rx_len() bytes and hands the first
 * s09_slave_rx_keep() of them to s09_slave_request(); the rest, if any
 * (S-09.037), it receives and discards in pieces of any size, so that the
 * next header is read where the master sent it. When s09_slave_reply() has
 * bytes, the next session is that reply. PB2
 * follows s09_slave_attention(). The eMMC and file operations themselves
 * (S-03, S-12) are behind struct s09_store_ops.
 */
#ifndef WAFFLE_S09_SLAVE_H
#define WAFFLE_S09_SLAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s06_link.h"
#include "waffle/s09_bridge.h"
#include "waffle/testos.h"
#include "waffle/devhost.h"

struct s09_store_ops {
	void *ctx;
	uint32_t (*erase_group)(void *ctx);		/* 0x03, S-03 */
	uint32_t (*boot_size)(void *ctx);		/* 0x05, S-03.008 */
	OPXY_TESTOS(uint32_t (*testos_row)(void *ctx, uint8_t cmd, uint32_t arg0, uint32_t arg1);)
#if !defined(OPXY_TEST_OS)
	/* S-10c.110: a slave register read (0x0E) or write (0x0D) for a
	 * developer hand-over, inside opxy_dev_mem_ok(). */
	uint32_t (*dev_cmd)(void *ctx, uint8_t cmd, uint32_t addr, uint32_t val);
#endif
	/*
	 * S-09.040: an eMMC command (0x04, 0x11-0x16, 0x18) on the partition
	 * the last select chose. data: the sectors read or written (n bytes).
	 * Returns 0 on success, nonzero when it failed after S-03's retries.
	 */
	int32_t (*emmc)(void *ctx, uint8_t cmd, uint32_t arg0, uint32_t arg1,
			uint8_t *data, uint32_t n);
	/*
	 * S-09.041: a file command on the /yaffs2 volume (S-12). path and
	 * path2 (rename) are NUL-terminated, relative ones already under
	 * /yaffs2/ (S-16.117); data/n is 0x2B's payload; out receives the
	 * command's record (52, 12, 268 or 8 bytes). Returns the result and
	 * sets *err as S-09.041's table says.
	 */
	int32_t (*file)(void *ctx, const struct s09_req *r, const char *path,
			const char *path2, const uint8_t *data, uint32_t n,
			uint8_t *out, uint32_t *err);
	/* S-09.035: one piece: read until n bytes, the end of file or failure. */
	int32_t (*read)(void *ctx, uint32_t fd, uint8_t *buf, uint32_t n,
			uint32_t *err);
	/* S-09.051: stage and check the slave's own OS: 0 accepted, 1 no OS. */
	uint32_t (*os_status)(void *ctx);
	/* S-09.036: the cycle counter, and an adjustment of it. */
	uint64_t (*cycles)(void *ctx);
	void (*adjust)(void *ctx, int64_t delta);
};

#define S09_SLAVE_TUNNEL 0x2000u
#define S09_SLAVE_SMALL (S09_HDR + S09_READ_CHUNK + 8u)
/* a sector read's reply, and its check outside the plain mode (S-09.072) */
#define S09_SLAVE_BIG (12u + S09_MAX_SECTORS * S09_SECTOR + 4u)
/* The eMMC driver's DMA sees sector data at this alignment in every build:
 * a reply's data start after its 12-byte header, a write's after its
 * request's, so each buffer has S09_DMA_LEAD bytes before that header. */
#define S09_DMA_ALIGN 32u
#define S09_DMA_LEAD (S09_DMA_ALIGN - 12u)

/* A slot's state on the slave. */
#define S09_SS_FREE 0
#define S09_SS_RUN 1		/* outstanding, not finished */
#define S09_SS_DONE 2		/* finished, in the queue */
#define S09_SS_PULL 3		/* a parked pull */
#define S09_SS_WAIT_BIG 4	/* a sector read waiting for the big buffer */

struct s09_sslot {
	uint8_t state;
	struct s09_req req;
	uint32_t received;	/* S-09.035: R */
	bool big;		/* its reply is in the big buffer */
	uint32_t len;
	uint8_t small[S09_SLAVE_SMALL];
};

struct s09_slave {
	const struct s09_store_ops *ops;
	bool linked;		/* S-09.016 */
	uint8_t mode;		/* S-09.070: how the next request comes */
	bool trial;		/* that mode is on trial (S-09.073) */
	/* S-09.074: fell back; PB2 low, and nothing but a plain echo is
	 * executed until one has arrived whole */
	bool fell_notice;
	/* ... the recovery echo taken: until a 0x19 with arg1 0 is, nothing
	 * else but it, the recovery echo or the init either */
	bool recovering;
	uint32_t fell;		/* fall-backs so far */
	/* S-09.079: the last fall-back's cause (S09_WHY_...), the command,
	 * payload length and mode of the request it met, the bytes its session
	 * brought, and the receive's fault words; and where those come from */
	uint8_t why, why_cmd, why_mode;
	uint32_t why_len, why_got;
	uint32_t why_words[4];
	/* S09_WHY_REPLY_END not yet reported by a 0x1B: it stays the cause */
	bool why_held;
	void (*fault_words)(void *ctx, uint32_t out[4]);
	void *fault_ctx;
	/*
	 * S-09.076: what the slave keeps for repeats, number 0: none.
	 * The last numbered request other than a poll: its number, its check
	 * word, its command, and its reply (an immediate or pull reply, kept
	 * with its check; or a deferred request's finished reply once a poll
	 * has handed it out, kept in np_buf or, a sector read's, in big).
	 * Kept until the next new numbered request other than a poll.
	 */
	uint32_t np_seq, np_check;
	uint8_t np_cmd, np_slot;
	bool np_deferred;	/* its reply is a finished one, in its slot */
	bool np_handed;		/* ... and a poll has handed it out */
	bool np_big;		/* ... from big, which it holds */
	bool replay;		/* that reply queued again for a poll */
	size_t np_len;
	uint8_t np_buf[S09_SLAVE_SMALL + 4u];
	/* The last numbered poll, when it was the last numbered request: its
	 * number, its check word and the reply it gave, with its check. */
	uint32_t p_seq, p_check;
	const uint8_t *p_reply;
	size_t p_len;
	bool big_held;		/* big holds a reply kept for a repeat */
	/* the reply to an unnumbered request, 0x1B's the longest */
	uint8_t uout[S09_STATUS_LEN + 4u];
	/* S-09.072: the check's engine, NULL for the table */
	uint32_t (*check)(void *ctx, const uint8_t *d, size_t n);
	void *check_ctx;
	bool released;		/* S-09.052: value 0 sent */
	bool release_queued;	/* the next reply session is a release's */
	bool release_sent;	/* ... and has been handed out */
	uint32_t release_value;
	bool clock_second;	/* S-09.036: the 8-byte session is next */
	struct s09_req clock_req;
	uint64_t clock_sample;
	struct s09_sslot slot[S09_SLOTS];
	uint8_t queue[S09_SLOTS];	/* finished, oldest first */
	uint8_t nqueue;
	bool big_busy;
	/* A sector read's reply, built at big_room + S09_DMA_LEAD
	 * (s09_slave_big()): its data, at 12 more, go straight to the eMMC
	 * driver's DMA, on a 32-byte boundary. */
	_Alignas(32) uint8_t big_room[S09_DMA_LEAD + S09_SLAVE_BIG];
	/* the next reply session */
	uint8_t out[S09_SLAVE_SMALL];
	const uint8_t *reply;
	size_t reply_len;
	/* channel 0: received from pushes, and to send in pulls */
	uint8_t rx[S09_SLAVE_TUNNEL];
	size_t rx_r, rx_n;
	uint8_t tx[S09_SLAVE_TUNNEL];
	size_t tx_n;
	bool dropped;		/* receive bytes dropped since the last pull reply */
	char path[2][S09_TUNNEL_MAX + 16u];
};

void s09_slave_init(struct s09_slave *s, const struct s09_store_ops *ops);
/* The big reply buffer: S09_SLAVE_BIG bytes, its data at + 12 aligned to
 * S09_DMA_ALIGN. */
/* cppcheck-suppress constParameterPointer ; the buffer it returns is written through */
static inline uint8_t *s09_slave_big(struct s09_slave *s)
{
	return s->big_room + S09_DMA_LEAD;
}

/*
 * S-09.016, S-09.021, S-09.023, S-09.036, S-09.037: how many bytes the
 * request session that begins with this 12-byte header carries for the
 * slave: the header and pad4 of the payload (12 alone for a header that is
 * ignored), and for 0x13 or 0x16 with a count outside 1-128 the header and
 * arg1 x 512 bytes. For the second session of a clock sync, call with hdr
 * NULL: 8.
 */
uint64_t s09_slave_rx_len(const struct s09_slave *s, const uint8_t *hdr);

/*
 * The leading bytes of that session the slave needs: s09_slave_rx_len()
 * less the bytes it discards (S-09.037), at most S09_HDR + 0x10000.
 */
size_t s09_slave_rx_keep(const struct s09_slave *s, const uint8_t *hdr);

/*
 * S-09.021: the bytes a master sends in the same session after the
 * s09_slave_rx_len() of the request, which the slave receives and
 * discards: 8 for a padded payload length of 0x44, 4 for 0x48 (the
 * master's 0x4C), else 0. They are not part of the request.
 */
uint32_t s09_slave_rx_trail(const struct s09_slave *s, const uint8_t *hdr);

/* A request session's bytes: at least the s09_slave_rx_keep() leading ones. */
void s09_slave_request(struct s09_slave *s, const uint8_t *d, size_t n);

/* The reply session to send next, if any; it is consumed by the call. */
bool s09_slave_reply(struct s09_slave *s, const uint8_t **d, size_t *n);

/*
 * S-09.051, S-09.052, S-05.020: the release hook. True once for each
 * release whose reply session s09_slave_reply() has handed out, with the
 * reply's value; the board calls it after it has sent each reply session.
 * Value 0: the server takes no further header (s09_slave_rx_len() keeps
 * the header alone), and the board stops serving and hands over. Value 1:
 * the server goes on serving as before. Any other call returns false.
 */
bool s09_slave_take_release(struct s09_slave *s, uint32_t *value);

/* S-09.031: PB2 low while a finished reply waits; S-09.074: and from a
 * fall-back to the plain mode until a plain echo has arrived whole. */
bool s09_slave_attention(const struct s09_slave *s);

/* S-09.070: the link mode the next request session comes in, and whether
 * it is on trial. */
unsigned s09_slave_mode(const struct s09_slave *s);
bool s09_slave_on_trial(const struct s09_slave *s);
/*
 * S-09.074: outside the plain mode, a request session that did not come
 * whole (the board's receive failed or ran out of time), or a trial in
 * which nothing came for T_TRIAL: the slave is back in the plain mode for
 * the rest of the boot unless the master asks again, and executes nothing
 * but a plain echo until one has arrived whole. Nothing in the plain mode.
 * s09_slave_link_fault(): the receive failed (S09_WHY_RECEIVE), before any
 * header came; s09_slave_rx_failed(): after hdr came, got bytes in; both
 * take the fault words from the hook. s09_slave_trial_lapsed(): a trial
 * in which nothing came for T_TRIAL (nothing when not on trial).
 */
void s09_slave_link_fault(struct s09_slave *s);
void s09_slave_rx_failed(struct s09_slave *s, const uint8_t *hdr, size_t got);
void s09_slave_trial_lapsed(struct s09_slave *s);
/*
 * S-09.079 cause 9, S-13b.083: a reply of n bytes to cmd, fed whole, whose
 * end the board's driver did not see, so that it ends its single-I/O
 * replies by their word count for the rest of the run. Recorded as the
 * last cause; the mode does not change and no fall-back is counted. It
 * stays the cause until a 0x1B has reported it: the fall-back that such a
 * reply brings is counted in n but does not replace it (S-13b.083).
 */
void s09_slave_reply_end_changed(struct s09_slave *s, uint8_t cmd, size_t n);
/* S-09.079: where a failed receive's four fault words come from (the
 * board's driver: rc, status, words left, stage); NULL, zeros. */
void s09_slave_fault_source(struct s09_slave *s,
			    void (*fn)(void *ctx, uint32_t out[4]), void *ctx);

/* The slave's CPU link port (channel 0 of the tunnel). */
extern const struct s06_port_ops s09_slave_tunnel_port;

/* S-09.046: the slave's answers on the CPU link. */
struct s09_cpu_ops {
	void *ctx;
	void (*power_off)(void *ctx);		/* S-04.025, after the 0x32 */
	void (*clock)(void *ctx, uint32_t secs);	/* S-04's clock */
	void (*common)(void *ctx, const struct s06_frame *f);	/* S-10 */
};

/* Handle the CPU link's delivered frames (on the slave's node). */
void s09_slave_cpu_poll(struct s06_node *node, unsigned link,
			const struct s09_cpu_ops *ops);

#endif

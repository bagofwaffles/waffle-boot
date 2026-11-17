/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09.009, S-13b.023: the slave boot stream's sessions handed to the SPI
 * controller under its flow control on the ready line, fed by DMA (make
 * BOOT_LINK_FC=2, waffle/s09_master.h). s09_fast_send() is the boot ops'
 * send(): it takes a session's bytes from s09_boot_slave() and moves them in
 * parts, a block's 16-byte header alone and the payload after it, each part
 * once a guard time has passed since the last part ended and the ready line
 * reads low.
 *
 * A part the controller gives up for a reason of its own, the ready line
 * low, has its bytes not clocked and the rest of the stream sent one byte at
 * a time, as the default build sends them, in the same chip-select session:
 * the slave's boot ROM has taken every byte clocked and is still in step.
 * That is the fall-back within the start, and it happens once: nothing goes
 * back to the controller after it. A part the slave holds for T_STREAM
 * (S-09.060) ends the stream as the byte path's wait does.
 */
#ifndef WAFFLE_S09_FAST_H
#define WAFFLE_S09_FAST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s06_clock.h"

/* A part's outcome: S09_PART_OK, S09_PART_HELD, or why the controller gave
 * it up, which is also the fall-back's cause. */
#define S09_PART_OK      0
#define S09_PART_HELD    1	/* nothing moved for T_STREAM with the line high */
#define S09_PART_DMA     2	/* the DMA reported an error */
#define S09_PART_STALL   3	/* nothing moved for T_STREAM with the line low */
#define S09_PART_NO_END  4	/* every byte fed, no end (TF) with the line low */
#define S09_PART_REFUSED 5	/* the driver refused the part's set-up */
#define S09_PART_CAUSES  6

struct s09_fast_ops {
	void *ctx;
	bool (*ready)(void *ctx);		/* the ready line reads low */
	/*
	 * n bytes by the controller under its flow control. Any outcome but
	 * S09_PART_OK sets *left to the bytes it did not clock, 0 to n.
	 */
	int (*part)(void *ctx, const uint8_t *d, size_t n, size_t *left);
	/* The link made ready for single bytes inside the session open now;
	 * false when it cannot be. */
	bool (*slow)(void *ctx);
	/* One byte, sent once the line has been read low, and the settle time
	 * after it; failed(): the byte path's driver has failed to move one. */
	void (*byte)(void *ctx, uint8_t tx);
	bool (*failed)(void *ctx);
};

/* How the stream went, for the log, the timing build and a developer's
 * request. Times in microseconds of the clock. */
struct s09_fast_stats {
	uint32_t sessions;	/* sessions given to s09_fast_send() */
	uint32_t parts;		/* parts sent whole by the controller */
	uint32_t fast_bytes;	/* bytes the controller clocked */
	uint32_t slow_bytes;	/* bytes sent one at a time after a fall-back */
	uint32_t high;		/* parts whose guard ended with the line high */
	uint32_t guard_us;	/* time in the guards */
	uint32_t wait_us;	/* time waiting for the line before parts and bytes */
	uint32_t wait_max_us;	/* the longest such wait */
	uint8_t fell;		/* the fall-back's cause, S09_PART_OK for none */
	bool held;		/* the slave held a part or a byte for T_STREAM */
	uint32_t fell_session;	/* the fall-back's session, 1 the first stream session */
	uint32_t fell_at;	/* the slot's bytes clocked before it */
};

struct s09_fast {
	const struct s09_fast_ops *ops;
	const struct s06_clock *clk;
	uint32_t guard_us;
	uint32_t last_end;	/* when the last part ended */
	bool began;		/* a part has ended */
	bool slow;		/* fallen back: one byte at a time from now on */
	uint8_t have;		/* the bytes of the header under way */
	uint8_t head[16];
	uint32_t left;		/* the payload's bytes still to come */
	struct s09_fast_stats st;
};

void s09_fast_init(struct s09_fast *f, const struct s09_fast_ops *ops,
		   const struct s06_clock *clk, uint32_t guard_us);

/* The link's last byte has ended now: the guard runs from here before the
 * first part too, as after the opening session, where the boot ROM takes its
 * mode. */
void s09_fast_pause(struct s09_fast *f);

/* S-13b.023: before a stream session's chip select falls, the guard from the
 * last part's end, so that the gap at every piece and block boundary is one
 * with the select high. Nothing after a fall-back. */
void s09_fast_session_guard(struct s09_fast *f);
/*
 * One stream session's bytes, the chip select asserted by the caller: true
 * when all of them went, by the controller or after a fall-back one at a
 * time; false when the slave held the line for T_STREAM or the byte path's
 * driver failed, the stream then ending as S-09.060 says.
 */
bool s09_fast_send(struct s09_fast *f, const uint8_t *d, size_t n);

#endif /* WAFFLE_S09_FAST_H */

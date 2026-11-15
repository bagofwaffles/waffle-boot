/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-14 diagnostic log: the line format, the threshold, the sinks, relayed
 * lines and the lines other documents require.
 *
 * Pure logic. Time and the three sinks come in through the operation
 * structs below; nothing here touches hardware. Lines are built on the
 * stack and offered to each sink at once: nothing is queued (S-14.027).
 */
#ifndef WAFFLE_S14_LOG_H
#define WAFFLE_S14_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* S-14.001, S-14.004, S-14.006 */
#define S14_LINE_MAX    248u
#define S14_CUT_KEEP    246u
#define S14_NAME_MAX    16u
#define S14_LINE_NO_MAX 2147483647u

/* S-14.021, S-14.023: routes (source 1 to PSoC 4 / host 2) and frame ids. */
#define S14_ROUTE_PSOC   0x14u
#define S14_ROUTE_CDC    0x12u
#define S14_ID_LOG       0x03u
#define S14_ID_LOG_ERROR 0x04u
#define S14_ADDR_PSOC    4u

/* S-14.011 */
#ifdef OPXY_LOG_DEBUG		/* a development build: make LOG_DEBUG=1 */
#define S14_DEFAULT_THRESHOLD 4u
#else
#define S14_DEFAULT_THRESHOLD 3u
#endif

/* S-14.010 */
enum s14_level {
	S14_FATAL = 0,
	S14_ERROR = 1,
	S14_WARNING = 2,
	S14_INFO = 3,
	S14_DEBUG = 4
};

/* Which core runs this log (S-14.026: the slave emits nothing). */
enum s14_core {
	S14_MASTER,
	S14_SLAVE
};

/*
 * A source tag (S-14.004, S-14.005). name NULL: no tag. A name outside the
 * grammar also gives no tag; a line number of 0 or above S14_LINE_NO_MAX
 * gives an empty number.
 */
struct s14_tag {
	const char *name;
	uint32_t line_no;
};

#define S14_TAG(name) ((struct s14_tag){ (name), (uint32_t)__LINE__ })
#define S14_NO_TAG    ((struct s14_tag){ NULL, 0u })

/*
 * Build a line (S-14.001) into out: the timestamp (S-14.002), the tag, then
 * text up to its first 0x00 (S-14.008), cut (S-14.006) or with one final LF
 * removed (S-14.007). Returns the line's length, 4..248.
 */
size_t s14_line_build(uint8_t out[S14_LINE_MAX], uint32_t timestamp,
		      struct s14_tag tag, const uint8_t *text, size_t text_len);

/*
 * Length of the tag at the start of a body (S-14.004), or 0 when the body
 * does not start with one. A tag is never shorter than 4 bytes ("[]: ").
 */
size_t s14_tag_len(const uint8_t *body, size_t len);

/* Whether a name and number make a tag of the S-14.004 grammar. */
bool s14_tag_name_valid(const char *name);

/*
 * A text under construction, for callers that put numbers or outside text
 * into a line. Nothing is interpreted: no format string (S-14.008,
 * S-16.099). Text past the buffer is counted but not kept; such a text
 * always makes the line long enough to be cut (S-14.006), so nothing that
 * would be emitted is lost.
 */
#define S14_TEXT_CAP S14_LINE_MAX
struct s14_text {
	uint8_t buf[S14_TEXT_CAP];
	size_t len;      /* bytes kept, at most S14_TEXT_CAP */
	bool overflow;   /* bytes were dropped past the end */
};

void s14_text_init(struct s14_text *t);
void s14_text_bytes(struct s14_text *t, const uint8_t *p, size_t n);
void s14_text_str(struct s14_text *t, const char *s);
void s14_text_dec(struct s14_text *t, uint32_t v);
/* Hexadecimal, lower case, at least min_digits digits, no prefix. */
void s14_text_hex(struct s14_text *t, uint32_t v, unsigned min_digits);

/* ---- interfaces ---------------------------------------------------- */

/* The microsecond count of S-14.002, from the processor's reset, mod 2^32. */
struct s14_clock_ops {
	uint32_t (*now_us)(void *ctx);
	void *ctx;
};

/*
 * One S-06 link sink: the PSoC link or the CDC host link.
 *
 * send() offers one frame (route, id, payload). It returns true when the
 * link has taken the frame for transmission, false when it has not: the
 * link is not open, the PSoC link is in raw mode, or its transmit side
 * cannot take the frame. The link's contract (S-14.027, S-14.028, S-16.172):
 *  - a frame that is not taken consumes no sequence number;
 *  - send() returns within 300 ms (at least the S-10c.033 wait for a
 *    255-byte frame) whether or not the frame is taken;
 *  - frames are sent in the order offered, interleaved with the link's
 *    other traffic at the point of the call (S-14.029).
 */
struct s14_link_ops {
	bool (*send)(void *ctx, uint8_t route, uint8_t id,
		     const uint8_t *payload, size_t len);
	void *ctx;
};

/*
 * The screen sink (S-14.025): append a body to the screen console. The
 * implementation must not log it again (S-15.004 covers lines a screen
 * appends, not this sink; see s15_console_append_raw()).
 */
struct s14_screen_ops {
	void (*show)(void *ctx, const uint8_t *body, size_t len);
	void *ctx;
};

/* ---- the log --------------------------------------------------------- */

struct s14_log {
	const struct s14_clock_ops *clock;
	const struct s14_link_ops *psoc;   /* may be NULL: no such sink */
	const struct s14_link_ops *cdc;    /* may be NULL */
	const struct s14_screen_ops *screen; /* may be NULL */
	uint8_t threshold;
	bool psoc_on;
	bool cdc_on;
	bool screen_started;
	bool silent;     /* the slave: S-14.026 */
};

/* The state of every start of the bootloader (S-14.011, S-14.022, S-14.024). */
void s14_log_init(struct s14_log *log, enum s14_core core,
		  const struct s14_clock_ops *clock,
		  const struct s14_link_ops *psoc,
		  const struct s14_link_ops *cdc,
		  const struct s14_screen_ops *screen);

/* The display has started in the main flow: level-0 lines also go to the
 * screen from now on (S-14.025). */
void s14_log_screen_start(struct s14_log *log);

/* The log-configuration request 0xb0 (S-14.012, S-14.013, S-14.014). */
void s14_log_config(struct s14_log *log, const uint8_t *payload, size_t len);

/* Whether a line of this level is emitted (S-14.011). */
bool s14_log_passes(const struct s14_log *log, enum s14_level level);

/* Emit a line: threshold, then every enabled sink (S-14.020 - S-14.029). */
void s14_log_emit(struct s14_log *log, enum s14_level level,
		  struct s14_tag tag, const uint8_t *text, size_t len);
void s14_log_str(struct s14_log *log, enum s14_level level,
		 struct s14_tag tag, const char *text);
void s14_log_text(struct s14_log *log, enum s14_level level,
		  struct s14_tag tag, const struct s14_text *text);

/*
 * A received frame with id 0x03 or 0x04 addressed to the bootloader, on a
 * link where it serves its command set (S-14.040 - S-14.042). src is the
 * route's source address. Returns true when the frame was such a message
 * (handled, with or without a line), false for any other id.
 */
bool s14_log_relay(struct s14_log *log, uint8_t src, uint8_t id,
		   const uint8_t *payload, size_t len);

/* ---- S-14.060: lines other documents require ---------------------- */

/* An S-06 frame failed its CRC: level 1, its own route, seq and id (S-16.020). */
void s14_log_bad_crc(struct s14_log *log, uint8_t route, uint8_t seq,
		     uint8_t id);
/* A double fault: level 0, no stack walk before it (S-16.007). */
void s14_log_double_fault(struct s14_log *log);
/* A coprocessor's DFU error reply: its text verbatim (S-16.099), at level
 * 1 (any of 0 - 3 is allowed). */
void s14_log_dfu_error(struct s14_log *log, const char *who,
		       const uint8_t *text, size_t len);

#endif /* WAFFLE_S14_LOG_H */

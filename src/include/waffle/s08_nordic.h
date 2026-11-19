/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-08: the master's side of the Nordic link. The reading rules (greeting
 * answer, key:value queries, DFU replies, the transfer's bytes) are pure
 * functions; the exchanges run on an S-06 node's Nordic link.
 *
 * When each exchange is made (S-08.035) is the caller's: S-04's reboot
 * requests, S-11's install runs, S-05/S-15's held mode.
 */
#ifndef WAFFLE_S08_NORDIC_H
#define WAFFLE_S08_NORDIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s06_link.h"

#define S08_STATE_UNKNOWN (-1)		/* the start-up read failed */
#define S08_STATE_IDLE 0
#define S08_STATE_UPGRADING 0xAB0D

#define S08_TYPE_APP 0xB0u		/* S-08.043 */
#define S08_TYPE_BOOTLOADER 0xB1u
#define S08_CHUNK_MAX 248u		/* S-08.046 */
#define S08_HEADER_LEN 64u		/* S-08.044 */

/* DFU commands (S-08.040). */
#define S08_DFU_PREPARE 0x02u
#define S08_DFU_CHUNK 0x03u
#define S08_DFU_PERFORM 0x05u

/* Query answers (S-08.020, S-08.021). */
#define S08_NO_ANSWER 0
#define S08_IN_DFU 1
#define S08_NOT_IN_DFU 2

/* DFU reply classes (S-08.040). */
#define S08_DFU_FAIL 0
#define S08_DFU_OK 1
#define S08_DFU_BUSY 2

/* CRC-16/XMODEM through the caller (S-08.044, S-11.008): polynomial 0x1021,
 * init 0, not reflected, no final XOR; "123456789" gives 0x31C3. */
typedef uint16_t (*s08_crc16_fn)(const uint8_t *data, size_t len);

/* Log events for S-14. */
#define S08_LOG_DROPPED 1		/* a frame discarded (S-08.015); data: payload */
#define S08_LOG_DFU_ERROR 2		/* data: the Nordic's error text, verbatim */
#define S08_LOG_ENTER_FAILED 3
#define S08_LOG_EXIT_FAILED 4
#define S08_LOG_TRANSFER_FAILED 5	/* a: the DFU command that failed */
#define S08_LOG_GREETING_UNANSWERED 6
#define S08_LOG_SLOT_UNREAD 7		/* data: the reply without a slot; a: 1 with a reply */
#define S08_LOG_GREETING_DROPPED 8	/* S-08.014's one-answer rule; data: payload; a: flag byte */

/* S-08.014: a greeting arriving this soon after an answer is dropped. */
#define S08_ANSWER_GAP_US S06_MS(20)

/* Where a transfer stopped (S-08.042, S-08.047), for the step's report. */
enum s08_stage {
	S08_AT_NONE,
	S08_AT_QUERY,		/* S-08.042: not in DFU, or no answer */
	S08_AT_PREPARE,
	S08_AT_HEADER,
	S08_AT_CHUNK,
	S08_AT_PERFORM,
};

/* What the exchange that stopped it got back. */
enum s08_reply {
	S08_REPLY_NONE,		/* nothing within the timeout */
	S08_REPLY_REFUSED,	/* a reply that is not success */
	S08_REPLY_BUSY,		/* busy, then nothing for 5 s (S-08.041) */
};

struct s08_failure {
	enum s08_stage at;
	enum s08_reply reply;
	uint16_t chunk, chunks;	/* S08_AT_CHUNK: which image chunk, from 1 */
};

struct s08_ops {
	void *ctx;
	/* S-14: a log line for an event; data/n may be NULL/0. */
	void (*log)(void *ctx, int ev, const uint8_t *data, size_t n, uint32_t a);
	/*
	 * S-08.004: power messages 0x00, 0x31, 0x35, 0x37, handled as S-07's
	 * (s07_power_message()). A nonzero return (S07_POWEROFF) is kept in
	 * power_status and returned by s08_poll().
	 */
	int (*power)(void *ctx, const struct s06_frame *f);
	/* S-08.004: any other ordinary message, S-10's command set. */
	void (*common)(void *ctx, const struct s06_frame *f);
	/* S-08.010, S-17.011: the revision byte of an answered greeting. */
	void (*revision)(void *ctx, uint8_t rev);
	s08_crc16_fn crc16;
};

struct s08_nordic {
	struct s06_node *node;
	unsigned link;
	const struct s08_ops *ops;
	int state;		/* the metadata state (S-08 Terms) */
	bool greeted;		/* S-08.002 made */
	unsigned greet_tries;	/* S-08.002's greetings sent */
	bool connected;		/* a greeting handled (S-08.031 step 4) */
	bool answered;		/* a greeting answer sent, at answered_at */
	uint32_t answered_at;
	bool replied;		/* the last info exchange had a reply */
	int power_status;	/* the first nonzero power() return */
	bool sku_read;		/* S-08.045: the power-on's sku is latched */
	uint32_t product;	/* ... and its product word */
	bool entry_failed;	/* the run's enter exchange failed (S-08.035) */
	struct s08_failure failure;	/* the last transfer's, S08_AT_NONE on success */
};

/* ---- reading rules ---- */

/*
 * S-08.011: the answer to a greeting; false when it is ignored (state
 * unknown, S-08.012). *id and *byte are the answer's id and payload byte.
 */
bool s08_answer_greeting(int state, const uint8_t *p, size_t n, uint8_t *id,
			 uint8_t *byte);

/*
 * S-08 Terms, S-17.023: the value of the first entry whose key is exactly
 * key, in a key:value payload. Returns the value's length and sets *val,
 * or -1 when there is no such entry.
 */
long s08_kv_find(const uint8_t *p, size_t n, const char *key,
		 const uint8_t **val);

/* S-08.021 on a reply payload (NULL: no reply). */
int s08_mode_answer(const uint8_t *p, size_t n);

/* S-08.022 on a reply payload (NULL: no reply): the slot, or -1. */
long s08_boot_slot(const uint8_t *p, size_t n);

/*
 * S-08.014: whether a greeting that arrived after the time at may be
 * answered, given the last greeting answer (answered false: none yet). It
 * may not when that answer was sent after at or at most 20 ms before it.
 */
bool s08_greeting_due(bool answered, uint32_t answered_at, uint32_t at);

/* S-08.031 step 4: an ordinary message that counts as the greeting. */
bool s08_counts_as_greeting(const struct s06_frame *f);

/* S-08.040 */
int s08_classify_dfu(uint8_t cmd, uint8_t reply_id, const uint8_t *p,
		     size_t n);

/* S-08.045: sku as read at start-up; read_ok false when the read failed. */
uint32_t s08_product_word(uint32_t sku, bool read_ok);

/* S-08.043: the 14-byte prepare payload. */
void s08_prepare_payload(const uint16_t version[4], uint8_t type,
			 uint32_t product, uint8_t out[14]);

/* S-08.044: the 64-byte TE firmware header of the header chunk. */
void s08_te_header(const uint8_t *image, uint32_t len,
		   const uint16_t version[4], uint32_t product,
		   s08_crc16_fn crc, uint8_t out[S08_HEADER_LEN]);

/* ---- exchanges ---- */

/*
 * Init; the CDC <-> Nordic rows of the node's forwarding table are taken
 * out of force until the link greeting ends (S-08.004).
 */
void s08_init(struct s08_nordic *n, struct s06_node *node, unsigned link,
	      const struct s08_ops *ops, int state);

/* The metadata state from now on (S-08.013). */
void s08_set_state(struct s08_nordic *n, int state);

/*
 * S-08.045: the sku read at start-up (read_ok false when the record or the
 * chip serial could not be read). Only the first call after s08_init()
 * counts: a sku set later in the power-on is used from the next one.
 */
void s08_set_sku(struct s08_nordic *n, uint32_t sku, bool read_ok);
/* The prepare's product word for this power-on; 0 before s08_set_sku(). */
uint32_t s08_product(const struct s08_nordic *n);

/* S-08.002: once per power-on, on entering held mode. True if answered. */
bool s08_link_greeting(struct s08_nordic *n);

/*
 * S-08.004: handle the frames the link has delivered so far. Returns
 * power_status: nonzero once a Nordic power message started a power-off
 * (the exchanges also handle frames; callers check power_status after them).
 */
int s08_poll(struct s08_nordic *n);

/*
 * S-08.014: between an install run's steps, answer the greetings that
 * arrived; other messages wait for held mode's loop. After a greeting
 * answer this, s08_poll() and the exchanges return only once the link has
 * been read through the 20 ms that follow it.
 */
void s08_answer_greetings(struct s08_nordic *n);

/* Held mode with nothing in progress, for us microseconds (S-08.014). */
void s08_idle(struct s08_nordic *n, uint32_t us);

/*
 * S-08.020: one info exchange. Returns the reply payload's length (copied
 * to buf, at most max) or -1 without a reply within 100 ms.
 */
long s08_info(struct s08_nordic *n, uint8_t *buf, size_t max);

int s08_mode_query(struct s08_nordic *n);		/* S-08.021 */
long s08_boot_slot_query(struct s08_nordic *n);	/* S-08.022 */

/* S-08.031, S-08.033: the frames delivered so far are handled first, so a
 * pending greeting is answered before the command. */
bool s08_enter_dfu(struct s08_nordic *n);		/* S-08.031 */
bool s08_exit_dfu(struct s08_nordic *n);		/* S-08.033 */

/* S-08.040, S-08.041: one DFU request; S08_DFU_OK or S08_DFU_FAIL. */
int s08_dfu_request(struct s08_nordic *n, const uint8_t *p, size_t len);

/* S-08.042-S-08.047: send an image; true when the perform succeeded.
 * n->failure says where a failed one stopped. */
bool s08_transfer(struct s08_nordic *n, const uint8_t *image, uint32_t len,
		  const uint16_t version[4], uint8_t type, uint32_t product);

/*
 * S-08.023, S-08.035 at the start of an install run: the enter exchange
 * when the package carries a Nordic image, then the version query. The
 * second info reply (when the Nordic is in DFU) is copied to info (S-17
 * reads sw_version and bl_version); *info_len is -1 without one. False when
 * the enter exchange failed.
 */
bool s08_run_start(struct s08_nordic *n, bool nordic_image, uint8_t *info,
		   size_t max, long *info_len);

/*
 * S-08.035 at the end of a run: one mode query, the exit when in DFU. After
 * a run whose enter exchange failed (s08_run_start() false, S-11.061 item
 * 2) nothing is sent and the result is true.
 */
bool s08_run_end(struct s08_nordic *n);

#endif

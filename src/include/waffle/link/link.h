/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-06 links and the node above them: sequence numbers, defaults, requests
 * and reply slots, forwarding between links, raw mode (S-06.010-S-06.065).
 *
 * A node owns up to S06_MAX_LINKS links. Each link reaches its byte stream
 * through a port (struct s06_port_ops): a non-blocking read, and a write
 * side that reports its room, so that a frame is written whole or not at
 * all. Received frames that are not replies queue in the link's inbox,
 * from which the protocol above takes them when it is ready (frames that
 * arrive during a request's wait are kept, not handled inside it).
 */
#ifndef WAFFLE_LINK_LINK_H
#define WAFFLE_LINK_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/link/clock.h"
#include "waffle/link/frame.h"
#include "waffle/boot/device.h"
#include "waffle/link/rx.h"

#define S06_SLOTS 4		/* S-06.030 */
#define S06_INBOX 8		/* delivered messages kept per link */
#define S06_MAX_LINKS 5
#define S06_MAX_ROUTES 8

/* The master's links (S-06.011), as indices into a node's links. */
#define S06_LINK_MIDI 0u
#define S06_LINK_CDC 1u
#define S06_LINK_PSOC 2u
#define S06_LINK_NORDIC 3u
#define S06_LINK_CPU 4u		/* to the slave; on the slave, to the master */

struct s06_port_ops {
	/* Up to max received bytes, oldest first; 0 if none. Never blocks. */
	size_t (*read)(void *ctx, uint8_t *buf, size_t max);
	/* Bytes the transmit side can take now. */
	size_t (*room)(void *ctx);
	/* Queue n bytes for sending; called only with n <= room(). */
	void (*write)(void *ctx, const uint8_t *data, size_t n);
};

/* Request slot states. */
#define S06_SLOT_FREE 0
#define S06_SLOT_PENDING 1
#define S06_SLOT_DONE 2		/* reply stored, not yet collected */

struct s06_slot {
	uint8_t state;
	uint32_t deadline;
	struct s06_frame reply;
};

struct s06_link {
	const struct s06_port_ops *ops;
	void *ctx;
	uint8_t own;
	uint8_t peer;
	uint8_t seq;		/* S-06.020 */
	uint8_t next_slot;
	bool raw;		/* S-06.060 */
	/* Not read at all: its bytes wait in the port (s06_link_hold()). */
	bool held;
	/* The port holds the sender while it is not read (USB's flow
	 * control): read only as far as the inbox has room, so that no
	 * message is dropped for a full inbox. */
	bool paced;
	struct s06_rx rx;
	struct s06_slot slot[S06_SLOTS];
	struct s06_frame inbox[S06_INBOX];
	uint8_t in_head;
	uint8_t in_count;
	uint32_t inbox_lost;	/* messages dropped because the inbox was full */
	/* Every byte that arrived before this time has been read: the start
	 * of the last poll that read the port empty. Stamped on each frame
	 * the next reads deliver (s06_frame's at). */
	uint32_t read_at;
};

/* A forwarding table row (S-06.050): arriving on in with dst, write on out. */
struct s06_route_entry {
	uint8_t in;
	uint8_t dst;
	uint8_t out;
};

/* Drops reported to the node's observer (for S-14's log lines). */
#define S06_DROP_PARTIAL S06_RX_DROP_PARTIAL
#define S06_DROP_CRC S06_RX_DROP_CRC
#define S06_DROP_LONG S06_RX_DROP_LONG
#define S06_DROP_FWD_LONG S06_RX_DROP_FWD_LONG
#define S06_DROP_NO_ROUTE 16	/* S-06.051, S-06.064 */
#define S06_DROP_NO_ROOM 17	/* S-06.046: whole or absent */
#define S06_DROP_REFUSED 18	/* forward_ok refused it */
#define S06_DROP_INBOX 18	/* the inbox was full */

struct s06_node_ops {
	/* A frame dropped on a link; data/n as s06_rx's buffer (may be NULL). */
	void (*dropped)(void *ctx, unsigned link, int why, const uint8_t *data,
			size_t n);
	/* S-10c.129: whether a frame from link for node dst is forwarded (wire: the
	 * encoded frame); NULL forwards every one, as S-06.050 says. */
	bool (*forward_ok)(void *ctx, unsigned link, uint8_t dst, const uint8_t *wire,
			   size_t n);
	void *ctx;
};

struct s06_node {
	struct s06_link *link[S06_MAX_LINKS];
	s06_crc8_fn crc;
	const struct s06_clock *clock;
	const struct s06_route_entry *routes;
	uint8_t nroutes;
	uint32_t routes_off;	/* bit i set: routes[i] is not in force */
	struct s06_node_ops obs;
};

/* The master's forwarding table, S-06.050 (6 rows). The slave has none. */
extern const struct s06_route_entry s06_master_routes[6];
#define S06_ROUTE_CDC_NORDIC 4u	/* index of the CDC -> Nordic row */
#define S06_ROUTE_NORDIC_CDC 5u	/* index of the Nordic -> CDC row */
/* The rows of the link to the slave (MIDI -> 5, CPU -> host): second-cpu. */
#define S06_ROUTES_CPU 0x3u

/*
 * S-06.050 on dev: the master's table in n, its two rows of the link to the
 * slave out of force on a device without second-cpu, where address 5 is in
 * no row on any link (S-06.010, S-06.051). Those two bits of routes_off
 * stay set; nothing else clears them.
 */
void s06_node_master_routes(struct s06_node *n, const struct wb_device *dev);

/* Own and peer address of each link (S-06.011). */
void s06_link_init(struct s06_link *l, const struct s06_port_ops *ops,
		   void *ctx, uint8_t own, uint8_t peer);

void s06_node_init(struct s06_node *n, s06_crc8_fn crc,
		   const struct s06_clock *clock);

/*
 * Read whatever each framed link has received and act on it: complete
 * requests, queue messages, forward or drop frames (S-06.040-S-06.052).
 * Links in raw mode are not read.
 */
void s06_node_poll(struct s06_node *n);

/* The same, for one link only. */
void s06_node_poll_link(struct s06_node *n, unsigned link);

/*
 * A held link is not read, by a poll, in raw mode or on leaving raw mode
 * (S-06.060's discard then takes nothing): nothing it receives is
 * delivered, forwarded or dropped until it is let go, and its bytes wait in
 * the port meanwhile, in order. Sending on it is not affected.
 */
void s06_link_hold(struct s06_link *l, bool on);

/* Take the oldest delivered message of a link; false if none. */
bool s06_link_take(struct s06_link *l, struct s06_frame *out);
/* Put a message just taken back, behind those still waiting. */
void s06_link_put(struct s06_link *l, const struct s06_frame *f);

/*
 * Originate a message (S-06.012, S-06.020). A zero nibble of route is
 * replaced by the default. The frame uses a sequence number whether or not
 * it is sent (S-16.147). Returns true if it was written; false in raw mode,
 * when the transmit side has no room for all of it, or for a payload over
 * 249 bytes (S-06.002: then no number is used either).
 */
bool s06_send(struct s06_node *n, unsigned link, uint8_t route, uint8_t id,
	      const uint8_t *payload, size_t len);

/*
 * Originate a log line (S-06.020, S-14.027): written whole or not at all,
 * and a sequence number is used only when it is written.
 */
bool s06_send_log(struct s06_node *n, unsigned link, uint8_t id,
		  const uint8_t *payload, size_t len);

/*
 * Start a request (S-06.030): takes a free slot (round robin), sends with
 * source 8 + slot to the peer, and waits timeout_us from now. Returns the
 * slot, or -1 when no slot is free (the request fails at once and nothing is
 * sent). A request not written because the link is raw or full still holds
 * its slot until its time is up.
 */
int s06_request(struct s06_node *n, unsigned link, uint8_t id,
		const uint8_t *payload, size_t len, uint32_t timeout_us);

/*
 * The state of a request: S06_SLOT_DONE (reply in l->slot[slot].reply),
 * S06_SLOT_PENDING, or S06_SLOT_FREE when its time ran out (S-06.035).
 */
int s06_request_state(struct s06_node *n, unsigned link, int slot);

/* Collect a DONE request's reply (may be NULL) and free its slot. */
void s06_request_collect(struct s06_node *n, unsigned link, int slot,
			 struct s06_frame *reply);

/* Free every pending slot whose time is up (S-06.035): a request waits
 * until its deadline, and a reply read by then is in time. */
void s06_link_expire(struct s06_link *l, uint32_t now);

/*
 * A whole request: send, wait (polling the whole node, idling on the
 * clock), and copy the reply. Returns 1 with a reply, 0 when the time ran
 * out, -1 when no slot was free.
 */
int s06_transact(struct s06_node *n, unsigned link, uint8_t id,
		 const uint8_t *payload, size_t len, uint32_t timeout_us,
		 struct s06_frame *reply);

/* S-06.060: enter or leave raw mode. */
void s06_set_raw(struct s06_node *n, unsigned link, bool on);

/* S-06.061: raw-mode bytes received, in order. */
size_t s06_raw_read(struct s06_node *n, unsigned link, uint8_t *buf,
		    size_t max);

/*
 * S-06.063, S-06.065: write bytes as given, no encoding, no delimiter, no
 * sequence number, in raw mode or between frames in framed mode. Written
 * whole or not at all; returns false if there was no room.
 */
bool s06_raw_write(struct s06_node *n, unsigned link, const uint8_t *data,
		   size_t len);

#endif

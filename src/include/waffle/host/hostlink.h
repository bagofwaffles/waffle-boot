/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_HOST_HOSTLINK_H
#define WAFFLE_HOST_HOSTLINK_H

/*
 * The host link: a small versioned protocol for any site or tool, carried in
 * S-06 frames of id HL_FRAME_ID on the USB serial port and on the USB MIDI
 * carrier. A request is call:1 tag:1 and the call's arguments, little-endian;
 * its reply is call:1 tag:1 status:1 and the reply's data. The unit also
 * sends HL_EVENT frames on its own: a job's record whenever it changes.
 *
 * S-10c.120 - S-10c.129: the protocol, its calls, its classes and its
 * approval. S-10c.135 - S-10c.137: an OS image sent into RAM and started
 * from there, and the approval with no hold that Developer mode can allow.
 *
 * Every call has one class (hl_calls): safe, needs the session's approval,
 * never from a host, or warned; the file calls take theirs from the path's
 * root. With the Host link setting off the unit answers HELLO, saying it is
 * off, and HL_E_OFF to everything else. One hold on the unit approves the
 * host for the session, which ends at a restart, when Host link is turned
 * off, or on HL_END. With Developer mode on and its "Host link without hold"
 * item on (S-10c.137), APPROVE is answered at once with HL_S_NO_HOLD, which
 * serves every call a hold would but those of class HL_HOLD; that session
 * ends too when the item or Developer mode is turned off.
 *
 * S-10c.131 - S-10c.134: the PSoC calls (the raw bridge and the reset) are
 * the warned class: keys come from the PSoC, so a dead one could approve
 * nothing. They need no approval, and the item above does not change them;
 * the unit shows a warning and counts down HL_PSOC_COUNT_MS first, and its
 * key 8 cancels.
 *
 * This file is portable: the board gives the volumes, the approval screen
 * and the actions through struct hl_ops (src/board/usb/hostlink.c).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HL_FRAME_ID   0x7Cu
#define HL_MAGIC      0x4C484257u	/* "WBHL" little-endian */
#define HL_VERSION    2u		/* this protocol's version: 2 adds S-10c.135 - S-10c.137 */
#define HL_MIN_CLIENT 1u		/* the oldest client version served */
#define HL_PAYLOAD    249u		/* S06_MAX_PAYLOAD */
#define HL_CHUNK      224u		/* data bytes in GET's reply and PUT_DATA */
#define HL_PATH_MAX   192u		/* bytes of a path, its root included */
#define HL_TEXT_MAX   40u		/* a job record's text, NUL not counted */
#define HL_INFO_LEN   24u
#define HL_PUT_SUFFIX ".wbpart"	/* the temporary name a put writes */

enum hl_call {
	HL_HELLO = 0x00, HL_SESSION = 0x01, HL_APPROVE = 0x02, HL_END = 0x03,
	HL_INFO = 0x04, HL_SET = 0x05, HL_JOB = 0x06,
	HL_STAT = 0x10, HL_LIST = 0x11, HL_GET = 0x12, HL_HASH = 0x13,
	HL_PUT_OPEN = 0x14, HL_PUT_DATA = 0x15, HL_PUT_END = 0x16,
	HL_REMOVE = 0x17, HL_MKDIR = 0x18, HL_RENAME = 0x19,
	HL_RUN = 0x20,
	HL_RAM_OPEN = 0x21, HL_RAM_DATA = 0x22, HL_RAM_END = 0x23,
	HL_PSOC_OPEN = 0x30, HL_PSOC_DATA = 0x31, HL_PSOC_CLOSE = 0x32,
	HL_PSOC_STATE = 0x33, HL_PSOC_RESET = 0x34,
	HL_PSOC_EVENT = 0x7E,		/* unit to host only */
	HL_EVENT = 0x7F,		/* unit to host only */
};

enum hl_status {
	HL_OK = 0,
	HL_E_OFF = 1,		/* the Host link setting is off */
	HL_E_APPROVAL = 2,	/* needs the session's approval: hold on the unit */
	HL_E_NEVER = 3,		/* never from a host, approved or not */
	HL_E_BAD = 4,		/* malformed */
	HL_E_UNKNOWN = 5,	/* no such call, action or setting in this build */
	HL_E_PATH = 6,		/* the path is refused */
	HL_E_NOT_FOUND = 7,
	HL_E_EXISTS = 8,
	HL_E_IO = 9,
	HL_E_BUSY = 10,		/* the unit is busy, or cannot show the screen now */
	HL_E_CHECKSUM = 11,	/* the whole file's SHA-256 is not the one declared */
	HL_E_OFFSET = 12,	/* not the offset the unit holds; data: have:4 */
	HL_E_UNREACHABLE = 13,	/* the volume does not answer */
	HL_E_READ_ONLY = 14,	/* the root takes no write */
	HL_E_REFUSED = 15,	/* the unit's own checks refused it */
	HL_E_FS = 16,		/* no volume this build reads there (exFAT, say) */
	HL_E_NOT_OPEN = 17,	/* the PSoC bridge is not open */
};

/* HL_HOLD: needs an approval given by the hold on the unit; a session the
 * Developer item approved (HL_S_NO_HOLD) is not enough (S-10c.137).
 * HL_WARNED: no approval; the unit warns and counts down (S-10c.131). */
enum hl_class { HL_SAFE = 0, HL_NEEDS = 1, HL_NEVER_C = 2, HL_BY_ROOT = 3,
		HL_HOLD = 4, HL_WARNED = 5 };

struct hl_entry {
	uint8_t id;
	uint8_t cls;		/* enum hl_class */
};

/* The one classification: calls, RUN's actions and SET's settings. */
enum hl_action {
	HL_A_INSTALL = 0x01,		/* parts:1 path: a package on the drive */
	HL_A_OS_WRITE = 0x02,		/* path: an OS image on the drive */
	HL_A_OS_ONCE = 0x03,		/* start /yaffs2/os.ldr once */
	HL_A_RESTART = 0x04,		/* the plain restart */
	HL_A_CONTINUE_OS = 0x05,	/* restart into the OS */
	HL_A_RESTART_MODE = 0x06,	/* mode:1, as the firmware command's EE */
	HL_A_RAM_START = 0x07,		/* start the image RAM_END accepted */
	HL_A_FACTORY_RESET = 0x10,	/* never */
	HL_A_PROTECTED_WRITES = 0x11,	/* never */
};
/* SET's settings, numbered as the Settings screen's digits. */
enum hl_setting { HL_SET_PROTECTED = 2, HL_SET_HOST_LINK = 7 };
#define HL_PARTS_ALL 0xFFu	/* INSTALL: every part the unit would offer */

extern const struct hl_entry hl_calls[];
extern const size_t hl_ncalls;
extern const struct hl_entry hl_actions[];
extern const size_t hl_nactions;
extern const struct hl_entry hl_settings[];
extern const size_t hl_nsettings;
/* An id's class in a table; 0xFF when it is not there. */
uint8_t hl_class_of(const struct hl_entry *t, size_t n, uint8_t id);

/* The roots a path starts with. */
enum hl_root { HL_ROOT_DRIVE = 0, HL_ROOT_OS = 1, HL_ROOT_USER = 2, HL_ROOTS = 3 };
#define HL_RF_READ  0x01u	/* get and hash */
#define HL_RF_WRITE 0x02u	/* put, remove, mkdir, rename */
#define HL_RF_OPEN  0x04u	/* list and stat need no approval */
struct hl_root_info {
	const char *name;
	uint8_t flags;
};
extern const struct hl_root_info hl_roots[HL_ROOTS];

/*
 * A checked path: "/<root>" or "/<root>/<rest>". full is it as given; rest
 * points into full past "/<root>/" ("" for the root itself).
 */
struct hl_path {
	uint8_t root;
	char full[HL_PATH_MAX + sizeof(HL_PUT_SUFFIX)];
	const char *rest;
};

/*
 * The one path check: n bytes (no NUL) naming a root of hl_roots and below
 * it components of 1 to 255 bytes, none "." or "..", no empty one and no
 * trailing "/", no byte below 0x20, 0x7F, '\\' or ':'. At most HL_PATH_MAX
 * bytes. HL_OK, or HL_E_PATH.
 */
int hl_path_parse(const uint8_t *p, size_t n, struct hl_path *out);

/* The session (enum hl_session); a refusal or timeout is kept to be read. */
enum hl_session { HL_S_NONE = 0, HL_S_PENDING = 1, HL_S_APPROVED = 2,
		  HL_S_REFUSED = 3, HL_S_TIMEOUT = 4, HL_S_NO_HOLD = 5 };

/*
 * S-10c.135: an OS image into the unit's RAM, never to a file. RAM_OPEN
 * size:4 sha256:32 makes room for it (the same size and hash again resume:
 * have:4); RAM_DATA off:4 and up to HL_CHUNK bytes at the offset the unit
 * holds; RAM_END checks the whole: its size and SHA-256 (HL_E_CHECKSUM),
 * then S-05.024's check (HL_E_REFUSED, data why:1, enum s05_refusal). An
 * image that passes is kept for HL_A_RAM_START until the session ends,
 * another RAM_OPEN or the unit needs the memory.
 */

/* A job's record: one shape for every action. */
enum hl_job_state { HL_J_NONE = 0, HL_J_QUEUED = 1, HL_J_RUNNING = 2,
		    HL_J_DONE = 3, HL_J_FAILED = 4 };
#define HL_PERCENT_NONE 0xFFu
struct hl_job {
	uint8_t id, action, state, step, percent, error;
	char text[HL_TEXT_MAX + 1u];
};

struct hl_stat {
	uint8_t type;		/* 1 file, 2 directory */
	uint32_t size, mtime;
};
typedef bool (*hl_each_fn)(void *arg, const char *name, const struct hl_stat *st);

/*
 * The PSoC calls (S-10c.131 - S-10c.134). OPEN takes entry:1 (1: the
 * bootloader-entry message first, 0: raw at once); RESET takes nothing;
 * DATA takes the bytes for the PSoC (at most HL_PSOC_TX_MAX); CLOSE and
 * STATE take nothing. Each reply's data is the PSoC record, and DATA's and
 * CLOSE's then the PSoC's bytes waiting, at most HL_PSOC_RX_MAX:
 *
 *   state:1 what:1 seconds:1 flags:1 overruns:4 held:2
 *
 * seconds: whole seconds left of the countdown, or of a reset's wait, else
 * 0; overruns: the
 * PSoC's bytes lost since the bridge opened; held: those still waiting
 * after this reply. The unit sends HL_PSOC_EVENT frames on its own, status
 * 0 and the record, at each change of state and of seconds.
 */
enum hl_psoc_state { HL_PS_NONE = 0, HL_PS_COUNTING = 1, HL_PS_OPEN = 2,
		     HL_PS_CANCELED = 3, HL_PS_ENDED = 4, HL_PS_RESETTING = 5 };
enum hl_psoc_what { HL_PW_NONE = 0, HL_PW_BRIDGE_ENTRY = 1, HL_PW_BRIDGE = 2,
		    HL_PW_RESET = 3 };
#define HL_PF_GREETED      0x01u	/* the bridge ended: the PSoC greeted */
#define HL_PF_GREETED_AT   0x02u	/* the PSoC had greeted when it was asked */
#define HL_PF_RESET_ALONE  0x04u	/* the reset request went alone: the unit waits */
#define HL_PF_NOT_RESET    0x08u	/* the PSoC did not act on the reset request */
#define HL_PSOC_REC        10u
#define HL_PSOC_TX_MAX     (HL_PAYLOAD - 2u)
#define HL_PSOC_RX_MAX     (HL_PAYLOAD - 3u - HL_PSOC_REC)
#define HL_PSOC_RING       2048u	/* the PSoC's bytes the unit holds */
#define HL_PSOC_COUNT_MS   15000u	/* the warning's countdown */
/* How long the unit runs on after the reset request alone before it holds
 * the reset not done: above the about 7 s a reset takes to come back. */
#define HL_PSOC_RESET_WAIT_MS 10000u

/* The board's side. Every int is an enum hl_status. */
struct hl_ops {
	void *ctx;
	bool (*link_on)(void *ctx);
	/* The approval screen shown; false when the unit cannot show it now. */
	bool (*approve_open)(void *ctx);
	/* HL_INFO_LEN bytes: versions, device, slot and boot state, settings. */
	void (*info)(void *ctx, uint8_t out[HL_INFO_LEN]);
	bool (*root_ok)(void *ctx, unsigned root);
	int (*stat)(void *ctx, const struct hl_path *p, struct hl_stat *st);
	/* Entries from `start` on, each(arg, ...) for each until it returns
	 * false; *end when the directory had no more. */
	int (*list)(void *ctx, const struct hl_path *p, uint32_t start,
		    hl_each_fn each, void *arg, bool *end);
	int (*read)(void *ctx, const struct hl_path *p, uint32_t off, uint8_t *buf,
		    uint32_t n, uint32_t *got);
	int (*hash)(void *ctx, const struct hl_path *p, uint8_t sha[32], uint32_t *size);
	/* One file written at a time: created (truncated), written, finished
	 * (closed and on the eMMC), or abandoned (closed). */
	int (*create)(void *ctx, const struct hl_path *p);
	int (*write)(void *ctx, const uint8_t *b, uint32_t n);
	int (*finish)(void *ctx);
	void (*abandon)(void *ctx);
	int (*remove)(void *ctx, const struct hl_path *p);
	int (*mkdir)(void *ctx, const struct hl_path *p);
	int (*rename)(void *ctx, const struct hl_path *from, const struct hl_path *to);
	/* Checks an action and queues it (p NULL when it names no file). */
	int (*run)(void *ctx, uint8_t action, uint8_t arg, const struct hl_path *p);
	int (*set)(void *ctx, uint8_t setting, uint8_t value);
	/* S-10c.137: Developer mode and its Host link without hold item both
	 * on. NULL: never. */
	bool (*no_hold)(void *ctx);
	/* S-10c.135: room for size bytes (any held image dropped first); the
	 * bytes at off; the whole checked, its SHA-256 out and *why S-05.024's
	 * refusal (HL_E_REFUSED); dropped. ram_open NULL: no RAM image in this
	 * build (HL_E_UNKNOWN). */
	int (*ram_open)(void *ctx, uint32_t size);
	int (*ram_write)(void *ctx, uint32_t off, const uint8_t *b, uint32_t n);
	int (*ram_end)(void *ctx, uint32_t size, uint8_t sha[32], uint8_t *why);
	void (*ram_drop)(void *ctx);

	/*
	 * The PSoC calls (S-10c.131 - S-10c.134); psoc_warn NULL: not in this
	 * build. now_ms: a millisecond clock. psoc_warn: the warning for what
	 * with seconds left, opened on the first call; HL_OK, HL_E_BUSY when it
	 * cannot show now, or HL_E_REFUSED. psoc_greeted: the PSoC has greeted
	 * in this start. psoc_canceled: the unit's 8 canceled the warning,
	 * whose screen is gone; read once. psoc_gone: the host has gone (USB
	 * not configured). psoc_enter: the bridge's screen, then the link raw,
	 * the bootloader-entry message first with entry. psoc_leave: the link
	 * framed again when it was raw, then the greeting (true when the PSoC
	 * greeted), and the covered screen back. psoc_read, psoc_write: the raw
	 * bytes, a write whole or not at all. psoc_lost: the PSoC's bytes the
	 * board lost below this layer, a running count. psoc_reset: the reset of
	 * a PSoC that has greeted, the plain restart's; on a unit it does not
	 * return. psoc_reset_alone: the reset request alone, for a PSoC that has
	 * not, the unit running on, and the screen saying so; psoc_leave(false)
	 * takes the screen down when the wait runs out. psoc_event: an HL_PSOC_EVENT payload
	 * for the host that asked.
	 */
	uint32_t (*now_ms)(void *ctx);
	int (*psoc_warn)(void *ctx, uint8_t what, uint8_t seconds);
	bool (*psoc_greeted)(void *ctx);
	bool (*psoc_canceled)(void *ctx);
	bool (*psoc_gone)(void *ctx);
	void (*psoc_enter)(void *ctx, bool entry);
	bool (*psoc_leave)(void *ctx, bool raw);
	size_t (*psoc_read)(void *ctx, uint8_t *buf, size_t max);
	bool (*psoc_write)(void *ctx, const uint8_t *b, size_t n);
	uint32_t (*psoc_lost)(void *ctx);
	void (*psoc_reset)(void *ctx);
	void (*psoc_reset_alone)(void *ctx);
	void (*psoc_event)(void *ctx, const uint8_t *payload, size_t n);
};

struct hostlink {
	const struct hl_ops *ops;
	uint8_t session;		/* enum hl_session */
	uint8_t next_job;
	struct hl_job job;
	struct {
		bool open;
		struct hl_path path, tmp;
		uint32_t size, have;
		uint8_t sha[32];
	} put;
	struct {
		bool open, ready;	/* being sent; checked, for RAM_START */
		uint32_t size, have;
		uint8_t sha[32];
	} ram;
	struct {
		uint8_t state, what, flags, seconds;	/* the record's */
		uint32_t start_ms;		/* the countdown's start */
		uint32_t dropped;		/* bytes the ring had no room for */
		uint32_t lost_at;		/* psoc_lost() at the open */
		uint16_t head, count;
		uint8_t ring[HL_PSOC_RING];
	} psoc;
};

void hl_init(struct hostlink *h, const struct hl_ops *ops);
/* A request's reply into out (HL_PAYLOAD bytes); its length. */
size_t hl_request(struct hostlink *h, const uint8_t *req, size_t n, uint8_t *out);
/* The approval screen's answer: HL_S_APPROVED, HL_S_REFUSED or HL_S_TIMEOUT. */
void hl_answer(struct hostlink *h, uint8_t outcome);
/* The session ends; a put under way is abandoned and its file removed, and
 * a PSoC countdown canceled or a bridge ended. */
void hl_end(struct hostlink *h);
/* Once a pass: with the setting off the session ends; a PSoC countdown runs
 * on, and an open bridge's bytes are read into the unit's ring. */
void hl_poll(struct hostlink *h);
/* A PSoC countdown, bridge or reset holds the unit (S-10c.134). */
bool hl_psoc_busy(const struct hostlink *h);
/* The PSoC record (HL_PSOC_REC bytes). */
void hl_psoc_record(const struct hostlink *h, uint8_t *out);
bool hl_approved(const struct hostlink *h);
/* Approved by the hold on the unit, as HL_HOLD needs. */
bool hl_held(const struct hostlink *h);
/* The RAM image is gone (the board took its memory): a later RAM_START
 * finds none. */
void hl_ram_drop(struct hostlink *h);
/* The job record changed: true when it differs from the one held. */
bool hl_job_set(struct hostlink *h, uint8_t state, uint8_t step, uint8_t percent,
		uint8_t error, const char *text);
/* An HL_EVENT frame's payload for the job held; its length. */
size_t hl_event(const struct hostlink *h, uint8_t *out);

#endif /* WAFFLE_HOST_HOSTLINK_H */

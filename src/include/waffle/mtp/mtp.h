/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_MTP_MTP_H
#define WAFFLE_MTP_MTP_H

/*
 * S-10b: the MTP responder, at level R (a read-only store) or level W (R
 * and the changes of S-10b.080 to S-10b.085), as its configuration says
 * (S-10b.021).
 *
 * Pure logic over the generic containers of the Still Image class: bulk
 * transfers in (s10b_rx), bulk transfers out (s10b_tx), nothing on the
 * interrupt endpoint (S-10b.008). The volume (S-12's yaffs2 volume) and the
 * device (battery, clock) are the board's, through the ops below; the USB
 * side is usb/device.c's.
 *
 * The session's listing (S-10b.013) is made at OpenSession into memory the
 * caller gives: a table of struct s10b_object and a pool holding each
 * listed object's name as stored, behind a record header of
 * S10B_NAME_REC bytes. S-10b.014 sets the floor: a caller gives at least
 * S10B_LIST_OBJECTS objects and S10B_LIST_POOL bytes. Past it, what does
 * not fit is not listed; folders are read breadth first, so the parent of
 * every listed object is listed, and the one listing serves the session.
 * At level W the listing follows each change: a deleted object's slot and
 * name are reused, never its handle (S-10b.012), and the pool is compacted
 * when a name does not fit, so a volume that stays within the floor stays
 * listed whole.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* S-10b.014: a volume of at most this many objects whose names total at
 * most this many bytes is listed in full. */
#define S10B_LIST_OBJECTS	8192u
#define S10B_LIST_NAMES		262144u
/* A name's place in the pool: its owner (u32, 0 when free) and length,
 * then its bytes. */
#define S10B_NAME_REC		5u
#define S10B_LIST_POOL		(S10B_LIST_NAMES + S10B_NAME_REC * S10B_LIST_OBJECTS)

/* --- Codes (MTP 1.1 appendix F) ----------------------------------------- */

#define S10B_OK				0x2001u
#define S10B_GENERAL_ERROR		0x2002u
#define S10B_SESSION_NOT_OPEN		0x2003u
#define S10B_OP_NOT_SUPPORTED		0x2005u
#define S10B_PARAM_NOT_SUPPORTED	0x2006u
#define S10B_INCOMPLETE_TRANSFER	0x2007u
#define S10B_INVALID_STORAGE		0x2008u
#define S10B_INVALID_HANDLE		0x2009u
#define S10B_DEVPROP_NOT_SUPPORTED	0x200Au
#define S10B_STORE_FULL			0x200Cu
#define S10B_WRITE_PROTECTED		0x200Du
#define S10B_ACCESS_DENIED		0x200Fu
#define S10B_NO_VALID_OBJECT_INFO	0x2015u
#define S10B_DEVICE_BUSY		0x2019u
#define S10B_INVALID_PARENT		0x201Au
#define S10B_INVALID_DEVPROP_VALUE	0x201Cu
#define S10B_INVALID_PARAMETER		0x201Du
#define S10B_SESSION_ALREADY_OPEN	0x201Eu
#define S10B_INVALID_PROP_CODE		0xA801u
#define S10B_INVALID_PROP_FORMAT	0xA802u
#define S10B_INVALID_PROP_VALUE		0xA803u
#define S10B_INVALID_DATASET		0xA806u
#define S10B_DEPTH_UNSUPPORTED		0xA808u

#define S10B_STORAGE_ID		0x00010001u
#define S10B_ALL		0xFFFFFFFFu

#define S10B_CONTAINER_HEADER	12u
#define S10B_NAME_MAX		255u	/* bytes of a name (S-12.058) */
#define S10B_NAME_UNITS		254u	/* UTF-16 units presented (S-10b.047) */
#define S10B_MAX_LEVEL		9u	/* S-10b.041 */
#define S10B_MAX_READ_PATH	501u	/* S-10b.042 */
/* A path the device rebuilds: a readable folder's and one name. */
#define S10B_PATH_MAX		(S10B_MAX_READ_PATH + 1u + S10B_NAME_MAX + 1u)

/* --- The board's side --------------------------------------------------- */

enum s10b_kind {
	S10B_KIND_FILE,
	S10B_KIND_DIR,
	S10B_KIND_OTHER,	/* links, special objects: not listed (S-10b Q-01) */
	S10B_KIND_FREE,		/* a table slot that lists nothing */
};

enum s10b_level {
	S10B_LEVEL_R,		/* read-only (S-10b.086) */
	S10B_LEVEL_W,		/* and the changes of S-10b.080 to S-10b.085 */
};

/* What a change to the volume came to. */
enum s10b_result {
	S10B_V_OK,
	S10B_V_EXISTS,		/* the name is taken; nothing changed */
	S10B_V_FULL,		/* no space (S-12.056) */
	S10B_V_FAIL,		/* any other failure */
};

/* One entry of a directory, as S-12 presents it. */
struct s10b_entry {
	const uint8_t *name;	/* not NUL-terminated */
	size_t name_len;
	enum s10b_kind kind;
	uint32_t size;		/* a file's bytes */
	uint32_t mtime, ctime;	/* seconds since 1970, UTC (S-12.059) */
};

typedef void (*s10b_entry_fn)(void *arg, const struct s10b_entry *e);

/*
 * Paths are the volume's: "/" for its root directory, "/a/b" for b in a.
 * Every call is made from s10b_rx() or s10b_tx(), so from the context that
 * drives the engine.
 */
struct s10b_volume_ops {
	void *ctx;
	/* The volume's total and free bytes (S-12.056); false when there is
	 * no volume (not mounted, absent: S-10b.031's empty store). */
	bool (*space)(void *ctx, uint64_t *total, uint64_t *free);
	/* Calls fn once for each entry of the directory; false when the
	 * directory cannot be read. */
	bool (*list)(void *ctx, const char *path, s10b_entry_fn fn, void *arg);
	/* Up to n bytes of a file from offset: the count read, fewer only at
	 * the end of the file; negative when it cannot be read. */
	long (*read)(void *ctx, const char *path, uint32_t offset,
		     uint8_t *buf, size_t n);

	/* Level W only; a level-R engine never calls these. */
	/* An object's kind, size and times (name unused); false when it is
	 * not there. */
	bool (*stat)(void *ctx, const char *path, struct s10b_entry *e);
	enum s10b_result (*mkdir)(void *ctx, const char *path);
	/* A new empty file open for writing, never replacing one: *fd is
	 * passed back to write and close. */
	enum s10b_result (*create)(void *ctx, const char *path, int32_t *fd);
	/* All n bytes at the end of the file, or a failure. */
	enum s10b_result (*write)(void *ctx, int32_t fd, const uint8_t *p,
				  size_t n);
	enum s10b_result (*close)(void *ctx, int32_t fd);
	/* A file, or a folder with everything beneath it. */
	enum s10b_result (*remove)(void *ctx, const char *path);
	/* Renames or moves; S10B_V_EXISTS when `to` exists, which is never
	 * replaced. */
	enum s10b_result (*rename)(void *ctx, const char *from, const char *to);
	enum s10b_result (*set_times)(void *ctx, const char *path,
				      uint32_t atime, uint32_t mtime);
	/* After each change: what the volume holds is committed to the eMMC. */
	void (*commit)(void *ctx);
};

struct s10b_device_ops {
	void *ctx;
	/* The battery charge in percent (S-07); above 100 counts as 100. */
	unsigned (*battery)(void *ctx);
	/* The RTC count register as S-04.070 encodes it. */
	uint32_t (*clock_read)(void *ctx);
	/* Sets the clock (S-10b.062); waiting for an earlier write of the
	 * register to complete is the board's (S-16.063). */
	void (*clock_write)(void *ctx, uint32_t count);
};

/* One listed object (S-10b.012): the listing gives object `index` the
 * handle s10b_handle(index); an object created later the next unused one. */
struct s10b_object {
	uint32_t handle;
	uint32_t parent;	/* index + 1 of its folder; 0: the root */
	uint32_t name;		/* offset of its name's bytes in the pool */
	uint32_t size, mtime, ctime;
	uint16_t path_len;	/* bytes of P (S-10b.042) */
	uint16_t rows;		/* S-10b.046 rows found in P, bit k = row k */
	uint8_t name_len;
	uint8_t level;		/* 1 at the root */
	uint8_t kind;		/* enum s10b_kind */
	uint8_t flags;		/* mtp/mtp.c's */
};

struct s10b_config {
	enum s10b_level level;
	const struct s10b_volume_ops *vol;
	const struct s10b_device_ops *dev;
	/* DeviceInfo's strings (S-10b.020), UTF-8; the product string is also
	 * StorageDescription (S-10b.031). s10b_identity() replaces the three
	 * S-10a ones. */
	const char *manufacturer, *model, *version, *serial;
	struct s10b_object *objects;
	size_t max_objects;
	uint8_t *names;
	size_t names_size;
};

/* --- The engine --------------------------------------------------------- */

/* Bytes of a host data phase kept: an ObjectInfo dataset through
 * DateModified with names of 255 units (S-10b.080). */
#define S10B_DATA_MAX		1600u
/* A file's bytes gathered before each write to the volume (S-10b.081). */
#define S10B_WRITE_CHUNK	4096u
#define S10B_BUF_MAX		2304u	/* the largest fixed dataset */
#define S10B_STAGE_MAX		(8u + 1u + 2u * (S10B_NAME_UNITS + 1u))

enum s10b_rx_state {
	S10B_RX_COMMAND,	/* waiting for a command container */
	S10B_RX_DATA,		/* a host data phase (S-10b.006) */
	S10B_RX_SKIP,		/* the rest of a container out of place */
};

enum s10b_tx_state {
	S10B_TX_IDLE,
	S10B_TX_DATA,
	S10B_TX_ZLP,		/* the data phase ends on a full packet */
	S10B_TX_RESPONSE,
};

enum s10b_source {
	S10B_SRC_BUF,		/* m->buf */
	S10B_SRC_HANDLES,	/* an ObjectHandle array of a selection */
	S10B_SRC_PROPS,		/* an ObjectPropList of a selection */
	S10B_SRC_FILE,		/* bytes of a file */
};

/* Which objects a list holds. */
struct s10b_select {
	uint8_t mode;		/* enum in mtp/mtp.c */
	uint32_t index;		/* the folder's or the object's index + 1 */
	uint16_t format;	/* 0: every format */
};

struct s10b {
	struct s10b_config cfg;
	uint32_t session;	/* 0: none open */
	size_t n_objects, names_used;
	size_t mps;		/* the bulk endpoints' packet size */

	/* The transaction. */
	uint16_t code;
	uint32_t tid, params[5];
	enum s10b_rx_state rx;
	uint32_t rx_total, rx_got;	/* container bytes */
	bool rx_header;			/* the data container's header in */
	uint8_t data[S10B_DATA_MAX];
	size_t data_len;

	enum s10b_tx_state tx;
	uint16_t reply;
	unsigned n_reply;
	uint32_t reply_params[5];
	uint32_t data_total;		/* the dataset's bytes */
	uint32_t data_sent;		/* container bytes sent */

	/* The dataset's source and where it stands. */
	enum s10b_source src;
	uint8_t phase;			/* header, count, body */
	struct s10b_select sel;
	uint16_t prop;			/* 0: every supported property */
	size_t cur, cur_prop;		/* next object index, property */
	uint32_t count;			/* elements or handles */
	uint32_t file_off, file_end;
	const uint8_t *st;
	size_t st_pos, st_len;
	uint8_t hdr[S10B_CONTAINER_HEADER];	/* sent, or received */
	uint8_t stage[S10B_STAGE_MAX];
	uint8_t buf[S10B_BUF_MAX];
	char path[S10B_PATH_MAX];

	/* Level W. */
	uint32_t next_handle;	/* index of the next new handle */
	size_t names_dead;	/* pool bytes of names no longer used */
	/* The file a SendObjectInfo prepared (S-10b.080). */
	bool send_ready;
	uint32_t send_parent;	/* index + 1, 0: the root */
	uint32_t send_handle, send_size, send_mtime;	/* mtime 0: the clock */
	uint8_t send_len;
	uint8_t send_name[S10B_NAME_MAX];
	/* SendObject's data phase (S-10b.081). */
	bool sending;		/* taking it into a file */
	bool w_open, w_abandon;	/* a file open; left by a cancel */
	enum s10b_result w_res;
	bool w_short;		/* its length is not the one announced */
	int32_t w_fd;
	size_t w_len;
	uint8_t w_buf[S10B_WRITE_CHUNK];
	char w_path[S10B_PATH_MAX];
};

void s10b_init(struct s10b *m, const struct s10b_config *cfg);

/* The S-10a strings of the set presented (S-10b.020, S-10b.031). */
void s10b_identity(struct s10b *m, const char *manufacturer,
		   const char *model, const char *serial);

/*
 * The bulk endpoints' packet size at the speed configured (S-10a); a bus
 * event (S-10b.007): any transaction ends and the session closes.
 */
void s10b_reset(struct s10b *m, size_t mps);

/* Cancel and Device Reset (S-10b.005, Q-06): the transaction in progress
 * ends without a response; the session stays open. */
void s10b_cancel(struct s10b *m);

/* Whether a transaction is in progress (for Get Device Status). */
bool s10b_busy(const struct s10b *m);

/*
 * Work a canceled or reset transaction left (level W: the file a SendObject
 * had begun is removed, S-10b.005). Called where the volume may be used,
 * outside s10b_cancel() and s10b_reset(), which never use it; s10b_rx()
 * and s10b_tx() call it first.
 */
void s10b_service(struct s10b *m);

/*
 * Bulk OUT. s10b_rx_size() is how many bytes to arm the next transfer
 * with (0: none now, the device is answering); s10b_rx() takes the
 * transfer, n bytes, short when n is not a multiple of the packet size.
 */
size_t s10b_rx_size(const struct s10b *m);
void s10b_rx(struct s10b *m, const uint8_t *p, size_t n);

/*
 * Bulk IN: the next transfer into buf (cap at least one packet). False
 * when there is nothing to send; *n 0 is a zero-length packet.
 */
bool s10b_tx(struct s10b *m, uint8_t *buf, size_t cap, size_t *n);

/* --- Pure helpers (for the tests) --------------------------------------- */

/* The handle of the listing's object `index` (S-10b.012). */
uint32_t s10b_handle(size_t index);

/*
 * S-10b.047: a name as UTF-16 units, every byte outside a well-formed UTF-8
 * sequence one U+FFFD, cut to the whole characters within 254 units.
 * Returns the units written (out holds S10B_NAME_UNITS).
 */
size_t s10b_name_utf16(const uint8_t *name, size_t n, uint16_t *out);

/* S-10b.046: the rows of the format table a name holds, bit k = row k. */
uint16_t s10b_format_rows(const uint8_t *name, size_t n);
/* The format of a file whose path holds `rows`. */
uint16_t s10b_format(uint16_t rows);
/* A file format the store can report: 0x3000 or one of the table's. */
bool s10b_format_known(uint16_t format);

/* S-10b.048: YYYYMMDDThhmmss of a UTC time, 16 bytes with the NUL. */
void s10b_datetime_text(uint32_t secs, char out[16]);

/*
 * S-10b.062: a DateTime value's characters (no count byte, no NUL) are
 * well-formed, with the clock's range when `range`. The date and time
 * written go to *dt.
 */
struct s10b_datetime {
	unsigned year, month, day, hour, minute, second;
};
bool s10b_datetime_parse(const uint16_t *units, size_t n, bool range,
			 struct s10b_datetime *dt);
/* Seconds since 1970-01-01 UTC; false before 1970 or past 2^32 - 1. */
bool s10b_datetime_secs(const struct s10b_datetime *dt, uint32_t *secs);

/*
 * A name from the host (S-10b.080): n UTF-16 units, without the String's
 * NUL, as UTF-8 into out (S10B_NAME_MAX bytes). 0 when it is not a name:
 * empty, a NUL unit, an unpaired surrogate, more than 255 bytes, a '/', or
 * "." or "..".
 */
size_t s10b_name_utf8(const uint16_t *units, size_t n, uint8_t *out);

#endif /* WAFFLE_MTP_MTP_H */

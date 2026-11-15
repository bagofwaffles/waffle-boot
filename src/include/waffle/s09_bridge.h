/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09 bridge records: request headers, payload lengths and padding, reply
 * shapes, the file status, volume status and directory entry records, the
 * error numbers and the chunked-read rule (S-09.020-S-09.043). Shared by the
 * master (s09_master.h) and the slave (s09_slave.h). All words are
 * little-endian.
 */
#ifndef WAFFLE_S09_BRIDGE_H
#define WAFFLE_S09_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Commands (S-09.023). */
#define S09_INIT 0x01u
#define S09_ERASE_GROUP 0x03u
#define S09_FLUSH 0x04u
#define S09_BOOT_SIZE 0x05u
#define S09_POLL 0x06u
#define S09_RELEASE 0x07u
#define S09_MOUNT 0x08u
#define S09_UNMOUNT 0x09u
#define S09_ECHO 0x0Au
#define S09_CLOCK_SYNC 0x0Bu
#define S09_READ_SECTOR 0x11u
#define S09_READ_SECTORS 0x12u
#define S09_WRITE_RELIABLE 0x13u
#define S09_ERASE 0x14u
#define S09_WRITE_SECTOR 0x15u
#define S09_WRITE_SECTORS 0x16u
#define S09_SELECT 0x18u
#define S09_LINK_MODE 0x19u	/* S-09.070 */
#define S09_LINK_TEST 0x1Au	/* S-09.071 */
#define S09_LINK_STATUS 0x1Bu	/* S-09.079 */
#define S09_PUSH0 0x1Eu
#define S09_PUSH1 0x1Fu
#define S09_PULL 0x20u
#define S09_ACCESS 0x21u
#define S09_STAT 0x22u
#define S09_STATFS 0x23u
#define S09_RENAME 0x24u
#define S09_UNLINK 0x25u
#define S09_MKDIR 0x26u
#define S09_RMDIR 0x27u
#define S09_SYNC 0x28u
#define S09_OPEN 0x29u
#define S09_READ 0x2Au
#define S09_WRITE 0x2Bu
#define S09_LSEEK 0x2Cu
#define S09_CLOSE 0x2Du
#define S09_OPENDIR 0x2Eu
#define S09_READDIR 0x2Fu
#define S09_CLOSEDIR 0x30u
#define S09_FREE_SPACE 0x31u
#define S09_TOTAL_SPACE 0x32u
#define S09_FSYNC 0x34u
#define S09_UTIME 0x35u
#define S09_FUTIMENS 0x36u

/* Kinds (S-09 Terms). */
#define S09_K_UNDEFINED 0
#define S09_K_IMMEDIATE 1
#define S09_K_DEFERRED 2
#define S09_K_POLL 3
#define S09_K_PUSH 4
#define S09_K_PULL 5
#define S09_K_CLOCK 6

#define S09_SLOTS 4
#define S09_HDR 12u
#define S09_SECTOR 512u
#define S09_MAX_SECTORS 128u
#define S09_TUNNEL_MAX 0x1000u	/* push size, pull capacity */
#define S09_READ_CHUNK 0x1000u	/* S-09.035: C as the master sends it */
#define S09_WRITE_MAX 0x8000u	/* S-09.023: a file write's most bytes */
/* S-09.071: a link test's most bytes, those of the largest request (a
 * 128-sector write) */
#define S09_TEST_MAX 0x10000u

/*
 * S-09.070: a link mode, how request sessions travel. The link comes up
 * plain. Reply sessions are single I/O in every mode.
 */
#define S09_MODE_PLAIN 0u	/* single I/O, no check */
#define S09_MODE_CHECKED 1u	/* single I/O, each request ends with its check */
#define S09_MODE_QUAD 2u	/* quad I/O, each request ends with its check */
#define S09_MODE_COUNT 3u
#define S09_LINK_TRY 0u		/* 0x19's arg1: on trial, or kept */
#define S09_LINK_KEEP 1u
#define S09_CHECK_LEN 8u	/* S-09.072: the trailer, number then check */
#define S09_SEQ_NONE 0u		/* S-09.076: a request with no number */
#define S09_STAT_LEN 52u
#define S09_STATFS_LEN 12u
#define S09_DIRENT_LEN 268u
#define S09_DIRENT_NAME 257u

#define S09_INIT_VALUE 0xFEDEBEDAu	/* S-09.015 */
#define S09_ECHO_ARG 0x12345678u
/* S-09.074: the recovery echo's arg0 (its arg1 0, no payload): the one
 * request a slave that has fallen back executes. */
#define S09_ECHO_RECOVER 0x6B3D90E5u
#define S09_POLL_NONE 0x0000FF06u	/* 06 FF 00 00 (S-09.031) */

/*
 * S-09.079: the link's status, 0x1B's reply: the header word, the slave's
 * fall-backs so far, then cause | command << 8 | mode << 16 of the last
 * one, the payload length its header gave, the bytes its session brought,
 * and the receive's four fault words (rc, status, words left, stage).
 */
#define S09_STATUS_LEN 36u
#define S09_WHY_NONE 0u
#define S09_WHY_CHECK 1u	/* a request's check word wrong */
#define S09_WHY_SHORT 2u	/* a session shorter than its request */
#define S09_WHY_IGNORED 3u	/* a header the slave ignores */
#define S09_WHY_RECEIVE 4u	/* the receive failed: the fault words */
#define S09_WHY_TRIAL 5u	/* a trial with no request for T_TRIAL */
#define S09_WHY_NUMBER 6u	/* a number or check that conflicts */
#define S09_WHY_ON_TRIAL 7u	/* a request other than 0x19 or 0x1A on trial */
#define S09_WHY_KAT 8u		/* its check failed its known answers */
#define S09_WHY_REPLY_END 9u	/* a reply fed whole, its end not seen (S-13b.083) */
#define S09_WHY_COUNT 10u

/* Error numbers (S-09.042). */
#define S09_ENOENT 2u
#define S09_EBADF 9u
#define S09_ENOMEM 12u
#define S09_EACCES 13u
#define S09_EFAULT 14u
#define S09_EBUSY 16u
#define S09_EEXIST 17u
#define S09_EXDEV 18u
#define S09_ENODEV 19u
/* The master's own report of a request in s09_plain_safe()'s set whose
 * plain exchange failed, the slave kept (S-09.013); no slave sends it. */
#define S09_EIO 5u
#define S09_ENOTDIR 20u
#define S09_EISDIR 21u
#define S09_EINVAL 22u
#define S09_ENFILE 23u
#define S09_ENOSPC 28u
#define S09_EROFS 30u
#define S09_ERANGE 34u
#define S09_ELOOP 40u
#define S09_ENODATA 61u
#define S09_ENAMETOOLONG 78u
#define S09_ENOTEMPTY 93u

/* Open flags (S-09.041). */
#define S09_O_RDONLY 0u
#define S09_O_WRONLY 1u
#define S09_O_RDWR 2u
#define S09_O_CREAT 0x40u
#define S09_O_EXCL 0x80u
#define S09_O_TRUNC 0x200u
#define S09_O_APPEND 0x400u

struct s09_req {
	uint8_t cmd;
	uint8_t slot;
	uint16_t len;
	uint32_t arg0;
	uint32_t arg1;
};

/* S-09.023 */
int s09_kind(uint8_t cmd);

/*
 * S-09.013 rule 4: requests that leave the slave as the master may rely on
 * whether they are executed once, twice or not at all: the size queries,
 * the sector reads, the partition select (the master selects again before
 * its next sector request after one that failed) and the path queries.
 * Their exchange failing in the plain mode costs the request, not the slave.
 */
bool s09_plain_safe(uint8_t cmd);

/* S-09.020 */
void s09_req_pack(const struct s09_req *r, uint8_t out[S09_HDR]);
void s09_req_unpack(const uint8_t in[S09_HDR], struct s09_req *r);

static inline uint32_t s09_pad4(uint32_t n)
{
	return (n + 3u) & ~3u;
}

/* S-09.021: the payload bytes the master clocks for a payload of n bytes
 * (pad4, with 0x44 and 0x48 lengthened to 0x4C, S-16.112). */
uint32_t s09_master_payload_wire(uint32_t n);

/*
 * S-09.072: the check word of a request session's n bytes (whole words)
 * before it: CRC-32/BZIP2 (polynomial 0x04C11DB7, not reflected, initial
 * value and final XOR 0xFFFFFFFF) of the words in the order the wire
 * carries their bits, each little-endian word most significant bit first
 * (S-13b.040). Four tables in software, a word a step, built by
 * s09_check_ready(), which every end calls before it leaves the plain mode.
 */
uint32_t s09_check_word(const uint8_t *d, uint32_t n);
/*
 * S-09.072: the check's tables built, then s09_check_word() put to known
 * answers; false when one is wrong: the end stays in the plain mode.
 */
bool s09_check_ready(void);
/* The tables, and a call made after each build (NULL; tests model memory
 * that keeps nothing with it). */
extern uint32_t s09_check_t[4][256];
extern void (*s09_check_built)(void);
/*
 * S-09.072, S-09.076: the trailer after a request's `body` bytes: the
 * number seq, then the check word of every byte before the check (the
 * body and the number). check: an engine for s09_check_word(), or NULL.
 * Returns the check word written.
 */
uint32_t s09_trailer(uint8_t *d, uint32_t body, uint32_t seq,
		     uint32_t (*check)(void *ctx, const uint8_t *d, size_t n),
		     void *ctx);
/* S-09.076: requests that carry no number in every mode: they change no
 * storage, so a second execution is harmless. */
static inline bool s09_unnumbered(uint8_t cmd)
{
	return cmd == S09_ECHO || cmd == S09_LINK_MODE || cmd == S09_LINK_TEST ||
	       cmd == S09_LINK_STATUS;
}
/* S-09.079: 0x1B exactly: no payload, arg0 and arg1 0. */
static inline bool s09_status_exact(const struct s09_req *r)
{
	return r->cmd == S09_LINK_STATUS && r->len == 0u && r->arg0 == 0u &&
	       r->arg1 == 0u;
}
/* S-09.071: a link test's reply value for its n payload bytes (padded to
 * whole words with zeros by the caller). */
uint32_t s09_test_value(const uint8_t *d, uint32_t n);

/* S-09.023, S-09.037: is a multi-sector request's count (arg1) in 1-128? */
static inline bool s09_count_ok(const struct s09_req *r)
{
	return r->arg1 >= 1 && r->arg1 <= S09_MAX_SECTORS;
}

/*
 * S-09.023: the payload length n the slave takes for a header and keeps.
 * For 0x13 and 0x16 with a count outside 1-128 it is 0: that payload is
 * s09_payload_discard()'s.
 */
uint32_t s09_payload_len(const struct s09_req *r);

/*
 * S-09.037: the payload bytes the slave takes after the header and discards
 * without keeping them: arg1 x 512 for 0x13 and 0x16 with a count outside
 * 1-128 (S-09.023: from arg1, not len), else 0.
 */
uint64_t s09_payload_discard(const struct s09_req *r);

/* The reply header word: cmd | slot << 8. */
static inline uint32_t s09_hdr_word(uint8_t cmd, uint8_t slot)
{
	return (uint32_t)cmd | (uint32_t)slot << 8;
}

/*
 * S-09.032: the data bytes after [hdr][result][errno] of a deferred reply
 * to cmd (n = the request's arg1 for 0x12). Returns -1 for 0x2A, 0x20 and
 * undefined commands, whose shapes are not fixed.
 */
long s09_deferred_data_len(uint8_t cmd, uint32_t arg1);

/* S-09.035 step 1: the size piece k asks for (R received so far). */
static inline uint32_t s09_read_ask(uint32_t n, uint32_t received, uint32_t c)
{
	return n - received < c ? n - received : c;
}

/* S-09.035 step 3: does the read go on after a piece of r? */
static inline bool s09_read_continues(int32_t r, uint32_t c, uint32_t received,
				      uint32_t n)
{
	return r >= 0 && (uint32_t)r == c && received + (uint32_t)r < n;
}

/* S-09.043 records */
struct s09_stat {
	uint32_t dev, ino, mode, nlink, uid, gid, rdev, size, blksize, blocks;
	uint32_t atime, mtime, ctime;
};

struct s09_statfs {
	uint32_t bsize, blocks, bfree;
};

struct s09_dirent {
	uint32_t ino;
	uint32_t off;
	uint8_t type;		/* 4 directory, 8 file, 10 symbolic link */
	char name[S09_DIRENT_NAME];
};

void s09_stat_pack(const struct s09_stat *s, uint8_t out[S09_STAT_LEN]);
void s09_stat_unpack(const uint8_t in[S09_STAT_LEN], struct s09_stat *s);
void s09_statfs_pack(const struct s09_statfs *s, uint8_t out[S09_STATFS_LEN]);
void s09_statfs_unpack(const uint8_t in[S09_STATFS_LEN], struct s09_statfs *s);
void s09_dirent_pack(const struct s09_dirent *d, uint8_t out[S09_DIRENT_LEN]);
void s09_dirent_unpack(const uint8_t in[S09_DIRENT_LEN], struct s09_dirent *d);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09 bridge records: request headers, payload lengths and padding, reply
 * shapes, the file status, volume status and directory entry records, the
 * error numbers and the chunked-read rule (S-09.020-S-09.043). Shared by the
 * master (bridge/master.h) and the slave (bridge/slave.h). All words are
 * little-endian.
 */
#ifndef WAFFLE_BRIDGE_RECORDS_H
#define WAFFLE_BRIDGE_RECORDS_H

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
/* S-09.041: 0x08 with arg0 1 is the scan before a write (S-12.062). Its
 * result when mounted is 0 for a mount that read no checkpoint, otherwise
 * the way (S-12.061: 1 restored, 2 to 5 scanned and why, 6 scanned for a
 * write) in bits 0-7 and the objects of the tree in bits 8-30. */
#define S09_MOUNT_FOR_WRITE 1u
#define S09_MOUNT_WAY(r) ((uint32_t)(r) & 0xFFu)
#define S09_MOUNT_OBJECTS(r) ((uint32_t)(r) >> 8)
#define S09_MOUNT_OBJECTS_MAX 0x7FFFFFu
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
#define S09_QUAD_TEST 0x1Cu	/* S-09.080: a developer's quad reply test */
#define S09_STAGE_WRITE 0x1Du	/* S-09.084: bytes into the slave's staging area */
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
 * plain. Reply sessions are single I/O in every mode, but for the sector
 * data of mode 3 (S-09.081).
 */
#define S09_MODE_PLAIN 0u	/* single I/O, no check */
#define S09_MODE_CHECKED 1u	/* single I/O, each request ends with its check */
#define S09_MODE_QUAD 2u	/* quad I/O, each request ends with its check */
#define S09_MODE_COUNT 3u	/* the modes a trial asks for */
/* S-09.081: requests as mode 1; a 0x12's sector data in a session of its
 * own in quad I/O. Kept from mode 1 by 0x19 keep, never on trial. */
#define S09_MODE_QDATA 3u
/* S-09.081: a 0x12's finished reply in mode 3, its session in single I/O:
 * [hdr][result][errno][the data's check word], before its check */
#define S09_QDATA_HEAD 16u
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

/*
 * S-09.080, S-13b.084: the quad reply test, a developer's measurement of
 * what ends a reply sent in quad I/O. 0x1C is an immediate request with no
 * payload, served in the plain mode only. arg0's low four bits are its
 * operation:
 *   ARM (arg1 the reply's bytes, S09_QT_MIN to S09_QT_MAX, whole words):
 *     arg0 also carries the candidate ending, REN, the pattern and a seed;
 *     the reply is [hdr][S09_QT_ARMED or a refusal], single I/O;
 *   SEND (arg0 1, arg1 0): the armed test reply, sent in quad I/O under
 *     the armed candidate; unarmed, [hdr][S09_QT_NOT_ARMED] in single I/O;
 *   RECORD (arg0 2, arg1 0): the slave's record of the last test reply,
 *     S09_QT_REC_WORDS words, single I/O.
 * Every 0x1C request disarms an armed test it does not send.
 */
#define S09_QT_ARM 0u
#define S09_QT_SEND 1u
#define S09_QT_RECORD 2u
#define S09_QT_OP(a) ((a) & 0xFu)
#define S09_QT_CAND(a) (((a) >> 4) & 3u)
#define S09_QT_REN 0x40u
#define S09_QT_PAT(a) (((a) >> 8) & 0xFu)
#define S09_QT_SEED(a) ((a) >> 16)
#define S09_QT_ARG0(cand, ren, pat, seed) \
	(S09_QT_ARM | ((uint32_t)(cand) & 3u) << 4 | ((ren) ? S09_QT_REN : 0u) | \
	 ((uint32_t)(pat) & 0xFu) << 8 | ((uint32_t)(seed) & 0xFFFFu) << 16)
/* The candidate endings (S-13b.084). */
#define S09_QT_TF_DMA 1u	/* TF, fed by the transmit DMA */
#define S09_QT_TF_CORE 2u	/* TF, fed by the core */
#define S09_QT_SPIF 3u		/* SPIF, a loop that reads SPI_STAT alone */
/* The patterns: a walking one, a walking zero, one data line high, one
 * line low, the lines alternating, pseudo-random words. */
#define S09_QT_PATS 6u
#define S09_QT_MIN 12u
#define S09_QT_MAX 0x10000u
/* ARM's and an unarmed SEND's value. */
#define S09_QT_ARMED 0u
#define S09_QT_BAD 1u		/* an argument out of range */
#define S09_QT_NOT_PLAIN 2u	/* the link is not in the plain mode */
#define S09_QT_NOT_ARMED 3u	/* SEND with no test armed */
#define S09_QT_NO_SLAVE 4u	/* the slave's build has no test */

/* RECORD's reply: these words, little-endian. Times are the slave's core
 * cycles from the hand-over of its ready line, S09_QT_NONE when not seen. */
enum {
	S09_QR_HDR,		/* the header word */
	S09_QR_NO,		/* test replies sent since the server started */
	S09_QR_ARM,		/* the arm's arg0 */
	S09_QR_LEN,		/* the reply's bytes */
	S09_QR_END,		/* S09_QE_... and S09_QF_... */
	S09_QR_STAT,		/* SPI_STAT at the end */
	S09_QR_DMA,		/* the transmit DMA's DMA_STAT at the end */
	S09_QR_TWC,		/* SPI_TWC at the end */
	S09_QR_STAT_OR,		/* SPI_STAT or-ed over the reply */
	S09_QR_STAT_BOUND,	/* SPI_STAT when the bound ran out, else 0 */
	S09_QR_HAND,		/* the cycle counter at the hand-over */
	S09_QR_T_START,		/* the master-ready pin first read low */
	S09_QR_T_FEED,		/* the last word fed; SPIF: SPIF's last fall */
	S09_QR_T_FLAG,		/* the candidate's flag; SPIF: its last rise */
	S09_QR_T_END,		/* the end: the flag taken, or the give-up */
	S09_QR_SPIF,		/* SPIF's falls, and its rises << 16 */
	S09_QR_RX,		/* words received with REN set */
	S09_QR_STAGE,		/* the driver's code (bits 31-8) and stage */
	S09_QT_REC_WORDS
};
#define S09_QT_REC_LEN (4u * S09_QT_REC_WORDS)
#define S09_QT_NONE 0xFFFFFFFFu
/* How a test reply ended (S09_QR_END bits 7-0). */
#define S09_QE_NONE 0u		/* no test reply sent */
#define S09_QE_FLAG 1u		/* the candidate's flag */
#define S09_QE_BOUND 2u		/* no flag within the bound */
#define S09_QE_MASTER 3u	/* the master-ready pin rose first */
#define S09_QE_TX_ERR 4u	/* a transmit underrun or collision */
#define S09_QE_DMA_ERR 5u	/* the transmit DMA reported an error */
#define S09_QE_NO_START 6u	/* the master never began */
#define S09_QE_REFUSED 7u	/* the driver refused the reply */
#define S09_QE_COUNT 8u
/* ... and what was seen (S09_QR_END bits 8 on). */
#define S09_QF_TF_STAT 0x100u	/* SPI_STAT.TF read set */
#define S09_QF_TF_ILAT 0x200u	/* SPI_ILAT.TF read set */
#define S09_QF_FED 0x400u	/* every word fed */
#define S09_QF_BEGAN 0x800u	/* the master-ready pin read low */
#define S09_QF_ENDED 0x1000u	/* ... and high again after the end */
#define S09_QF_SINGLE 0x2000u	/* the controller back in single I/O */

/* S-09.080: arm's arguments in range: an operation, a candidate, REN only
 * with a TF candidate, a pattern, and a length of whole words. */
bool s09_qt_args_ok(uint32_t arg0, uint32_t arg1);
/* Word j of a pattern's payload. */
uint32_t s09_qt_word(unsigned pat, uint32_t seed, uint32_t j);
/*
 * A test reply of n bytes for arg0's pattern and seed: the header word of
 * 0x1C in slot, the pattern's words, then the check word of every word
 * before it (s09_check_word(), whose tables the caller has built).
 */
void s09_qt_build(uint8_t *d, uint32_t n, uint8_t slot, uint32_t arg0);
/* The data lines a word's wrong bits were on, bit k for line k: a quad
 * clock carries a nibble, line k its bit k. */
static inline uint8_t s09_qt_lines(uint32_t diff)
{
	uint32_t l = 0;

	for (unsigned i = 0; i < 8u; i++)
		l |= diff >> (4u * i);
	return (uint8_t)(l & 0xFu);
}

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

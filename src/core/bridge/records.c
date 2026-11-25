/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09 bridge records.
 */
#include "waffle/bridge/records.h"
#include "waffle/util/bytes.h"
#include "waffle/testos.h"
#include "waffle/host/devhost.h"

/* S-09.023 */
int s09_kind(uint8_t cmd)
{
	switch (cmd) {
	case S09_INIT:
	case S09_ERASE_GROUP:
	case S09_BOOT_SIZE:
	case S09_RELEASE:
	case S09_ECHO:
	case S09_LINK_MODE:
	case S09_LINK_TEST:
	case S09_LINK_STATUS:
	case S09_QUAD_TEST:
	OPXY_TESTOS(case OPXY_TESTOS_ROW_REQ: case OPXY_TESTOS_PEEK_REQ: case OPXY_TESTOS_POKE_REQ:)
#if !defined(OPXY_TEST_OS)
	case OPXY_DEV_PEEK_REQ: case OPXY_DEV_POKE_REQ:
#endif
		return S09_K_IMMEDIATE;
	case S09_POLL:
		return S09_K_POLL;
	case S09_CLOCK_SYNC:
		return S09_K_CLOCK;
	case S09_PUSH0:
	case S09_PUSH1:
		return S09_K_PUSH;
	case S09_PULL:
		return S09_K_PULL;
	case S09_FLUSH:
	case S09_MOUNT:
	case S09_UNMOUNT:
	case S09_READ_SECTOR:
	case S09_READ_SECTORS:
	case S09_WRITE_RELIABLE:
	case S09_ERASE:
	case S09_WRITE_SECTOR:
	case S09_WRITE_SECTORS:
	case S09_SELECT:
	case S09_STAGE_WRITE:
		return S09_K_DEFERRED;
	default:
		if ((cmd >= S09_ACCESS && cmd <= S09_TOTAL_SPACE) ||
		    (cmd >= S09_FSYNC && cmd <= S09_FUTIMENS))
			return S09_K_DEFERRED;
		return S09_K_UNDEFINED;
	}
}

/* S-09.013 rule 4 */
bool s09_plain_safe(uint8_t cmd)
{
	switch (cmd) {
	case S09_ERASE_GROUP:
	case S09_BOOT_SIZE:
	case S09_READ_SECTOR:
	case S09_READ_SECTORS:
	case S09_SELECT:
	case S09_ACCESS:
	case S09_STAT:
	case S09_STATFS:
	case S09_FREE_SPACE:
	case S09_TOTAL_SPACE:
	case S09_QUAD_TEST:	/* S-09.080: it writes no storage */
		return true;
	default:
		return false;
	}
}

/* S-09.080 */
bool s09_qt_args_ok(uint32_t arg0, uint32_t arg1)
{
	const uint32_t cand = S09_QT_CAND(arg0);

	return S09_QT_OP(arg0) == S09_QT_ARM && cand >= S09_QT_TF_DMA &&
	       cand <= S09_QT_SPIF && (arg0 & 0x80u) == 0u &&
	       ((arg0 & S09_QT_REN) == 0u || cand != S09_QT_SPIF) &&
	       S09_QT_PAT(arg0) < S09_QT_PATS && (arg0 & 0xF000u) == 0u &&
	       arg1 >= S09_QT_MIN && arg1 <= S09_QT_MAX && arg1 % 4u == 0u;
}

uint32_t s09_qt_word(unsigned pat, uint32_t seed, uint32_t j)
{
	const uint32_t k = j + seed;
	uint32_t x;

	switch (pat) {
	case 0u:		/* a walking one */
		return 1u << (k % 32u);
	case 1u:		/* a walking zero */
		return ~(1u << (k % 32u));
	case 2u:		/* one data line high in every nibble */
		return 0x11111111u << (k % 4u);
	case 3u:		/* ... one low */
		return ~(0x11111111u << (k % 4u));
	case 4u:		/* adjacent lines at opposite levels */
		return (k & 1u) != 0u ? 0xAAAAAAAAu : 0x55555555u;
	default:		/* pseudo-random */
		x = k * 0x9E3779B9u ^ seed << 7;
		x ^= x >> 16;
		x *= 0x85EBCA6Bu;
		x ^= x >> 13;
		x *= 0xC2B2AE35u;
		return x ^ x >> 16;
	}
}

void s09_qt_build(uint8_t *d, uint32_t n, uint8_t slot, uint32_t arg0)
{
	const uint32_t words = n / 4u;

	wb_put_le32(d, s09_hdr_word(S09_QUAD_TEST, slot));
	for (uint32_t j = 0; j + 2u < words; j++)
		wb_put_le32(d + 4u + 4u * j,
			    s09_qt_word(S09_QT_PAT(arg0), S09_QT_SEED(arg0), j));
	wb_put_le32(d + n - 4u, s09_check_word(d, n - 4u));
}

/* S-09.020 */
void s09_req_pack(const struct s09_req *r, uint8_t out[S09_HDR])
{
	wb_put_le32(out, (uint32_t)r->cmd | (uint32_t)r->slot << 8 |
				 (uint32_t)r->len << 16);
	wb_put_le32(out + 4, r->arg0);
	wb_put_le32(out + 8, r->arg1);
}

void s09_req_unpack(const uint8_t in[S09_HDR], struct s09_req *r)
{
	uint32_t w = wb_get_le32(in);

	r->cmd = (uint8_t)w;
	r->slot = (uint8_t)(w >> 8);
	r->len = (uint16_t)(w >> 16);
	r->arg0 = wb_get_le32(in + 4);
	r->arg1 = wb_get_le32(in + 8);
}

/* S-09.021, S-16.112 */
uint32_t s09_master_payload_wire(uint32_t n)
{
	uint32_t p = s09_pad4(n);

	return p == 0x44u || p == 0x48u ? 0x4Cu : p;
}

typedef uint32_t __attribute__((may_alias)) word;

/* The words of a session, as they lie when the buffer is word-aligned on a
 * little-endian host, read from their bytes otherwise. */
static inline uint32_t word_at(const uint8_t *d, uint32_t i)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	if (((uintptr_t)d & 3u) == 0u)
		return ((const word *)(const void *)d)[i / 4u];
#endif
	return wb_get_le32(d + i);
}

/*
 * S-09.072: the check a whole wire word a step (slicing by four):
 * s09_check_t[k][i] is the register after shifting in the byte i, most
 * significant bit first, then k zero bytes, with the polynomial
 * 0x04C11DB7. Built, not stored in the image, by s09_check_ready().
 */
#define CHECK_POLY 0x04C11DB7u

uint32_t s09_check_t[4][256];
void (*s09_check_built)(void);

/* The register after shifting in the byte i alone. */
static uint32_t check_byte(uint32_t i)
{
	uint32_t c = i << 24;

	for (unsigned b = 0; b < 8u; b++)
		c = (c & 0x80000000u) != 0u ? c << 1 ^ CHECK_POLY : c << 1;
	return c;
}

static uint32_t check_next(unsigned k, uint32_t i)
{
	return s09_check_t[k - 1][i] << 8 ^ s09_check_t[0][s09_check_t[k - 1][i] >> 24];
}

static void check_build(void)
{
	for (uint32_t i = 0; i < 256u; i++)
		s09_check_t[0][i] = check_byte(i);
	for (unsigned k = 1; k < 4u; k++)
		for (uint32_t i = 0; i < 256u; i++)
			s09_check_t[k][i] = check_next(k, i);
}

/* Every entry read back as it should be. */
static bool check_entries(void)
{
	bool ok = true;

	for (uint32_t i = 0; i < 256u; i++) {
		ok = ok && s09_check_t[0][i] == check_byte(i);
		for (unsigned k = 1; k < 4u; k++)
			ok = ok && s09_check_t[k][i] == check_next(k, i);
	}
	return ok;
}

/* The known answers: every entry of the four tables read back as its
 * rule gives it, then the eight bytes "12345678" as the wire sends them,
 * whose CRC-32/BZIP2 is 0xB61C3D04 (worked out independently; the
 * catalogue's own check string is the nine bytes "123456789"), aligned
 * and not, and 512 bytes that pass through all four tables. */
#define KNOWN_LONG 512u
static _Alignas(4) uint8_t known[KNOWN_LONG];

bool s09_check_ready(void)
{
	static const uint8_t mem8[8] = { '4', '3', '2', '1', '8', '7', '6', '5' };
	_Alignas(4) uint8_t odd[12];

	check_build();
	if (s09_check_built != NULL)
		s09_check_built();
	for (uint32_t i = 0; i < KNOWN_LONG; i++)
		known[i] = (uint8_t)(i * 151u + 7u);
	wb_copy(odd + 1, mem8, 8);	/* the path for a buffer not aligned */
	return check_entries() && s09_check_word(mem8, 8) == 0xB61C3D04u &&
	       s09_check_word(odd + 1, 8) == 0xB61C3D04u &&
	       s09_check_word(known, KNOWN_LONG) == 0xE1A0F4D3u;
}

/* One word, as the wire carries it: the register xor the word, its four
 * bytes shifted out through the four tables at once. */
static inline uint32_t check_step(uint32_t c, uint32_t w)
{
	c ^= w;
	return s09_check_t[3][c >> 24] ^ s09_check_t[2][c >> 16 & 0xFFu] ^
	       s09_check_t[1][c >> 8 & 0xFFu] ^ s09_check_t[0][c & 0xFFu];
}

uint32_t s09_check_word(const uint8_t *d, uint32_t n)
{
	uint32_t c = 0xFFFFFFFFu, i = 0;

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	/* a session's buffer is word-aligned: its words as they lie */
	if (((uintptr_t)d & 3u) == 0u) {
		const word *w = (const word *)(const void *)d;

		for (; i + 4u <= n; i += 4u)
			c = check_step(c, *w++);
	}
#endif
	for (; i + 4u <= n; i += 4u)
		c = check_step(c, wb_get_le32(d + i));
	return ~c;
}


uint32_t s09_trailer(uint8_t *d, uint32_t body, uint32_t seq,
		     uint32_t (*check)(void *ctx, const uint8_t *d, size_t n),
		     void *ctx)
{
	uint32_t c;

	wb_put_le32(d + body, seq);
	c = check != NULL ? check(ctx, d, body + 4u) : s09_check_word(d, body + 4u);
	wb_put_le32(d + body + 4u, c);
	return c;
}

/* S-09.071: the link test's value */
#define TEST_START 0x61C88646u

uint32_t s09_test_value(const uint8_t *d, uint32_t n)
{
	uint32_t a = TEST_START, b = n;

	for (uint32_t i = 0; i + 4u <= n; i += 4u) {
		a += word_at(d, i);
		b += a;
	}
	return a ^ (b << 16 | b >> 16);
}

/* S-09.023: len is the payload length, with the table's exceptions */
uint32_t s09_payload_len(const struct s09_req *r)
{
	switch (r->cmd) {
	case S09_WRITE_RELIABLE:
	case S09_WRITE_SECTORS:
		/* from arg1: 128 sectors do not fit len's 16 bits. A count
		 * outside 1-128 makes a malformed request, whose arg1 x 512
		 * bytes are discarded, not kept (s09_payload_discard()) */
		return s09_count_ok(r) ? r->arg1 * S09_SECTOR : 0u;
	case S09_READ_SECTORS:
		return s09_count_ok(r) ? 0u : r->len;	/* S-09.037 */
	case S09_WRITE_SECTOR:
		return S09_SECTOR;
	case S09_PUSH0:
	case S09_PUSH1:
	case S09_ACCESS:
	case S09_STAT:
	case S09_STATFS:
	case S09_RENAME:
	case S09_UNLINK:
	case S09_MKDIR:
	case S09_RMDIR:
	case S09_SYNC:
	case S09_OPEN:
	case S09_OPENDIR:
	case S09_FREE_SPACE:
	case S09_TOTAL_SPACE:
	case S09_UTIME:
		return r->len;
	case S09_WRITE:
	case S09_STAGE_WRITE:	/* S-09.084: as a file write */
		return r->arg1;
	case S09_LINK_TEST:
		/* S-09.037: a length outside 4-S09_TEST_MAX has no payload */
		return r->arg1 >= 4u && r->arg1 <= S09_TEST_MAX ? r->arg1 : 0u;
	default:
		/* S-09.037: an undefined command's payload is pad4(len) */
		return s09_kind(r->cmd) == S09_K_UNDEFINED ? (uint32_t)r->len : 0u;
	}
}

/* S-09.037: a malformed write's payload is taken from arg1 as S-09.023 says
 * (len carries only its length mod 2^16), and none for a count of 0 */
uint64_t s09_payload_discard(const struct s09_req *r)
{
	if ((r->cmd == S09_WRITE_RELIABLE || r->cmd == S09_WRITE_SECTORS) &&
	    !s09_count_ok(r))
		return (uint64_t)r->arg1 * S09_SECTOR;
	return 0;
}

/* S-09.032 */
long s09_deferred_data_len(uint8_t cmd, uint32_t arg1)
{
	switch (cmd) {
	case S09_READ_SECTOR:
		return S09_SECTOR;
	case S09_READ_SECTORS:
		return (long)(arg1 * S09_SECTOR);
	case S09_STAT:
		return S09_STAT_LEN;
	case S09_STATFS:
		return S09_STATFS_LEN;
	case S09_READDIR:
		return S09_DIRENT_LEN;
	case S09_FREE_SPACE:
	case S09_TOTAL_SPACE:
		return 8;
	case S09_READ:
	case S09_PULL:
		return -1;
	default:
		return s09_kind(cmd) == S09_K_DEFERRED ? 0 : -1;
	}
}

/* S-09.043 */
void s09_stat_pack(const struct s09_stat *s, uint8_t out[S09_STAT_LEN])
{
	const uint32_t v[13] = { s->dev, s->ino, s->mode, s->nlink, s->uid,
				 s->gid, s->rdev, s->size, s->blksize, s->blocks,
				 s->atime, s->mtime, s->ctime };

	for (unsigned i = 0; i < 13; i++)
		wb_put_le32(out + 4 * i, v[i]);
}

void s09_stat_unpack(const uint8_t in[S09_STAT_LEN], struct s09_stat *s)
{
	uint32_t *v[13] = { &s->dev, &s->ino, &s->mode, &s->nlink, &s->uid,
			    &s->gid, &s->rdev, &s->size, &s->blksize, &s->blocks,
			    &s->atime, &s->mtime, &s->ctime };

	for (unsigned i = 0; i < 13; i++)
		*v[i] = wb_get_le32(in + 4 * i);
}

void s09_statfs_pack(const struct s09_statfs *s, uint8_t out[S09_STATFS_LEN])
{
	wb_put_le32(out, s->bsize);
	wb_put_le32(out + 4, s->blocks);
	wb_put_le32(out + 8, s->bfree);
}

void s09_statfs_unpack(const uint8_t in[S09_STATFS_LEN], struct s09_statfs *s)
{
	s->bsize = wb_get_le32(in);
	s->blocks = wb_get_le32(in + 4);
	s->bfree = wb_get_le32(in + 8);
}

void s09_dirent_pack(const struct s09_dirent *d, uint8_t out[S09_DIRENT_LEN])
{
	size_t i;

	wb_fill(out, 0, S09_DIRENT_LEN);
	wb_put_le32(out, d->ino);
	wb_put_le32(out + 4, d->off);
	wb_put_le16(out + 8, (uint16_t)S09_DIRENT_LEN);
	out[0x0A] = d->type;
	/* NUL-terminated, zero-filled: at most 256 name bytes */
	for (i = 0; i + 1 < S09_DIRENT_NAME && d->name[i]; i++)
		out[0x0B + i] = (uint8_t)d->name[i];
}

void s09_dirent_unpack(const uint8_t in[S09_DIRENT_LEN], struct s09_dirent *d)
{
	d->ino = wb_get_le32(in);
	d->off = wb_get_le32(in + 4);
	d->type = in[0x0A];
	for (size_t i = 0; i < S09_DIRENT_NAME; i++)
		d->name[i] = (char)in[0x0B + i];
	d->name[S09_DIRENT_NAME - 1] = '\0';
}

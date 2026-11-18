/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The package's two bulk CRCs on the processor's CRC peripheral (CRC0 and
 * memory DMA stream 1, src/drivers/bf70x/crc.c): CRC-16/XMODEM over every
 * piece of a package (S-11.008, check 6) and CRC-32 over every extracted
 * member (S-11.030). The software (wire_crc.h) computes the unaligned head
 * and tail of a call, every call shorter than LEAST, and everything after
 * a failure; it is also the reference the peripheral is tested against.
 *
 * The peripheral's conventions are found, not assumed. opxy_crc_begin()
 * computes each CRC in software over three runs of a fixed buffer (1 KiB
 * from the CRC's start, 516 bytes from a running value that is not zero,
 * 68 bytes from the first run's result) and tries the peripheral's options
 * (BITMIRR, BYTMIRR, W16SWP, POLYMIRR: 16 sets) with each of four
 * conversions of the running value, applied going in and undone coming out
 * (bit-reversed or not, complemented or not), until one set and conversion
 * gives the software's answer to all three. At worst 16 x 4 x 3 = 192
 * operations of at most 1 KiB for each CRC, some milliseconds; an operation
 * that fails ends the search at once, after at most its own wait
 * (WB_CRC_LUT_US, or WB_CRC_TIMEOUT_US and 64 us for the data). It runs at
 * each install and each timing request.
 *
 * A CRC whose search finds nothing, or whose operation times out or reports
 * a DMA error, is the software's for the rest of the boot (opxy_crc_off).
 * The piece that failed is computed again in software, so its result is
 * right. These pieces the peripheral computes are also computed in
 * software, so a fault tied to a shape is met while the bytes are in hand:
 * every CHECK_EVERY-th, from the first; the first of each DMA transfer size
 * the driver picks from the address and length (wb_crc_msize); and, for the
 * CRC-16 only, every one whose length differs from the one before it. The
 * CRC-32 goes without that last rule because a member's CRC-32 is computed
 * again in software before it refuses anything (below), while a deflated
 * member's pieces change length at nearly every call and the rule would
 * hand most of them to the software. A difference turns the peripheral off
 * for that CRC for the boot, and the software's result is the one returned.
 *
 * Their cost: check 6 reads the package in pieces of 32 KiB, so beyond the
 * first piece only the last, shorter one is compared, at most 32 KiB in
 * software (1.3 ms at 25 MiB/s) per package. For the CRC-32 it is the first
 * piece of each of the four transfer sizes and every 64th piece.
 *
 * What stands behind a piece between those, when the peripheral gives a
 * wrong answer with a good status (s11_engine.h, set while the use lasts):
 * - A member's CRC-32 that does not match, with a piece of it computed here
 *   unchecked, turns the peripheral off for CRC-32 and the member is
 *   extracted again with the software's CRC-32. A member is never refused
 *   for the peripheral's fault, and every verdict is the software's.
 * - Check 6 cannot do the same: the zip is decrypted in place in the pass
 *   that computes its CRC-16 (S-11.018), so the bytes are gone when it
 *   fails. A check-6 refusal with a piece of the CRC-16 computed here
 *   unchecked turns the peripheral off for CRC-16 and logs it, so the same
 *   package copied again is judged by the software. The peripheral can
 *   refuse a good package at check 6 once in a boot, never twice.
 * - A bad package is never installed: check 11's SHA-256 covers every byte
 *   check 6 does, and the members are extracted from bytes it covered.
 *
 * Between operations CRC0 is disabled and DMA18 stopped; opxy_crc_end()
 * leaves them as reset does. S-05.016's CRC engine row is about CRC1, which
 * nothing here touches; CRC0 ends the same way, and the hand-over's state
 * does not depend on this module having run.
 */
#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_services.h"
#include "waffle/drivers/bf70x_crc.h"
#include "waffle/s11_engine.h"
#include "waffle/s14_log.h"
#include "waffle/wire_crc.h"

#define LEAST 0x400u		/* below this the set-up costs more */
#define CHECK_EVERY 64u
#define TEST_LEN 1024u
#define RUNS 3u
#define CONV_REV 1u
#define CONV_NOT 2u
#define CONVS 4u
#define OPTION_SETS 16u

struct wb_crc opxy_crc_dev;
uint8_t opxy_crc_off;

/* As the driver takes them: CRC-16/XMODEM's 0x1021 at the top of 32 bits
 * (its CRC is the top half of this one's), CRC-32's reflected form. */
static const uint32_t poly[OPXY_CRCS] = { 0x10210000u, 0xEDB88320u };
static const uint32_t seed[OPXY_CRCS] = { 0x1D0Fu, 0x9E3779B9u };
static const struct {
	uint16_t off, len;
} runs[RUNS] = { { 0, TEST_LEN }, { 4, 516 }, { 520, 68 } };

static struct {
	bool on;
	uint32_t opts;
	unsigned conv;
	uint32_t pieces;
	uint32_t last;			/* the length of the last piece */
	uint8_t sizes;			/* the transfer sizes met, bit (1 << msize) */
	bool unchecked;			/* a piece's result used uncompared */
} use[OPXY_CRCS];
static bool touched;		/* registers written since the last end */
static struct opxy_master *owner;	/* the use's, for its log */
static _Alignas(4) uint8_t test[TEST_LEN];

static uint32_t soft(unsigned which, uint32_t v, const uint8_t *p, size_t n)
{
	return which == OPXY_CRC16 ? wire_s11_crc16(NULL, (uint16_t)v, p, n) :
				     wire_s11_crc32(NULL, v, p, n);
}

static uint32_t rev32(uint32_t x)
{
	uint32_t r = 0;

	for (unsigned i = 0; i < 32u; i++, x >>= 1)
		r = r << 1 | (x & 1u);
	return r;
}

/* Its own inverse: a complement and a reversal commute. */
static uint32_t convert(uint32_t x, unsigned conv)
{
	if ((conv & CONV_NOT) != 0u)
		x = ~x;
	return (conv & CONV_REV) != 0u ? rev32(x) : x;
}

static uint32_t options(unsigned set)
{
	return ((set & 1u) != 0u ? CRC_CTL_BITMIRR : 0u) |
	       ((set & 2u) != 0u ? CRC_CTL_BYTMIRR : 0u) |
	       ((set & 4u) != 0u ? CRC_CTL_W16SWP : 0u) |
	       ((set & 8u) != 0u ? CRC_CTL_POLYMIRR : 0u);
}

/* The CRC of len aligned bytes at p continued from v, on the peripheral. */
static int engine(unsigned which, uint32_t opts, unsigned conv, uint32_t v,
		  const uint8_t *p, uint32_t len, uint32_t *out)
{
	uint32_t s = convert(which == OPXY_CRC16 ? v << 16 : v, conv);
	const int rc = wb_crc_run(&opxy_crc_dev, poly[which], opts, &s, p, len);

	s = convert(s, conv);
	*out = which == OPXY_CRC16 ? s >> 16 : s;
	return rc;
}

static void turn_off(unsigned which)
{
	use[which].on = false;
	opxy_crc_off |= (uint8_t)(1u << which);
}

/* The runs' starting values and the software's answers. */
static void wanted(unsigned which, uint32_t from[RUNS], uint32_t want[RUNS])
{
	for (unsigned k = 0; k < RUNS; k++) {
		from[k] = k == 0u ? 0u : k == 1u ? seed[which] : want[0];
		want[k] = soft(which, from[k], test + runs[k].off, runs[k].len);
	}
}

/* 1 when an option set and conversion answer all the runs as the software
 * does, 0 when one does not, -1 when an operation fails. */
static int answers(unsigned which, uint32_t opts, unsigned conv,
		   const uint32_t from[RUNS], const uint32_t want[RUNS])
{
	for (unsigned k = 0; k < RUNS; k++) {
		uint32_t r;

		if (engine(which, opts, conv, from[k], test + runs[k].off,
			   runs[k].len, &r) != WB_CRC_OK)
			return -1;
		if (r != want[k])
			return 0;
	}
	return 1;
}

/* The first option set and conversion that answer all the runs as the
 * software does; false when none does or an operation fails. */
static bool search(unsigned which)
{
	uint32_t want[RUNS], from[RUNS];

	wanted(which, from, want);
	for (unsigned set = 0; set < OPTION_SETS; set++) {
		for (unsigned conv = 0; conv < CONVS; conv++) {
			const int a = answers(which, options(set), conv, from, want);

			if (a < 0)
				return false;
			if (a > 0) {
				use[which].opts = options(set);
				use[which].conv = conv;
				return true;
			}
		}
	}
	return false;
}

/* Whether the software computes this piece too: every CHECK_EVERY-th, the
 * first of each transfer size and, for the CRC-16, one whose length is not
 * the last's. */
static bool compared(unsigned which, const uint8_t *p, uint32_t len)
{
	const uint8_t bit = (uint8_t)(1u << wb_crc_msize((uint32_t)(uintptr_t)p,
							 len));
	const bool c = use[which].pieces++ % CHECK_EVERY == 0u ||
		       (use[which].sizes & bit) == 0u ||
		       (which == OPXY_CRC16 && len != use[which].last);

	use[which].sizes |= bit;
	use[which].last = len;
	return c;
}

static uint32_t run(unsigned which, uint32_t v, const uint8_t *p, size_t n)
{
	while (use[which].on && n >= LEAST) {
		const size_t head = (4u - ((uintptr_t)p & 3u)) & 3u;
		uint32_t len, r;

		if (head != 0u) {
			v = soft(which, v, p, head);
			p += head;
			n -= head;
			continue;
		}
		len = n > WB_CRC_MAX ? WB_CRC_MAX : (uint32_t)n & ~3u;
		if (engine(which, use[which].opts, use[which].conv, v, p, len,
			   &r) != WB_CRC_OK) {
			turn_off(which);
			break;		/* this piece in software, below */
		}
		if (compared(which, p, len)) {
			const uint32_t want = soft(which, v, p, len);

			if (r != want) {
				turn_off(which);
				r = want;
			}
		} else {
			use[which].unchecked = true;
		}
		v = r;
		p += len;
		n -= len;
	}
	return soft(which, v, p, n);
}

uint16_t opxy_crc16(void *ctx, uint16_t crc, const uint8_t *p, size_t n)
{
	(void)ctx;
	return (uint16_t)run(OPXY_CRC16, crc, p, n);
}

uint32_t opxy_crc32(void *ctx, uint32_t crc, const uint8_t *p, size_t n)
{
	(void)ctx;
	return run(OPXY_CRC32, crc, p, n);
}

/* Whether a piece of this CRC was the peripheral's, uncompared, since the
 * use began; the CRC is the software's from then on. */
static bool replaced(unsigned which)
{
	if (!use[which].unchecked)
		return false;
	use[which].unchecked = false;
	turn_off(which);
	return true;
}

static bool crc32_again(void)
{
	return replaced(OPXY_CRC32);
}

static void crc16_refused(void)
{
	if (replaced(OPXY_CRC16) && owner != NULL)
		s14_log_str(&owner->sv.log, S14_WARNING, S14_NO_TAG,
			    "crc: check 6 failed on the CRC peripheral; "
			    "CRC-16 in software until a restart");
}

static const struct s11_crc_engine hooks = { crc32_again, crc16_refused };

void opxy_crc_begin(struct opxy_master *m)
{
	wb_crc_init(&opxy_crc_dev, &m->hw.time);
	owner = m;
	s11_crc_engine = &hooks;
	for (unsigned w = 0; w < OPXY_CRCS; w++) {
		use[w].on = false;
		use[w].pieces = 0;
		use[w].last = 0;
		use[w].sizes = 0;
		use[w].unchecked = false;
	}
	/* DMA18 in someone else's use is left alone */
	if (opxy_crc_off == (1u << OPXY_CRCS) - 1u ||
	    wb_dma_enabled(WB_CRC_DMA))
		return;
	for (unsigned i = 0; i < TEST_LEN; i++)
		test[i] = (uint8_t)(i * 151u + (i >> 3) * 7u + 0x3Cu);
	touched = true;
	for (unsigned w = 0; w < OPXY_CRCS; w++) {
		if ((opxy_crc_off & (1u << w)) != 0u)
			continue;
		use[w].on = search(w);
		if (!use[w].on)
			turn_off(w);
	}
}

/*
 * A CRC-32 checked against a known value (S-02.016's slot check, S-03.044's
 * OS image): s02_env's and s03_env's crc32_fast, on either processor. The
 * caller takes a result equal to the known value as the verdict and computes
 * any other again in software (s02_image_check(), s03_load_os()), so a wrong
 * answer of the peripheral costs time, never a verdict, and no piece is
 * compared here. A use of its own, begun and ended in the call: the
 * conventions found as opxy_crc_begin() finds them, the ones the last call
 * found tried first; CRC0 and DMA18 left as reset does at the return. The
 * software's alone during another use (an install's), for fewer than LEAST
 * bytes, with DMA18 in someone else's use, and once the CRC-32 is off for
 * the boot; a failed operation turns it off and the rest is the software's.
 * idle, when not NULL, runs between pieces of FAST_PIECE bytes.
 */
#define FAST_PIECE 0x10000u

static struct {
	bool known;
	uint32_t opts;
	unsigned conv;
} fast_found;
/* Whether the last call's result was the peripheral's, for the timing. */
bool opxy_crc_fast_used;

static uint32_t soft_pieces(uint32_t v, const uint8_t *p, size_t n, void (*idle)(void))
{
	while (n != 0u) {
		const size_t k = n < FAST_PIECE ? n : FAST_PIECE;

		v = soft(OPXY_CRC32, v, p, k);
		p += k;
		n -= k;
		if (idle != NULL)
			idle();
	}
	return v;
}

/* The CRC-32's conventions for this call: the last call's when they still
 * answer the runs, else the search's. */
static bool fast_ready(void)
{
	for (unsigned i = 0; i < TEST_LEN; i++)
		test[i] = (uint8_t)(i * 151u + (i >> 3) * 7u + 0x3Cu);
	if (fast_found.known) {
		uint32_t want[RUNS], from[RUNS];

		wanted(OPXY_CRC32, from, want);
		switch (answers(OPXY_CRC32, fast_found.opts, fast_found.conv, from, want)) {
		case 1:
			use[OPXY_CRC32].opts = fast_found.opts;
			use[OPXY_CRC32].conv = fast_found.conv;
			return true;
		case 0:
			break;
		default:
			return false;
		}
	}
	return search(OPXY_CRC32);
}

uint32_t opxy_crc32_fast(uint32_t crc, const uint8_t *p, uint32_t n,
			 const struct wb_time *t, void (*idle)(void))
{
	uint32_t v = crc;
	size_t left = n;

	opxy_crc_fast_used = false;
	if (n < LEAST || owner != NULL || touched ||
	    (opxy_crc_off & (1u << OPXY_CRC32)) != 0u || wb_dma_enabled(WB_CRC_DMA))
		return soft_pieces(crc, p, n, idle);
	wb_crc_init(&opxy_crc_dev, t);
	touched = true;
	fast_found.known = fast_ready();
	if (fast_found.known) {
		fast_found.opts = use[OPXY_CRC32].opts;
		fast_found.conv = use[OPXY_CRC32].conv;
		opxy_crc_fast_used = true;
	} else {
		turn_off(OPXY_CRC32);
	}
	while (fast_found.known && left >= LEAST) {
		const size_t head = (4u - ((uintptr_t)p & 3u)) & 3u;
		uint32_t len, r;

		if (head != 0u) {
			v = soft(OPXY_CRC32, v, p, head);
			p += head;
			left -= head;
			continue;
		}
		len = left > FAST_PIECE ? FAST_PIECE : (uint32_t)left & ~3u;
		if (engine(OPXY_CRC32, use[OPXY_CRC32].opts, use[OPXY_CRC32].conv, v, p, len,
			   &r) != WB_CRC_OK) {
			fast_found.known = false;
			opxy_crc_fast_used = false;
			turn_off(OPXY_CRC32);
			break;		/* this piece in software, below */
		}
		v = r;
		p += len;
		left -= len;
		if (idle != NULL)
			idle();
	}
	wb_crc_end(&opxy_crc_dev);
	touched = false;
	return soft_pieces(v, p, left, idle);
}

bool opxy_crc_engine(unsigned which)
{
	return which < OPXY_CRCS && use[which].on;
}

void opxy_crc_end(void)
{
	if (touched)
		wb_crc_end(&opxy_crc_dev);
	touched = false;
	for (unsigned w = 0; w < OPXY_CRCS; w++)
		use[w].on = false;
	if (s11_crc_engine == &hooks)
		s11_crc_engine = NULL;
	owner = NULL;
}

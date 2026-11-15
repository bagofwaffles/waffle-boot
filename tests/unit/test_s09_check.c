/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09.072: the check of a request session outside the plain mode, and
 * what it is guaranteed to catch at the largest request: 12 + 0x8000 + 4
 * bytes before it (a file write with its number), N = 262,272 bits.
 *
 * The check is CRC-32 with the polynomial g = 0x104C11DB7 over the bits in
 * the order the wire carries them (S-13b.040: 32-bit words, most
 * significant bit first). It is affine: the check of d ^ e is the check of
 * d xor the CRC of e from a zero register with no final XOR, so an error
 * pattern e is missed exactly when g divides e(x), the error read as a
 * polynomial with the wire's first bit highest. Each guarantee below is
 * checked against that, and the model itself against s09_check_word().
 */
#include "waffle/s09_bridge.h"

#include "s06_test.h"

#define G 0x04C11DB7u
#define BODY (S09_HDR + S09_WRITE_MAX + 4u)	/* the largest request, its number */
#define NBITS (BODY * 8u)

/* x^k mod g for k = 0 .. KMAX - 1 */
#define KMAX (NBITS + 64u)
static uint32_t pw[KMAX];

static void powers(void)
{
	pw[0] = 1;
	for (uint32_t k = 1; k < KMAX; k++)
		pw[k] = pw[k - 1] << 1 ^ (pw[k - 1] & 0x80000000u ? G : 0u);
}

/* The bit at wire index p (0 the first) of a BODY-byte request: word
 * p / 32, bit 31 - p % 32 of it, little-endian in memory. */
static void flip(uint8_t *d, uint32_t p)
{
	const uint32_t bit = 31u - p % 32u;

	d[(p / 32u) * 4u + bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
}

/* The check's difference for an error at wire indices p[0..n): the model. */
static uint32_t syndrome(const uint32_t *p, unsigned n)
{
	uint32_t s = 0;

	for (unsigned i = 0; i < n; i++)
		s ^= pw[NBITS - 1u - p[i] + 32u];
	return s;
}

static uint8_t req[BODY];

static void fill(uint32_t seed)
{
	for (uint32_t i = 0; i < BODY; i++) {
		seed = seed * 1103515245u + 12345u;
		req[i] = (uint8_t)(seed >> 16);
	}
}

/* The words' bytes as the wire sends them, the eight bytes "12345678",
 * against their CRC-32/BZIP2 worked out independently (the catalogue's
 * check string is the nine bytes "123456789", value 0xFC891918). */
static void test_reference(void)
{
	static const uint8_t mem[8] = { '4', '3', '2', '1', '8', '7', '6', '5' };
	uint8_t d[64];

	t_case = "reference";
	CHECK_EQ(s09_check_word(mem, 8), 0xB61C3D04u);
	for (unsigned i = 0; i < 64u; i++)
		d[i] = (uint8_t)((i & ~3u) + 3u - (i & 3u));
	CHECK_EQ(s09_check_word(d, 64), 0x4342F70Au);
	/* unaligned the same */
	{
		uint8_t u[65];

		memcpy(u + 1, d, 64);
		CHECK_EQ(s09_check_word(u + 1, 64), 0x4342F70Au);
	}
}

/* The reference: the check a byte a step, through one table, as it was
 * first written; the four-table check must give the same value. */
static uint32_t ref_table[256];

static uint32_t ref_check(const uint8_t *d, uint32_t n)
{
	uint32_t c = 0xFFFFFFFFu;

	if (ref_table[1] == 0u) {
		for (uint32_t i = 0; i < 256u; i++) {
			uint32_t t = i << 24;

			for (unsigned b = 0; b < 8u; b++)
				t = (t & 0x80000000u) != 0u ? t << 1 ^ G : t << 1;
			ref_table[i] = t;
		}
	}
	for (uint32_t i = 0; i + 4u <= n; i += 4u) {
		const uint32_t w = (uint32_t)d[i] | (uint32_t)d[i + 1] << 8 |
				   (uint32_t)d[i + 2] << 16 | (uint32_t)d[i + 3] << 24;

		c = c << 8 ^ ref_table[(c >> 24 ^ w >> 24) & 0xFFu];
		c = c << 8 ^ ref_table[(c >> 24 ^ w >> 16) & 0xFFu];
		c = c << 8 ^ ref_table[(c >> 24 ^ w >> 8) & 0xFFu];
		c = c << 8 ^ ref_table[(c >> 24 ^ w) & 0xFFu];
	}
	return ~c;
}

static void test_against_reference(void)
{
	static uint8_t buf[BODY + 8];
	uint32_t seed = 77, differ = 0;

	t_case = "against the reference";
	for (uint32_t k = 0; k < 4000u; k++) {
		uint32_t n, off;

		seed = seed * 1103515245u + 12345u;
		n = k < 3000u ? (seed >> 8) % 1025u * 4u : BODY - (seed >> 8) % 64u * 4u;
		off = k % 4u;
		for (uint32_t i = 0; i < n; i++) {
			seed = seed * 1103515245u + 12345u;
			buf[off + i] = k % 7u == 0u ? (uint8_t)(k & 1u ? 0xFF : 0) :
						      (uint8_t)(seed >> 16);
		}
		differ += s09_check_word(buf + off, n) != ref_check(buf + off, n);
	}
	CHECK_EQ(differ, 0u);
	CHECK_EQ(s09_check_word(buf, 0), ref_check(buf, 0));
	CHECK_EQ(s09_check_word(buf + 1, 6), ref_check(buf + 1, 6));	/* a word and a part */
}

/* The model agrees with the check for errors of several shapes. */
static void test_model(void)
{
	uint32_t base, p[3];

	t_case = "model";
	fill(1);
	base = s09_check_word(req, BODY);
	for (uint32_t k = 0; k < 2000u; k++) {
		p[0] = (k * 2654435761u) % NBITS;
		p[1] = (p[0] + 1u + k * 7u) % NBITS;
		p[2] = (k * 40503u + 17u) % NBITS;
		for (unsigned i = 0; i < 3u; i++)
			flip(req, p[i]);
		CHECK_EQ(s09_check_word(req, BODY) ^ base, syndrome(p, 3));
		for (unsigned i = 0; i < 3u; i++)
			flip(req, p[i]);
	}
}

/* Every error of one or two bits: x^k mod g is never 0, and never 1 for
 * 0 < k < N (so no x^a + x^b). */
static void test_one_two_bits(void)
{
	unsigned bad = 0;

	t_case = "one and two bits";
	for (uint32_t k = 1; k < KMAX; k++)
		bad += pw[k] == 0u || pw[k] == 1u;
	CHECK_EQ(bad, 0u);
}

/*
 * Every error of three bits within a span of 91,638 bits: 1 + x^a + x^b
 * is not 0 mod g for 0 < a < b <= 91,638. A hash of x^k for k up to the
 * span, and for each a a look-up of 1 + x^a. The first such codeword is at
 * b = 91,639 (shown: the guarantee is exact); the largest request is
 * longer, so three bits further apart than that can go unseen.
 */
#define SPAN3 91639u
#define HBITS 18u
static uint32_t hkey[1u << HBITS], hval[1u << HBITS];

static void put(uint32_t key, uint32_t val)
{
	uint32_t h = key * 2654435761u >> (32u - HBITS);

	while (hval[h] != 0u)
		h = (h + 1u) & ((1u << HBITS) - 1u);
	hkey[h] = key;
	hval[h] = val;
}

static uint32_t get(uint32_t key)
{
	uint32_t h = key * 2654435761u >> (32u - HBITS);

	while (hval[h] != 0u) {
		if (hkey[h] == key)
			return hval[h];
		h = (h + 1u) & ((1u << HBITS) - 1u);
	}
	return 0;
}

static void test_three_bits(void)
{
	uint32_t first = 0;

	t_case = "three bits";
	for (uint32_t k = 1; k <= SPAN3; k++)
		put(pw[k], k);
	for (uint32_t a = 1; a < SPAN3; a++) {
		const uint32_t b = get(pw[a] ^ 1u);

		if (b > a && (first == 0u || b < first))
			first = b;
	}
	CHECK_EQ(first, SPAN3);
}

/* Every burst of 32 bits or fewer in wire order: so every error within one
 * word, and in single I/O every error confined to 32 consecutive clocks.
 * B(x) of degree under 32 is not divisible by g; tried on the check at
 * every alignment of a word, with every burst length. */
static void test_bursts(void)
{
	uint32_t base;
	unsigned missed = 0;

	t_case = "bursts";
	fill(2);
	base = s09_check_word(req, BODY);
	for (uint32_t start = 0; start < 64u; start++) {
		for (uint32_t len = 1; len <= 32u; len++) {
			const uint32_t at = 8000u * 32u + start;

			flip(req, at);
			if (len > 1u)
				flip(req, at + len - 1u);	/* its ends set */
			for (uint32_t i = 1; i + 1u < len; i += 3u)
				flip(req, at + i);
			missed += s09_check_word(req, BODY) == base;
			fill(2);
		}
	}
	CHECK_EQ(missed, 0u);
}

/* The rank over GF(2) of n syndromes: n when no nonzero pattern on those
 * positions goes unseen. */
static unsigned rank(const uint32_t *v, unsigned n)
{
	uint32_t basis[32];
	unsigned r = 0;

	for (unsigned i = 0; i < n; i++) {
		uint32_t x = v[i];

		for (unsigned j = 0; j < r; j++)
			if ((x ^ basis[j]) < x)
				x ^= basis[j];
		if (x != 0u && r < 32u)
			basis[r++] = x;
	}
	return r;
}

/*
 * Quad I/O: line k carries bit 4j + k of each nibble (or, if the hardware
 * puts them the other way, 4j + 3 - k: the same set of lines), the clock c
 * the wire bits 4c .. 4c + 3. Any error confined to one data line within
 * 32 consecutive clocks is seen, at every phase, on every line: g is
 * irreducible and the error is E(x^4) = E(x)^4. Across a whole request no
 * 32-bit check can see every error on one line: such errors span 2^65,568
 * patterns and at least 2^65,536 of them give the check back unchanged;
 * a line stuck, open or swapped is the link test's to find (S-09.073).
 */
static void test_quad_line(void)
{
	unsigned short_rank = 0;

	t_case = "quad line";
	for (unsigned k = 0; k < 4u; k++) {
		for (uint32_t c0 = 0; c0 < 8u; c0++) {
			uint32_t v[32];

			for (uint32_t c = 0; c < 32u; c++) {
				const uint32_t p = 4u * (5000u + c0 + c) + (3u - k);

				v[c] = syndrome(&p, 1);
			}
			short_rank += rank(v, 32) != 32u;
		}
	}
	CHECK_EQ(short_rank, 0u);
	/* so too four lines in one clock window of 8 clocks: a word's span */
	{
		uint32_t v[32];

		for (uint32_t i = 0; i < 32u; i++) {
			const uint32_t p = 4u * 6001u + 2u + i;

			v[i] = syndrome(&p, 1);
		}
		CHECK_EQ(rank(v, 32), 32u);
	}
}

/* The two sums the check replaced (for the control below). */
static void old_sums(const uint8_t *d, uint32_t n, uint32_t *a, uint32_t *b)
{
	uint32_t x = 0x9E3779B9u, y = 0;

	for (uint32_t i = 0; i + 4u <= n; i += 4u) {
		x += (uint32_t)d[i] | (uint32_t)d[i + 1] << 8 |
		     (uint32_t)d[i + 2] << 16 | (uint32_t)d[i + 3] << 24;
		y += x;
	}
	*a = x;
	*b = y;
}

static void add_word(uint8_t *d, uint32_t v)
{
	uint32_t w = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 |
		     (uint32_t)d[3] << 24;

	w += v;
	d[0] = (uint8_t)w;
	d[1] = (uint8_t)(w >> 8);
	d[2] = (uint8_t)(w >> 16);
	d[3] = (uint8_t)(w >> 24);
}

/*
 * The shapes the sums missed: +x, -2x, +x on three words in a row, and bit
 * 31 flipped in two words an even distance apart (in quad I/O, one line).
 * The sums miss every one (the control); the check misses none. The second
 * is two bits, always seen; the first changes at most six bits within 96
 * when x has one bit and nothing carries, always seen then (an error of six
 * bits or fewer within 202 bits is), and is otherwise seen with
 * probability 1 - 2^-32.
 */
static void test_shapes(void)
{
	uint32_t base, a0, b0;
	unsigned old_missed = 0, missed = 0, tried = 0;

	t_case = "shapes";
	fill(3);
	base = s09_check_word(req, BODY);
	old_sums(req, BODY, &a0, &b0);
	for (uint32_t k = 0; k < 4000u; k++) {
		const uint32_t i = 3u + (k * 37u) % (BODY / 4u - 6u);
		const uint32_t x = k < 32u ? 1u << k : k * 2654435761u;
		uint32_t a, b;

		add_word(req + 4u * i, x);
		add_word(req + 4u * (i + 1u), 0u - 2u * x);
		add_word(req + 4u * (i + 2u), x);
		old_sums(req, BODY, &a, &b);
		old_missed += a == a0 && b == b0;
		missed += s09_check_word(req, BODY) == base;
		tried++;
		fill(3);
	}
	for (uint32_t i = 0; i + 2u < BODY / 4u; i += 97u) {
		for (uint32_t gap = 2; i + gap < BODY / 4u && gap < 4000u; gap += 2u * (gap / 64u + 1u)) {
			uint32_t a, b;

			req[4u * i + 3u] ^= 0x80u;
			req[4u * (i + gap) + 3u] ^= 0x80u;
			old_sums(req, BODY, &a, &b);
			old_missed += a == a0 && b == b0;
			missed += s09_check_word(req, BODY) == base;
			tried++;
			req[4u * i + 3u] ^= 0x80u;
			req[4u * (i + gap) + 3u] ^= 0x80u;
		}
	}
	CHECK_EQ(old_missed, tried);
	CHECK_EQ(missed, 0u);
}

/* S-09.076: the trailer is the number, then the check of every byte
 * before the check, the number included. */
static void test_trailer(void)
{
	uint8_t d[S09_HDR + 8u + S09_CHECK_LEN];
	uint32_t c;

	t_case = "trailer";
	memset(d, 0x5A, sizeof d);
	c = s09_trailer(d, S09_HDR + 8u, 0x01020304u, NULL, NULL);
	CHECK(d[20] == 4 && d[21] == 3 && d[22] == 2 && d[23] == 1);
	CHECK_EQ(c, s09_check_word(d, S09_HDR + 12u));
	CHECK(d[24] == (uint8_t)c && d[27] == (uint8_t)(c >> 24));
}

/* The known answers pass with the tables built, and fail with tables that
 * read back wrong; a build mends them. */
static void spoil(void)
{
	s09_check_t[2][0x5A] ^= 1u;
}

static void test_ready(void)
{
	t_case = "ready";
	CHECK(s09_check_ready());
	s09_check_built = spoil;
	CHECK(!s09_check_ready());
	s09_check_built = NULL;
	memset(s09_check_t, 0, sizeof s09_check_t);
	CHECK(s09_check_ready());
}

int main(void)
{
	powers();
	test_ready();
	test_reference();
	test_against_reference();
	test_model();
	test_one_two_bits();
	test_three_bits();
	test_bursts();
	test_quad_line();
	test_shapes();
	test_trailer();
	return t_finish("test_s09_check");
}

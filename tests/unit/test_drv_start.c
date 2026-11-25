/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-01.021, checked on the source of the reset entry
 * (src/drivers/bf70x/start.S, read from the repository root as make check
 * runs the tests): the CPLBs go off before the first memory access, so
 * nothing meets the first stage's data CPLBs, and the masks it uses are
 * the bits wb_cplb_disable() clears; the cycle counter is started before
 * any time use (S-13a.030). Before all of that the branch predictor is
 * emptied and switched off (S-13a.042); the front's entry
 * (src/front/start/entry.S) is held to the same, and to S-01.024's mask.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/check.h"

#include "waffle/drivers/bf70x/coremmr.h"
#include "waffle/drivers/bf70x/cplb.h"
#include "waffle/drivers/bf70x/rcu.h"

#define START_S "src/drivers/bf70x/start.S"
#define FRONT_S "src/front/start/entry.S"

static char src[32 * 1024];

/* A source file with its comments blanked out; NULL if unreadable. */
static char *load_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	size_t n;
	char *p;

	if (f == NULL)
		return NULL;
	n = fread(src, 1, sizeof(src) - 1u, f);
	fclose(f);
	src[n] = '\0';
	for (p = src; (p = strstr(p, "/*")) != NULL;) {
		char *e = strstr(p + 2, "*/");

		if (e == NULL)
			return NULL;
		memset(p, ' ', (size_t)(e + 2 - p));
		p = e + 2;
	}
	return src;
}

/* The value of "#define name value", or -1. */
static long define(const char *s, const char *name)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "#define %s ", name);
	p = strstr(s, pat);
	return p == NULL ? -1 : strtol(p + strlen(pat), NULL, 0);
}

static char *load(void)
{
	return load_file(START_S);
}

/* Whether word w stands alone in [a, b). */
static int has_word(const char *a, const char *b, const char *w)
{
	const size_t n = strlen(w);
	const char *p;

	for (p = a; (p = strstr(p, w)) != NULL && p < b; p++)
		if (!isalnum((unsigned char)p[-1]) && p[-1] != '_' &&
		    !isalnum((unsigned char)p[n]) && p[n] != '_')
			return 1;
	return 0;
}

/*
 * S-13a.042 on one entry: from the label, CSYNC, BP_CFG = 0x02778001 and
 * SYSCFG.BPEN cleared by read-modify-write are the first instructions, and
 * nothing in the file sets BPEN again. Returns the end of the SYSCFG write.
 */
static const char *predictor_steps(const char *s, const char *label)
{
	static const char *const steps[] = {
		"CSYNC;", "P0.L = BPCFG_LO;", "P0.H = BPCFG_HI;",
		"R0.L = BPCLR_L;", "R0.H = BPCLR_H;", "[P0] = R0;",
		"R0 = SYSCFG;", "BITCLR(R0, BPEN_BIT);", "SYSCFG = R0;",
	};
	const char *p = strstr(s, label);
	size_t i;

	CHECK(p != NULL);
	if (p == NULL)
		return NULL;
	p += strlen(label);
	for (i = 0; i < ARRAY_LEN(steps); i++) {
		while (isspace((unsigned char)*p))
			p++;
		CHECK(strncmp(p, steps[i], strlen(steps[i])) == 0);
		if (strncmp(p, steps[i], strlen(steps[i])) != 0)
			return NULL;
		p += strlen(steps[i]);
	}
	CHECK(((unsigned long)define(s, "BPCFG_HI") << 16 |
	       (unsigned long)define(s, "BPCFG_LO")) == 0x1FC0A000ul);
	CHECK(((unsigned long)define(s, "BPCLR_H") << 16 |
	       (unsigned long)define(s, "BPCLR_L")) == 0x02778001ul);
	CHECK(define(s, "BPEN_BIT") == 8);
	CHECK(strstr(s, "BITSET(R0, BPEN_BIT)") == NULL);
	CHECK(strstr(s, "BITSET(R0, 8)") == NULL);
	return p;
}

/* The test OS's entry: the same steps first (it is entered with
 * prediction on, over code the bootloader ran). */
static void test_testos_entry(void)
{
	const char *s = load_file("src/testos/testos_entry.S");

	CHECK(s != NULL);
	if (s != NULL)
		CHECK(predictor_steps(s, "\n_wb_testos_entry:") != NULL);
}

static void test_predictor_first(void)
{
	const char *s = load();
	const char *p, *b;

	CHECK(s != NULL);
	if (s == NULL)
		return;
	p = predictor_steps(s, "\n_wb_reset:");
	b = strstr(s, "\n_wb_bp_off:");
	CHECK(p != NULL && b != NULL);
	if (p == NULL || b == NULL)
		return;
	while (isspace((unsigned char)*p))
		p++;
	CHECK(p == b + 1);
}

/*
 * The front's entry: the same steps first, then IMASK bits 5 to 14 cleared
 * (S-01.024) before the first MMR access below the core's own; its INIT
 * entry touches neither BP_CFG, SYSCFG nor IMASK.
 */
static void test_front_entry(void)
{
	const char *s = load_file(FRONT_S);
	static const char *const mask[] = {
		"CLI R0;", "R1.L = IMASK_KEEP_L;", "R1.H = IMASK_KEEP_H;",
		"R0 = R0 & R1;", "STI R0;",
	};
	const char *p, *a, *e;
	size_t i;

	CHECK(s != NULL);
	if (s == NULL)
		return;
	p = predictor_steps(s, "\n_fr_entry:");
	if (p == NULL)
		return;
	for (i = 0; i < ARRAY_LEN(mask); i++) {
		while (isspace((unsigned char)*p))
			p++;
		CHECK(strncmp(p, mask[i], strlen(mask[i])) == 0);
		p += strlen(mask[i]);
	}
	CHECK(((unsigned long)define(s, "IMASK_KEEP_H") << 16 |
	       (unsigned long)define(s, "IMASK_KEEP_L")) ==
	      (0xFFFFFFFFul & ~0x7FE0ul));
	/* One STI, the entry's: nothing sets a mask bit again. */
	CHECK(strstr(p, "STI") == NULL);
	a = strstr(s, "\n_fr_init:");
	e = a ? strstr(a, ".size _fr_init") : NULL;
	CHECK(a != NULL && e != NULL);
	if (a == NULL || e == NULL)
		return;
	CHECK(!has_word(a, e, "SYSCFG") && !has_word(a, e, "BPCFG_LO") &&
	      !has_word(a, e, "CLI") && !has_word(a, e, "STI"));
}

/* The test OS's entry clears only what wb_cplb_disable() clears: read
 * parity checking stays on (S-13a.040). */
static void test_testos_masks(void)
{
	const char *s = load_file("src/testos/testos_entry.S");
	const unsigned long dclr = L1DM_DCTL_ENCPLB | L1DM_DCTL_CFG_MASK |
				   L1DM_DCTL_ENX;
	const unsigned long iclr = L1IM_ICTL_ENCPLB | L1IM_ICTL_CFG;

	CHECK(s != NULL);
	if (s == NULL)
		return;
	CHECK(((unsigned long)define(s, "DCTL_KEEP_H") << 16 |
	       (unsigned long)define(s, "DCTL_KEEP_L")) == (~dclr & 0xFFFFFFFFul));
	CHECK(((unsigned long)define(s, "ICTL_KEEP_H") << 16 |
	       (unsigned long)define(s, "ICTL_KEEP_L")) == (~iclr & 0xFFFFFFFFul));
}

static void test_masks(void)
{
	const char *s = load();
	const unsigned long dclr = L1DM_DCTL_ENCPLB | L1DM_DCTL_CFG_MASK |
				   L1DM_DCTL_RDCHK | L1DM_DCTL_ENX;
	const unsigned long iclr = L1IM_ICTL_ENCPLB | L1IM_ICTL_CFG |
				   L1IM_ICTL_RDCHK;
	unsigned long dkeep, ikeep;

	CHECK(s != NULL);
	if (s == NULL)
		return;
	CHECK((unsigned long)define(s, "CTL_HI") == L1DM_DCTL >> 16);
	CHECK((unsigned long)define(s, "CTL_HI") == L1IM_ICTL >> 16);
	CHECK((unsigned long)define(s, "DCTL_LO") == (L1DM_DCTL & 0xFFFFu));
	CHECK((unsigned long)define(s, "ICTL_LO") == (L1IM_ICTL & 0xFFFFu));
	dkeep = (unsigned long)define(s, "DCTL_KEEP_H") << 16 |
		(unsigned long)define(s, "DCTL_KEEP_L");
	ikeep = (unsigned long)define(s, "ICTL_KEEP_H") << 16 |
		(unsigned long)define(s, "ICTL_KEEP_L");
	CHECK(dkeep == (~dclr & 0xFFFFFFFFul));
	CHECK(ikeep == (~iclr & 0xFFFFFFFFul));
}

/*
 * From _wb_bp_off (the predictor's steps come first) to _wb_cplb_off: the only memory operand is [P0], P0
 * holds only the two control registers' halves, and SP, FP, calls and
 * pushes do not appear; then the stack and the vectors come after.
 */
static void test_cplb_off_first(void)
{
	const char *s = load();
	const char *a, *b, *p;
	static const char *const banned[] = {
		"SP", "FP", "CALL", "LINK", "RETI", "RAISE", "JUMP",
	};
	size_t i;

	CHECK(s != NULL);
	if (s == NULL)
		return;
	a = strstr(s, "\n_wb_bp_off:");
	b = strstr(s, "\n_wb_cplb_off:");
	CHECK(a != NULL && b != NULL && a < b);
	if (a == NULL || b == NULL || a > b)
		return;
	for (p = a; (p = strchr(p, '[')) != NULL && p < b; p++)
		CHECK(strncmp(p, "[P0]", 4) == 0);
	for (p = a; (p = strstr(p, "P0.")) != NULL && p < b; p++) {
		const char *v = strchr(p, '=');

		CHECK(v != NULL);
		if (v == NULL)
			break;
		while (isspace((unsigned char)*++v))
			;
		CHECK(strncmp(v, "CTL_HI;", 7) == 0 ||
		      strncmp(v, "DCTL_LO;", 8) == 0 ||
		      strncmp(v, "ICTL_LO;", 8) == 0);
	}
	for (i = 0; i < ARRAY_LEN(banned); i++)
		for (p = a + 1; (p = strstr(p, banned[i])) != NULL && p < b;
		     p++) {
			/* Whole words only: "SSYNC" holds no "SP". */
			size_t n = strlen(banned[i]);
			int word = !isalnum((unsigned char)p[-1]) &&
				   p[-1] != '_' &&
				   !isalnum((unsigned char)p[n]) &&
				   p[n] != '_';

			if (word)
				printf("  %s before _wb_cplb_off\n",
				       banned[i]);
			CHECK(!word);
		}
	/* The stack and the event vectors follow the CPLBs going off. */
	p = strstr(b, "SP.L = _wb_stack_end;");
	CHECK(p != NULL);
	p = strstr(b, "P0.H = EVT_HI;");
	CHECK(p != NULL);
}

/*
 * S-13a.030: SYSCFG.CCEN set by read-modify-write in the reset path,
 * after the CPLBs go off and before the stack, the vectors and the first
 * call (the main image's first time use is in C).
 */
static void test_counter_started(void)
{
	const char *s = load();
	const char *b, *r, *w, *sp, *call;

	CHECK(s != NULL);
	if (s == NULL)
		return;
	b = strstr(s, "\n_wb_cplb_off:");
	r = b ? strstr(b, "R0 = SYSCFG;") : NULL;
	w = r ? strstr(r, "BITSET(R0, 1);") : NULL;
	w = w ? strstr(w, "SYSCFG = R0;") : NULL;
	sp = b ? strstr(b, "SP.L = _wb_stack_end;") : NULL;
	call = b ? strstr(b, "CALL") : NULL;
	CHECK(r != NULL && w != NULL && sp != NULL && call != NULL);
	if (r == NULL || w == NULL || sp == NULL || call == NULL)
		return;
	CHECK(w < sp && w < call);
	/* The counter is never cleared or written (S-13a.030). */
	CHECK(strstr(s, "BITCLR(R0, 1)") == NULL);
	CHECK(strstr(s, "CYCLES =") == NULL && strstr(s, "CYCLES2 =") == NULL);
}

/*
 * S-05.013, S-05.027: RCU0_SVECT0 = 0x11A00000 in the reset path, after
 * the CPLBs go off and before the first call (so before any log line),
 * once; the hand-over only reads it.
 */
static void test_svect0(void)
{
	static const char *const steps[] = {
		"P0.L = SVECT0_LO;", "P0.H = SVECT0_HI;", "R0.L = SVECT_L;",
		"R0.H = SVECT_H;", "[P0] = R0;",
	};
	const char *s = load();
	const char *b, *p, *call;
	size_t i;

	CHECK(s != NULL);
	if (s == NULL)
		return;
	CHECK(((unsigned long)define(s, "SVECT0_HI") << 16 |
	       (unsigned long)define(s, "SVECT0_LO")) == RCU0_SVECT0);
	CHECK(((unsigned long)define(s, "SVECT_H") << 16 |
	       (unsigned long)define(s, "SVECT_L")) == 0x11A00000ul);
	b = strstr(s, "\n_wb_cplb_off:");
	p = b ? strstr(b, steps[0]) : NULL;
	call = b ? strstr(b, "CALL") : NULL;
	CHECK(p != NULL && call != NULL && p < call);
	if (p == NULL || call == NULL)
		return;
	for (i = 0; i < ARRAY_LEN(steps); i++) {
		while (isspace((unsigned char)*p))
			p++;
		CHECK(strncmp(p, steps[i], strlen(steps[i])) == 0);
		p += strlen(steps[i]);
	}
	/* Written once. */
	CHECK(strstr(p, "SVECT0_LO") == NULL);
	/* The hand-over's C reads it and writes nothing to it. */
	s = load_file("src/board/boot/handover.c");
	CHECK(s != NULL);
	if (s == NULL)
		return;
	for (p = s; (p = strstr(p, "RCU0_SVECT0")) != NULL; p++)
		CHECK(p - s >= 14 && strncmp(p - 14, "wb_mmr_read32(", 14) == 0);
}

int main(void)
{
	RUN(test_svect0);
	RUN(test_predictor_first);
	RUN(test_testos_entry);
	RUN(test_front_entry);
	RUN(test_masks);
	RUN(test_testos_masks);
	RUN(test_cplb_off_first);
	RUN(test_counter_started);
	return check_summary("test_drv_start");
}

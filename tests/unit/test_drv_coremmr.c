/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The core MMR addresses of waffle/drivers/bf70x_coremmr.h against S-01.012's
 * table: tests/oracle/S-01-boot-chain/memory_map.json, mmr.core_mmr, read at run
 * time (from the repository root, as make check runs the tests).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/check.h"

#include "waffle/drivers/bf70x_coremmr.h"

#define MAP "tests/oracle/S-01-boot-chain/memory_map.json"

static char json[64 * 1024];

static const struct {
	const char *name;
	unsigned long addr;
} want[] = {
	{ "L1DM_DCTL", L1DM_DCTL },
	{ "L1DM_DCPLB_DFLT", L1DM_DCPLB_DFLT },
	{ "L1DM_DCPLB_ADDR0", L1DM_DCPLB_ADDR(0) },
	{ "L1DM_DCPLB_DATA0", L1DM_DCPLB_DATA(0) },
	{ "L1IM_ICTL", L1IM_ICTL },
	{ "L1IM_ICPLB_DFLT", L1IM_ICPLB_DFLT },
	{ "L1IM_ICPLB_ADDR0", L1IM_ICPLB_ADDR(0) },
	{ "L1IM_ICPLB_DATA0", L1IM_ICPLB_DATA(0) },
	{ "EVT0", ICU_EVT(0) },
	{ "EVT15", ICU_EVT(15) },
	{ "EVT_OVERRIDE", ICU_EVT_OVERRIDE },
	{ "IMASK", ICU_IMASK },
	{ "IPEND", ICU_IPEND },
	{ "ILAT", ICU_ILAT },
	{ "TCNTL", TMR_TCNTL },
	{ "TPERIOD", TMR_TPERIOD },
	{ "TSCALE", TMR_TSCALE },
	{ "TCOUNT", TMR_TCOUNT },
};

/* The core_mmr object's text, from its '{' to its '}' (it nests none). */
static const char *core_mmr(void)
{
	FILE *f = fopen(MAP, "rb");
	size_t n;
	char *s, *e;

	if (f == NULL)
		return NULL;
	n = fread(json, 1, sizeof(json) - 1u, f);
	fclose(f);
	json[n] = '\0';
	s = strstr(json, "\"core_mmr\"");
	if (s == NULL || (s = strchr(s, '{')) == NULL ||
	    (e = strchr(s, '}')) == NULL)
		return NULL;
	*e = '\0';
	return s;
}

/* The value of "key": "0x..." inside obj, or 0 when absent. */
static unsigned long value(const char *obj, const char *key)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(obj, pat);
	if (p == NULL || (p = strchr(p + strlen(pat), '"')) == NULL)
		return 0;
	return strtoul(p + 1, NULL, 16);
}

static void test_table(void)
{
	const char *obj = core_mmr();
	size_t i;

	CHECK(obj != NULL);
	if (obj == NULL)
		return;
	for (i = 0; i < ARRAY_LEN(want); i++) {
		unsigned long v = value(obj, want[i].name);

		if (v != want[i].addr)
			printf("  %s: header 0x%08lx, S-01.012 0x%08lx\n",
			       want[i].name, want[i].addr, v);
		CHECK(v == want[i].addr);
	}
}

/* No vector lands at the PRM's misprinted table (S-01.012). */
static void test_not_the_misprint(void)
{
	const char *obj = core_mmr();
	unsigned long bad, n;

	CHECK(obj != NULL);
	if (obj == NULL)
		return;
	bad = value(obj, "prm_evt_misprint");
	CHECK(bad != 0);
	for (n = 0; n < 16u; n++)
		CHECK(ICU_EVT(n) < bad || ICU_EVT(n) >= bad + 0x40u);
	/* Nothing above 0x1FC04000 is used at the interfaces. */
	CHECK(TMR_TCOUNT < 0x1FC04000u && ICU_ILAT < 0x1FC04000u);
}

int main(void)
{
	RUN(test_table);
	RUN(test_not_the_misprint);
	return check_summary("test_drv_coremmr");
}

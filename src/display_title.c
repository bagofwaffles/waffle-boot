/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The title bar's font (S-15.003), built from wb_display_font by the rule
 * of tools/bdf2c.py, which also writes the table this must equal
 * (tests/unit/display_title_ref.h). Rows are 9 bits here, bit 8 the
 * leftmost, until the last step shifts them to the table's 10.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "waffle/display.h"

uint16_t wb_display_title_font[WB_FONT_COUNT][WB_TITLE_H];
static bool built;

#define ROW_W 9u
#define ROW_MASK 0x1FFu

/* The glyphs the rule does not serve, drawn by hand in the widened 16-row
 * cell from FIX_ROW (bdf2c.py's TITLE_FIXES). */
#define FIX_ROW 2u
#define FIX_ROWS 10u
static const uint16_t fix_i[FIX_ROWS] = {
	0x0FE, 0x038, 0x038, 0x038, 0x038, 0x038, 0x038, 0x038, 0x038, 0x0FE,
};
static const uint16_t fix_t[FIX_ROWS] = {
	0x1FF, 0x038, 0x038, 0x038, 0x038, 0x038, 0x038, 0x038, 0x038, 0x038,
};

/* A row of 8 pixels to 9: columns 1 and 5 repeated (column 7 dropped),
 * then a stem of 2 with 2 clear columns (or the edge) after it made 3. */
static unsigned widen(uint8_t row)
{
	unsigned b[7], n[ROW_W], out, i, j;

	for (i = 0; i < 7u; i++)
		b[i] = ((unsigned)row >> (WB_FONT_W - 1u - i)) & 1u;
	n[0] = b[0];
	n[1] = b[1];
	n[2] = b[1];
	n[3] = b[2];
	n[4] = b[3];
	n[5] = b[4];
	n[6] = b[5];
	n[7] = b[5];
	n[8] = b[6];
	out = 0;
	for (i = 0; i < ROW_W; i++)
		out |= n[i] << (ROW_W - 1u - i);
	i = 0;
	while (i < ROW_W) {
		if (!n[i]) {
			i++;
			continue;
		}
		j = i;
		while (j + 1u < ROW_W && n[j + 1u])
			j++;
		if (j - i == 1u && j + 2u < ROW_W && !n[j + 2u] &&
		    (j + 3u >= ROW_W || !n[j + 3u]))
			out |= 1u << (ROW_W - 1u - (j + 1u));
		i = j + 1u;
	}
	return out;
}

/* Whether a row holds a horizontal stroke: 5 set columns in a row. */
static int has_bar(unsigned row)
{
	return (row & (row >> 1) & (row >> 2) & (row >> 3) & (row >> 4)) != 0u;
}

static void insert(uint16_t *rows, unsigned *len, unsigned at)
{
	memmove(&rows[at + 1u], &rows[at], (*len - at) * sizeof rows[0]);
	(*len)++;
}

/* rows[0..len) to want rows: each horizontal stroke repeated once, then
 * the longest run of one set row (the last of the longest) repeated. */
static void grow(uint16_t *rows, unsigned len, unsigned want)
{
	unsigned i = 0;

	while (len < want && i < len) {
		if (has_bar(rows[i])) {
			insert(rows, &len, i);
			i += 2u;
		} else {
			i++;
		}
	}
	while (len < want) {
		unsigned at = 0, best = 0, j;

		for (i = 0; i < len; i = j + 1u) {
			j = i;
			while (j + 1u < len && rows[j + 1u] == rows[i])
				j++;
			if (rows[i] != 0u && j - i + 1u >= best) {
				best = j - i + 1u;
				at = i;
			}
		}
		insert(rows, &len, at);
	}
}

/* S-15.003: rows 2-4 of the widened cell to 4, the x-height's 5-11 to 10,
 * rows 12-14 kept. Once: wb_display_init() calls it each time. */
void wb_display_title_font_build(void)
{
	unsigned c, j;

	if (built)
		return;
	built = true;
	for (c = 0; c < WB_FONT_COUNT; c++) {
		uint16_t cell[WB_FONT_H], tall[WB_TITLE_H];
		const uint16_t *fix = NULL;

		for (j = 0; j < WB_FONT_H; j++)
			cell[j] = (uint16_t)widen(wb_display_font[c][j]);
		if (c + WB_FONT_FIRST == 'I')
			fix = fix_i;
		else if (c + WB_FONT_FIRST == 'T')
			fix = fix_t;
		if (fix != NULL)
			memcpy(&cell[FIX_ROW], fix, sizeof fix_i);
		memcpy(&tall[0], &cell[2], 3u * sizeof cell[0]);
		grow(&tall[0], 3u, 4u);
		memcpy(&tall[4], &cell[5], 7u * sizeof cell[0]);
		grow(&tall[4], 7u, 10u);
		memcpy(&tall[14], &cell[12], 3u * sizeof cell[0]);
		for (j = 0; j < WB_TITLE_H; j++)
			wb_display_title_font[c][j] =
				(uint16_t)((tall[j] & ROW_MASK) << 1);
	}
}

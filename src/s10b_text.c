/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10b's text rules: names (S-10b.047), formats (S-10b.046) and DateTime
 * strings (S-10b.048, S-10b.062).
 */
#include "waffle/s10b_mtp.h"

/* The length of the well-formed UTF-8 sequence at p (Unicode 3.9, Table
 * 3-7) and its scalar value; 0 when there is none. */
static size_t utf8_seq(const uint8_t *p, size_t n, uint32_t *cp)
{
	const uint8_t b = p[0];
	uint8_t lo = 0x80, hi = 0xBF;
	size_t len, i;
	uint32_t v;

	if (b < 0x80u) {
		*cp = b;
		return 1;
	}
	if (b >= 0xC2u && b <= 0xDFu) {
		len = 2;
		v = b & 0x1Fu;
	} else if (b >= 0xE0u && b <= 0xEFu) {
		len = 3;
		v = b & 0x0Fu;
		if (b == 0xE0u)
			lo = 0xA0;
		else if (b == 0xEDu)
			hi = 0x9F;
	} else if (b >= 0xF0u && b <= 0xF4u) {
		len = 4;
		v = b & 0x07u;
		if (b == 0xF0u)
			lo = 0x90;
		else if (b == 0xF4u)
			hi = 0x8F;
	} else {
		return 0;
	}
	if (n < len)
		return 0;
	for (i = 1; i < len; i++) {
		const uint8_t c = p[i];

		if (c < lo || c > hi)
			return 0;
		lo = 0x80;
		hi = 0xBF;
		v = v << 6 | (c & 0x3Fu);
	}
	*cp = v;
	return len;
}

size_t s10b_name_utf16(const uint8_t *name, size_t n, uint16_t *out)
{
	size_t i = 0, units = 0;

	while (i < n) {
		uint32_t cp = 0xFFFD;
		size_t len = utf8_seq(name + i, n - i, &cp);
		const size_t need = cp > 0xFFFFu ? 2u : 1u;

		if (len == 0) {
			cp = 0xFFFD;
			len = 1;
		}
		if (units + need > S10B_NAME_UNITS)
			break;
		if (need == 2) {
			cp -= 0x10000u;
			out[units++] = (uint16_t)(0xD800u | cp >> 10);
			out[units++] = (uint16_t)(0xDC00u | (cp & 0x3FFu));
		} else {
			out[units++] = (uint16_t)cp;
		}
		i += len;
	}
	return units;
}

/* S-10b.046, in order; a row's patterns end at an empty one. */
static const struct {
	const char *pat[2];
	uint16_t format;
} format_table[] = {
	{ { ".JPG", ".JPEG" }, 0x3801 },
	{ { ".TIF", "" }, 0x380D },
	{ { ".BMP", "" }, 0x3804 },
	{ { ".PNG", "" }, 0x380B },
	{ { ".GIF", "" }, 0x3807 },
	{ { ".EXE", "" }, 0x3003 },
	{ { ".TXT", "" }, 0x3004 },
	{ { ".HTM", "" }, 0x3005 },
	{ { ".WAV", "" }, 0x3008 },
	{ { ".AIFF", "" }, 0x3007 },
	{ { ".MP3", "" }, 0x3009 },
	{ { ".AVI", "" }, 0x300A },
	{ { ".MPEG", "" }, 0x300B },
	{ { ".ASF", "" }, 0x300C },
};

#define N_ROWS (sizeof(format_table) / sizeof(format_table[0]))

static uint8_t upper(uint8_t c)
{
	return c >= 'a' && c <= 'z' ? (uint8_t)(c - ('a' - 'A')) : c;
}

static bool holds(const uint8_t *name, size_t n, const char *pat)
{
	size_t i, k;

	for (i = 0; pat[0] && i < n; i++) {
		for (k = 0; pat[k] && i + k < n; k++)
			if (upper(name[i + k]) != (uint8_t)pat[k])
				break;
		if (!pat[k])
			return true;
	}
	return false;
}

uint16_t s10b_format_rows(const uint8_t *name, size_t n)
{
	uint16_t rows = 0;
	size_t r;

	/* A pattern holds no '/', so it lies within one name of P. */
	for (r = 0; r < N_ROWS; r++)
		if (holds(name, n, format_table[r].pat[0]) ||
		    holds(name, n, format_table[r].pat[1]))
			rows |= (uint16_t)(1u << r);
	return rows;
}

uint16_t s10b_format(uint16_t rows)
{
	size_t r;

	for (r = 0; r < N_ROWS; r++)
		if (rows & (1u << r))
			return format_table[r].format;
	return 0x3000;
}

bool s10b_format_known(uint16_t format)
{
	size_t r;

	for (r = 0; r < N_ROWS; r++)
		if (format_table[r].format == format)
			return true;
	return format == 0x3000u;
}

/* --- DateTime ------------------------------------------------------------ */

static bool leap(unsigned y)
{
	return (y % 4u == 0 && y % 100u != 0) || y % 400u == 0;
}

static unsigned month_days(unsigned y, unsigned mo)
{
	static const uint8_t days[12] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};

	return mo == 2 && leap(y) ? 29u : days[mo - 1u];
}

static void put_digits(char *p, unsigned v, unsigned n)
{
	while (n--) {
		p[n] = (char)('0' + v % 10u);
		v /= 10u;
	}
}

void s10b_datetime_text(uint32_t secs, char out[16])
{
	uint32_t days = secs / 86400u, rem = secs % 86400u;
	unsigned y = 1970, mo = 1;

	while (days >= (leap(y) ? 366u : 365u)) {
		days -= leap(y) ? 366u : 365u;
		y++;
	}
	while (days >= month_days(y, mo)) {
		days -= month_days(y, mo);
		mo++;
	}
	put_digits(out, y, 4);
	put_digits(out + 4, mo, 2);
	put_digits(out + 6, days + 1u, 2);
	out[8] = 'T';
	put_digits(out + 9, rem / 3600u, 2);
	put_digits(out + 11, rem / 60u % 60u, 2);
	put_digits(out + 13, rem % 60u, 2);
	out[15] = '\0';
}

static bool digits(const uint16_t *u, size_t n, unsigned *v)
{
	size_t i;

	*v = 0;
	for (i = 0; i < n; i++) {
		if (u[i] < '0' || u[i] > '9')
			return false;
		*v = *v * 10u + (unsigned)(u[i] - '0');
	}
	return true;
}

/* Days since 1970-01-01, for the range check. */
static unsigned long day_number(const struct s10b_datetime *t)
{
	unsigned long d = 0;
	unsigned y, mo;

	for (y = 1970; y < t->year; y++)
		d += leap(y) ? 366u : 365u;
	for (mo = 1; mo < t->month; mo++)
		d += month_days(t->year, mo);
	return d + t->day - 1u;
}

bool s10b_datetime_parse(const uint16_t *u, size_t n, bool range,
			 struct s10b_datetime *dt)
{
	struct s10b_datetime t;
	size_t i = 15;
	unsigned zone;

	if (n < 15 || u[8] != 'T' || !digits(u, 4, &t.year) ||
	    !digits(u + 4, 2, &t.month) || !digits(u + 6, 2, &t.day) ||
	    !digits(u + 9, 2, &t.hour) || !digits(u + 11, 2, &t.minute) ||
	    !digits(u + 13, 2, &t.second))
		return false;
	if (i < n && u[i] == '.') {
		unsigned tenth;

		if (n - i < 2 || !digits(u + i + 1, 1, &tenth))
			return false;
		i += 2;
	}
	if (i < n && u[i] == 'Z') {
		i++;
	} else if (i < n && (u[i] == '+' || u[i] == '-')) {
		if (n - i < 5 || !digits(u + i + 1, 4, &zone))
			return false;
		i += 5;
	}
	if (i != n)
		return false;
	if (t.month < 1 || t.month > 12 || t.day < 1 ||
	    t.day > month_days(t.year, t.month) || t.hour > 23 ||
	    t.minute > 59 || t.second > 59)
		return false;
	/* 1970-01-01T000000 to 2059-09-20T235959 (S-04.070). */
	if (range) {
		static const struct s10b_datetime last = { 2059, 9, 20, 0, 0, 0 };

		if (t.year < 1970 || day_number(&t) > day_number(&last))
			return false;
	}
	*dt = t;
	return true;
}

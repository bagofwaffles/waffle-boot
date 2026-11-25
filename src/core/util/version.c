/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/util/version.h"

/* S-11.036: optional spaces or tabs, then decimal digits up to the first
 * other character; no digits read as 0; the value is taken modulo 2^16. */
static uint16_t read_number(const uint8_t *s, size_t len, size_t i)
{
	uint16_t v = 0;

	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;
	while (i < len && s[i] >= '0' && s[i] <= '9') {
		v = (uint16_t)(v * 10u + (unsigned)(s[i] - '0'));
		i++;
	}
	return v;
}

static bool find(const uint8_t *s, size_t len, size_t from, uint8_t c,
		 size_t *at)
{
	for (size_t i = from; i < len; i++) {
		if (s[i] == c) {
			*at = i;
			return true;
		}
	}
	return false;
}

/* S-11.036, S-17.024 */
bool s11_version_parse(const uint8_t *text, size_t len, size_t window,
		       struct s11_version *out)
{
	size_t d1, d2, plus;

	if (len > window)
		len = window;
	if (!find(text, len, 0, '.', &d1) || !find(text, len, d1 + 1, '.', &d2))
		return false;
	out->major = read_number(text, len, 0);
	out->minor = read_number(text, len, d1 + 1);
	out->patch = read_number(text, len, d2 + 1);
	/* build: after the first '+' that follows the patch's '.' */
	out->build = 0;
	if (find(text, len, d2 + 1, '+', &plus))
		out->build = read_number(text, len, plus + 1);
	return true;
}

/* S-11.036 */
struct s11_version s11_version_file(const uint8_t *data, size_t len,
				    bool present)
{
	struct s11_version v = { 1234, 0, 0, 0 };

	if (present)
		(void)s11_version_parse(data, len, S11_VERSION_FILE_WINDOW, &v);
	return v;
}

static int cmp16(uint16_t a, uint16_t b)
{
	return a < b ? -1 : a > b ? 1 : 0;
}

/* S-11.037 */
int s11_version_cmp(const struct s11_version *a, const struct s11_version *b)
{
	int c = cmp16(a->major, b->major);

	if (c == 0)
		c = cmp16(a->minor, b->minor);
	if (c == 0)
		c = cmp16(a->patch, b->patch);
	if (c == 0)
		c = cmp16(a->build, b->build);
	return c;
}

/* S-11.003, S-11.005, S-11.055 */
bool s11_version_is_forced_marker(const struct s11_version *v)
{
	return v->major == 0 && v->minor == 1 && v->patch == 0;
}

size_t s11_u32_text(uint32_t v, char *buf, size_t cap)
{
	char tmp[S11_U32_TEXT_MAX];
	size_t n = 0;

	do {
		tmp[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0);
	if (n > cap)
		return 0;
	for (size_t i = 0; i < n; i++)
		buf[i] = tmp[n - 1 - i];
	return n;
}

/* S-11 terms "Version"; S-10c.055 */
size_t s11_version_text(const struct s11_version *v, char *buf, size_t cap)
{
	char tmp[S11_VERSION_TEXT_MAX];
	const uint16_t f[4] = { v->major, v->minor, v->patch, v->build };
	size_t n = 0;

	for (unsigned i = 0; i < 4; i++) {
		if (i == 3) {
			if (f[3] == 0)
				break;
			tmp[n++] = '+';
		} else if (i != 0) {
			tmp[n++] = '.';
		}
		n += s11_u32_text(f[i], tmp + n, sizeof(tmp) - n);
	}
	if (n > cap)
		return 0;
	for (size_t i = 0; i < n; i++)
		buf[i] = tmp[i];
	return n;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/s10c_host.h"

/* A bounded text builder; writes past `cap` are dropped. */
struct text {
	char *buf;
	size_t len, cap;
};

static void put(struct text *t, const char *s, size_t n)
{
	for (size_t i = 0; i < n && t->len < t->cap; i++)
		t->buf[t->len++] = s[i];
}

static void puts_(struct text *t, const char *s)
{
	while (*s != '\0' && t->len < t->cap)
		t->buf[t->len++] = *s++;
}

static void put_u32(struct text *t, uint32_t v)
{
	char d[S11_U32_TEXT_MAX];
	size_t n = s11_u32_text(v, d, sizeof(d));

	put(t, d, n);
}

static void put_3digits(struct text *t, uint32_t v)
{
	char d[3];

	if (v > 999u)
		v = 0;
	d[0] = (char)('0' + v / 100u);
	d[1] = (char)('0' + v / 10u % 10u);
	d[2] = (char)('0' + v % 10u);
	put(t, d, 3);
}

static void put_version(struct text *t, const struct s11_version *v)
{
	char d[S11_VERSION_TEXT_MAX];
	size_t n = s11_version_text(v, d, sizeof(d));

	put(t, d, n);
}

/* S-10c.054, S-10c.055. S-17.033: no coprocessor value is carried. */
size_t s10c_info_text(const struct s10c_info *v, char *buf)
{
	struct text t = { buf, 0, S10C_INFO_TEXT_MAX };

	puts_(&t, "mode:bootloader;serial:");
	put(&t, v->serial, v->serial_len <= 8u ? (size_t)v->serial_len : 8u);
	puts_(&t, ";dsp_serial:");
	/* D: OTP item 1 up to its first 00 byte; empty when unreadable */
	if (v->dsp_ok) {
		size_t dsp = 0;

		while (dsp < sizeof(v->dsp_otp) && v->dsp_otp[dsp] != 0)
			dsp++;
		put(&t, (const char *)v->dsp_otp, dsp);
	}
	puts_(&t, ";hw_rev:");
	put_u32(&t, v->hw_rev);		/* S-17.013 */
	puts_(&t, ";product:OP-XY;sku:TE033AS");
	put_3digits(&t, v->sku);
	puts_(&t, ";sw_version:");
	put_version(&t, &v->sw);
	puts_(&t, ";bl_version:");
	put_version(&t, &v->bl);
	puts_(&t, ";os_version:");
	put_version(&t, &v->os);
	puts_(&t, ";factory:");
	put_u32(&t, v->factory);
	return t.len;
}

/* S-10c.088 */
void s10c_te_code(uint32_t word, char out[10])
{
	struct text t = { out, 0, 10 };
	uint32_t variant = (word >> 10) & 0xFu;

	puts_(&t, "TE");
	put_3digits(&t, (word >> 14) & 0x3FFu);
	puts_(&t, variant == 0 ? "AS" : variant == 1 ? "XS" : "__");
	put_3digits(&t, word & 0x3FFu);
}

static bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
	       c == '\r';
}

/* S-10c.056: leading white space, at most one sign, then decimal digits up
 * to the first other character; no digits give 0 (so "- 5" reads 0), a
 * value beyond 32 bits gives 2^32 - 1 whatever its sign, and otherwise a
 * '-' negates the value modulo 2^32. */
uint32_t s10c_parse_u32(const char *s, size_t len)
{
	size_t i = 0;
	bool neg = false, over = false;
	uint32_t v = 0;

	while (i < len && is_space(s[i]))
		i++;
	if (i < len && (s[i] == '+' || s[i] == '-'))
		neg = s[i++] == '-';
	for (; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
		uint32_t d = (uint32_t)(s[i] - '0');

		if (v > (UINT32_MAX - d) / 10u)
			over = true;
		else
			v = v * 10u + d;
	}
	if (over)
		return UINT32_MAX;
	return neg ? (uint32_t)(0u - v) : v;
}

/*
 * S-10c.056: the value of `key` in a set-metadata text, split at every ';'.
 * An entry's key is its text before its first
 * ':'; an entry with no ':' has no key. The first entry whose key equals
 * `key` is taken; the key counts as absent when that entry's value holds a
 * ':', or when it is the last entry and its value is empty.
 */
static bool setmeta_lookup(const char *s, size_t len, const char *key,
			   const char **val, size_t *vlen)
{
	size_t start = 0;

	while (start <= len) {
		size_t end = start, colon = len + 1;

		while (end < len && s[end] != ';') {
			if (colon > len && s[end] == ':')
				colon = end;
			end++;
		}
		if (colon <= len) {
			size_t k = 0;

			while (key[k] != '\0' && start + k < colon &&
			       s[start + k] == key[k])
				k++;
			if (key[k] == '\0' && start + k == colon) {
				const char *v = s + colon + 1;
				size_t n = end - colon - 1;

				for (size_t i = 0; i < n; i++) {
					if (v[i] == ':')
						return false;
				}
				if (n == 0 && end == len)
					return false;
				*val = v;
				*vlen = n > 23u ? 23u : n;
				return true;
			}
		}
		start = end + 1;
	}
	return false;
}

/* S-10c.056 */
void s10c_setmeta_parse(const uint8_t *payload, size_t len,
			struct s10c_setmeta *out)
{
	const char *s = (const char *)payload;
	const char *v;
	size_t n = 0, vlen;

	out->has_serial = out->has_sku = out->has_factory = false;
	out->serial_len = 0;
	out->sku = out->factory = 0;
	/* a payload of 249 bytes counts as having no keys */
	if (len != 249u) {
		while (n < len && payload[n] != 0)	/* up to the first 00 */
			n++;
	}
	if (setmeta_lookup(s, n, "serial", &v, &vlen)) {
		out->has_serial = true;
		out->serial_len = (uint8_t)vlen;
		for (size_t i = 0; i < vlen; i++)
			out->serial[i] = v[i];
	}
	if (setmeta_lookup(s, n, "sku_variant", &v, &vlen)) {
		out->has_sku = true;
		out->sku = s10c_parse_u32(v, vlen);
	}
	if (setmeta_lookup(s, n, "factory", &v, &vlen)) {
		out->has_factory = true;
		out->factory = s10c_parse_u32(v, vlen);
	}
}

/* S-10c.103, S-16.022 */
uint8_t s10c_percent(uint32_t done, uint32_t total)
{
	uint64_t t = total;

	if (total == 0)
		return 100;
	if (done > total)
		done = total;
	return (uint8_t)((200u * (uint64_t)done + t) / (2u * t));
}

uint8_t s10c_percent_te(uint32_t done, uint32_t total)
{
	return done == 0 && total == 0 ? 0u : s10c_percent(done, total);
}

/* S-10c.101 */
size_t s10c_progress_s06(uint32_t done, uint32_t total, const char *label,
			 uint8_t *out)
{
	struct text t = { (char *)out + 1, 0, S10C_TEXT_MAX };

	out[0] = 0x92;
	if (done != 0 || total != 0) {
		put_u32(&t, s10c_percent(done, total));
		puts_(&t, "%");
	}
	puts_(&t, label);
	return 1 + t.len;
}

/* S-10c.102 */
size_t s10c_progress_te(uint32_t done, uint32_t total, bool outcome,
			const char *label, uint8_t *out)
{
	struct text t = { (char *)out + 1, 0, S10C_LABEL_MAX_TE };

	out[0] = outcome ? 100u : s10c_percent_te(done, total);	/* S-10c.106 */
	puts_(&t, label);		/* cut to 62 bytes (S-10c Q-C16), no 00 */
	return 1 + t.len;
}

/* S-10c.105 */
size_t s10c_error_text(const struct s11_outcome *o, char *buf)
{
	struct text t = { buf, 0, S10C_TEXT_MAX };

	switch (o->kind) {
	case S11_OUTCOME_REFUSED:
		puts_(&t, s11_reason_name(o->reason));
		break;
	case S11_OUTCOME_FAILED:
		puts_(&t, "step ");
		put_u32(&t, o->step);
		puts_(&t, " result ");
		put_u32(&t, (uint32_t)o->result);
		break;
	case S11_OUTCOME_DONE:
	case S11_OUTCOME_NOTHING_TO_DO:
	default:
		break;
	}
	return t.len;
}

/* S-10c.104: the reserved step 6 has none */
const char *s10c_step_label(unsigned step)
{
	static const char *const labels[6] = {
		"[content]", "[main dsp]", "[bootloader]", "[ble]",
		"[ble bootloader]", "[backplane]",
	};

	return step < 6 ? labels[step] : "";
}

/* S-10c.106 */
size_t s10c_skip_label(unsigned step, enum s11_skip why, char *out)
{
	static const char *const reasons[] = {
		[S11_SKIP_NONE] = "",
		[S11_SKIP_OPTION_OFF] = "option off",
		[S11_SKIP_NOT_CONFIRMED] = "not confirmed",
		[S11_SKIP_SWITCHED_OFF] = "switched off",
		[S11_SKIP_ALREADY_WRITTEN] = "already written",
	};
	const char *const parts[] = { "[skipped] ", NULL, ": ",
				      reasons[why <= S11_SKIP_ALREADY_WRITTEN ?
					      why : S11_SKIP_NONE] };
	const char *label = s10c_step_label(step);
	size_t n = 0;

	for (unsigned i = 0; i < 4u; i++) {
		const char *s = parts[i];

		if (s == NULL) {
			/* the step's label without its brackets */
			for (s = label[0] != '\0' ? label + 1 : label;
			     s[0] != '\0' && s[1] != '\0' &&
			     n < S10C_SKIP_LABEL_MAX; s++)
				out[n++] = *s;
			continue;
		}
		for (; *s != '\0' && n < S10C_SKIP_LABEL_MAX; s++)
			out[n++] = *s;
	}
	out[n] = '\0';
	return n;
}

/* S-10c.033: the nominal timeout, not S-16.046's tenth of it */
uint32_t s10c_write_timeout_ms(size_t len)
{
	return (uint32_t)(len / 10u) + 110u;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10a.001 to S-10a.021: identity and descriptor sets
 * (waffle/usb/desc.h).
 */
#include "waffle/usb/desc.h"

#include <string.h>

#include "waffle/util/bytes.h"
#include "waffle/boot/device.h"

#define VID		0x2367u		/* S-10a.001 */
#define BCD_USB		0x0200u
#define BCD_DEVICE	0x0257u
#define EP0_SIZE	64u
#define INT_MPS		16u		/* S-10a.021 */

static const char manufacturer[] = "teenage engineering";

struct fn_at {
	uint8_t fn;		/* enum s10a_function */
	uint8_t first;		/* its first interface */
	bool iad;		/* led by an interface association */
};

/* A string: device.product_text and a suffix, or a fixed text. */
struct text {
	bool product;		/* led by device.product_text */
	const char *s;
};
#define PROD(suffix) { true, suffix }
#define FIXED(t) { false, t }

struct set_info {
	bool midi_pid;		/* device.midi_pid, else device.usb_pid */
	struct text product;
	uint8_t cls[3];
	const char *config_str;
	uint8_t powers[S10A_MAX_CONFIGS];
	uint8_t n_configs;
	uint8_t n_fns;
	struct fn_at fns[2];
};

/* S-10a.001, S-10a.014 to S-10a.020: idProduct and the product string follow
 * the device's profile */
static const struct set_info sets[S10A_SET_COUNT] = {
	[S10A_SET_MIDI] = { true, PROD(""), { 0, 0, 0 }, "Audio Config",
		{ 250, 50, 1 }, 3, 1, { { S10A_FN_MIDI, 0, false } } },
	[S10A_SET_MIDI_CDC] = { false, PROD(" Boot"), { 0xEF, 0x02, 0x01 },
		"Composite Config", { 250 }, 1, 2,
		{ { S10A_FN_MIDI, 0, true }, { S10A_FN_CDC, 2, true } } },
	[S10A_SET_DISK] = { false, PROD(" Disk"), { 0, 0, 0 }, "MS Config",
		{ 250, 50 }, 2, 1, { { S10A_FN_MSC, 0, false } } },
	[S10A_SET_DISK_CDC] = { false, PROD(" Boot"), { 0xEF, 0x02, 0x01 },
		"Composite Config", { 250 }, 1, 2,
		{ { S10A_FN_CDC, 0, true }, { S10A_FN_MSC, 2, false } } },
	[S10A_SET_MTP] = { false, PROD(""), { 0, 0, 0 }, "MTP Config",
		{ 250 }, 1, 1, { { S10A_FN_MTP, 0, false } } },
	[S10A_SET_MTP_VENDOR] = { false, PROD(""), { 0, 0, 0 }, "MTP Config",
		{ 250 }, 1, 1, { { S10A_FN_MTP_VENDOR, 0, false } } },
	[S10A_SET_SERIAL] = { false, PROD(" Boot"), { 0x02, 0x00, 0x00 },
		"Serial Config", { 250 }, 1, 1, { { S10A_FN_CDC, 0, false } } },
	[S10A_SET_MTP_CDC] = { false, PROD(" Boot"), { 0xEF, 0x02, 0x01 },
		"Composite Config", { 250 }, 1, 2,
		{ { S10A_FN_CDC, 0, true }, { S10A_FN_MTP, 2, false } } },
	[S10A_SET_MTP_VENDOR_CDC] = { false, PROD(" Boot"), { 0xEF, 0x02, 0x01 },
		"Composite Config", { 250 }, 1, 2,
		{ { S10A_FN_CDC, 0, true }, { S10A_FN_MTP_VENDOR, 2, false } } },
};

/* S-10a.013: interface strings, in interface order, per function. */
static const struct text fn_strings[][2] = {
	[S10A_FN_MIDI] = { PROD(""), PROD(" Midi") },
	[S10A_FN_CDC] = { FIXED("PORT0"), FIXED("PORT0") },
	[S10A_FN_MSC] = { FIXED("MS Data Intf BulkOnly"), FIXED(NULL) },
	[S10A_FN_MTP] = { FIXED("MTP Data Intf"), FIXED(NULL) },
	[S10A_FN_MTP_VENDOR] = { FIXED("MTP TETP interface"), FIXED(NULL) },
};

/* S-10a.015, S-10a.017: the function class and string of each IAD. */
static const uint8_t iad_cls[][3] = {
	[S10A_FN_MIDI] = { 0x01, 0x02, 0x00 },
	[S10A_FN_CDC] = { 0x02, 0x02, 0x01 },
};
static const struct text iad_strings[] = {
	[S10A_FN_MIDI] = PROD(" (Composite)"),
	[S10A_FN_CDC] = PROD(" Boot"),
};

static unsigned fn_interfaces(unsigned fn)
{
	return fn == S10A_FN_MIDI || fn == S10A_FN_CDC ? 2u : 1u;
}

static const struct set_info *info(enum s10a_set set)
{
	return &sets[(unsigned)set < S10A_SET_COUNT ? set : S10A_SET_MIDI];
}

/* S-10a.013: MTP's bulk pair is the CDC data pair's addresses when it is
 * alone, the mass-storage pair's beside the CDC port. */
static bool has_cdc(const struct set_info *s)
{
	unsigned i;

	for (i = 0; i < s->n_fns; i++)
		if (s->fns[i].fn == S10A_FN_CDC)
			return true;
	return false;
}

static uint8_t mtp_in(const struct set_info *s)
{
	return has_cdc(s) ? S10A_EP_BULK_IN : S10A_EP_CDC_IN;
}

static uint8_t mtp_out(const struct set_info *s)
{
	return has_cdc(s) ? S10A_EP_BULK_OUT : S10A_EP_CDC_OUT;
}

void s10a_serial_from_meta(const uint8_t *meta, size_t n,
			   char out[S10A_SERIAL_LEN + 1])
{
	size_t i;

	if (n > S10A_SERIAL_LEN)
		n = S10A_SERIAL_LEN;
	for (i = 0; i < n && meta[i]; i++)
		out[i] = meta[i] >= 0x20u && meta[i] <= 0x7Eu ?
			 (char)meta[i] : '0';
	out[i] = '\0';
}

unsigned s10a_desc_num_configs(enum s10a_set set)
{
	return info(set)->n_configs;
}

unsigned s10a_desc_num_interfaces(enum s10a_set set)
{
	const struct set_info *s = info(set);
	unsigned i, n = 0;

	for (i = 0; i < s->n_fns; i++)
		n += fn_interfaces(s->fns[i].fn);
	return n;
}

bool s10a_desc_interface(enum s10a_set set, unsigned itf,
			 enum s10a_function *fn, unsigned *first)
{
	const struct set_info *s = info(set);
	unsigned i;

	for (i = 0; i < s->n_fns; i++) {
		const struct fn_at *f = &s->fns[i];

		if (itf >= f->first && itf < f->first + fn_interfaces(f->fn)) {
			*fn = (enum s10a_function)f->fn;
			*first = f->first;
			return true;
		}
	}
	return false;
}

bool s10a_desc_has_endpoint(enum s10a_set set, uint8_t addr)
{
	const struct set_info *s = info(set);
	unsigned i;

	for (i = 0; i < s->n_fns; i++) {
		switch (s->fns[i].fn) {
		case S10A_FN_MIDI:
		case S10A_FN_MSC:
			if (addr == S10A_EP_BULK_OUT || addr == S10A_EP_BULK_IN)
				return true;
			break;
		case S10A_FN_CDC:
			if (addr == S10A_EP_CDC_NOTIFY ||
			    addr == S10A_EP_CDC_OUT || addr == S10A_EP_CDC_IN)
				return true;
			break;
		case S10A_FN_MTP:
		case S10A_FN_MTP_VENDOR:
		default:
			if (addr == mtp_in(s) || addr == mtp_out(s) ||
			    addr == S10A_EP_MTP_EVENT)
				return true;
			break;
		}
	}
	return false;
}

unsigned s10a_desc_max_power(enum s10a_set set, unsigned value)
{
	const struct set_info *s = info(set);

	if (value == 0 || value > s->n_configs)
		return 0;
	return s->powers[value - 1u];
}

void s10a_desc_device(const struct wb_device *dev, enum s10a_set set,
		      bool has_serial, uint8_t out[18])
{
	const struct set_info *s = info(set);

	out[0] = 18;
	out[1] = 0x01;
	wb_put_le16(out + 2, BCD_USB);
	memcpy(out + 4, s->cls, 3);
	out[7] = EP0_SIZE;
	wb_put_le16(out + 8, VID);
	wb_put_le16(out + 10, s->midi_pid ? dev->ident.midi_pid : dev->ident.usb_pid);
	wb_put_le16(out + 12, BCD_DEVICE);
	out[14] = 1;			/* manufacturer */
	out[15] = 2;			/* product */
	out[16] = (uint8_t)(has_serial ? 3 : 0);
	out[17] = s->n_configs;
}

void s10a_desc_qualifier(enum s10a_set set, uint8_t out[10])
{
	const struct set_info *s = info(set);

	out[0] = 10;
	out[1] = 0x06;
	wb_put_le16(out + 2, BCD_USB);
	memcpy(out + 4, s->cls, 3);
	out[7] = EP0_SIZE;
	out[8] = s->n_configs;
	out[9] = 0;
}

/* Strings named by one configuration: its own, the IADs', the interfaces'. */
static unsigned strings_per_config(const struct set_info *s)
{
	unsigned i, n = 1;

	for (i = 0; i < s->n_fns; i++)
		n += (s->fns[i].iad ? 1u : 0u) + fn_interfaces(s->fns[i].fn);
	return n;
}

static unsigned first_config_string(bool has_serial)
{
	return has_serial ? 4u : 3u;
}

/* String `index` (1-based); its s is NULL when the set names none. */
static struct text string_text(const struct set_info *s, const char *serial,
			       unsigned index)
{
	const bool has_serial = serial && serial[0];
	const unsigned base = first_config_string(has_serial);
	const unsigned per = strings_per_config(s);
	struct text t = FIXED(NULL);
	unsigned i, k;

	if (index == 1) {
		t.s = manufacturer;
		return t;
	}
	if (index == 2)
		return s->product;
	if (index == 3 && has_serial) {
		t.s = serial;
		return t;
	}
	if (index < base || index >= base + per * s->n_configs)
		return t;
	k = (index - base) % per;
	if (k-- == 0) {
		t.s = s->config_str;
		return t;
	}
	for (i = 0; i < s->n_fns; i++) {
		if (!s->fns[i].iad)
			continue;
		if (k-- == 0)
			return iad_strings[s->fns[i].fn];
	}
	for (i = 0; i < s->n_fns; i++) {
		const unsigned n = fn_interfaces(s->fns[i].fn);

		if (k < n)
			return fn_strings[s->fns[i].fn][k];
		k -= n;
	}
	return t;
}

/* The text of t on dev, cut to S10A_TEXT_MAX - 1 characters; false when t
 * names none. */
static bool compose(const struct wb_device *dev, struct text t,
		    char out[S10A_TEXT_MAX])
{
	size_t n = 0, i;

	if (!t.s)
		return false;
	if (t.product)
		for (i = 0; dev->ident.product_text[i] && n < S10A_TEXT_MAX - 1u; i++)
			out[n++] = dev->ident.product_text[i];
	for (i = 0; t.s[i] && n < S10A_TEXT_MAX - 1u; i++)
		out[n++] = t.s[i];
	out[n] = '\0';
	return true;
}

size_t s10a_desc_string(const struct wb_device *dev, enum s10a_set set,
			const char *serial, unsigned index,
			uint8_t out[S10A_STRING_MAX])
{
	char t[S10A_TEXT_MAX];
	size_t i, n;

	if (index == 0) {
		out[0] = 4;
		out[1] = 0x03;
		wb_put_le16(out + 2, 0x0409);
		return 4;
	}
	if (!compose(dev, string_text(info(set), serial, index), t))
		return 0;
	n = strlen(t);
	if (n > (S10A_STRING_MAX - 2u) / 2u)
		n = (S10A_STRING_MAX - 2u) / 2u;
	out[0] = (uint8_t)(2u + 2u * n);
	out[1] = 0x03;
	for (i = 0; i < n; i++)
		wb_put_le16(out + 2 + 2 * i, (uint8_t)t[i]);
	return out[0];
}

bool s10a_desc_text(const struct wb_device *dev, enum s10a_set set,
		    const char *serial, unsigned index, char out[S10A_TEXT_MAX])
{
	return index && compose(dev, string_text(info(set), serial, index), out);
}

/* --- Configurations ------------------------------------------------------ */

struct emit {
	uint8_t *p;
	size_t n;
	bool high;
	unsigned istr;		/* the next interface string index */
};

static void bytes(struct emit *e, const uint8_t *b, size_t n)
{
	if (e->n + n <= S10A_CONFIG_MAX)
		memcpy(e->p + e->n, b, n);
	e->n += n;
}

static void interface(struct emit *e, unsigned num, unsigned n_ep,
		      uint8_t cls, uint8_t sub, uint8_t proto)
{
	const uint8_t d[9] = { 9, 0x04, (uint8_t)num, 0, (uint8_t)n_ep,
			       cls, sub, proto, (uint8_t)e->istr++ };

	bytes(e, d, sizeof(d));
}

/* S-10a.021 */
static void bulk(struct emit *e, uint8_t addr, bool audio)
{
	const unsigned mps = e->high ? 512u : 64u;
	const uint8_t d[9] = { (uint8_t)(audio ? 9 : 7), 0x05, addr, 0x02,
			       (uint8_t)mps, (uint8_t)(mps >> 8), 0, 0, 0 };

	bytes(e, d, audio ? 9u : 7u);
}

static void interrupt(struct emit *e, uint8_t addr, unsigned period_ms)
{
	unsigned iv = period_ms;

	if (e->high) {
		/* 2^(bInterval-1) microframes of 125 us */
		for (iv = 1; (1u << (iv - 1u)) < period_ms * 8u; iv++)
			;
	}
	{
		const uint8_t d[7] = { 7, 0x05, addr, 0x03, INT_MPS, 0,
				       (uint8_t)iv };

		bytes(e, d, sizeof(d));
	}
}

static void midi(struct emit *e, unsigned n)
{
	/* S-10a.013: USB-MIDI 1.0 Appendix B with jack IDs 0x15-0x18. */
	static const uint8_t ms_cs[] = {
		7, 0x24, 0x01, 0x00, 0x01, 0x25, 0x00,
		6, 0x24, 0x02, 0x01, 0x15, 0x00,
		6, 0x24, 0x02, 0x02, 0x16, 0x00,
		9, 0x24, 0x03, 0x01, 0x17, 0x01, 0x16, 0x01, 0x00,
		9, 0x24, 0x03, 0x02, 0x18, 0x01, 0x15, 0x01, 0x00,
	};
	static const uint8_t gen_in[] = { 5, 0x25, 0x01, 0x01, 0x15 };
	static const uint8_t gen_out[] = { 5, 0x25, 0x01, 0x01, 0x17 };
	const uint8_t ac[9] = { 9, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01,
				(uint8_t)(n + 1u) };

	interface(e, n, 0, 0x01, 0x01, 0x00);
	bytes(e, ac, sizeof(ac));
	interface(e, n + 1u, 2, 0x01, 0x03, 0x00);
	bytes(e, ms_cs, sizeof(ms_cs));
	bulk(e, S10A_EP_BULK_OUT, true);
	bytes(e, gen_in, sizeof(gen_in));
	bulk(e, S10A_EP_BULK_IN, true);
	bytes(e, gen_out, sizeof(gen_out));
}

static void cdc(struct emit *e, unsigned n)
{
	const uint8_t cs[] = {
		5, 0x24, 0x00, 0x00, 0x02,			/* Header */
		5, 0x24, 0x06, (uint8_t)n, (uint8_t)(n + 1u),	/* Union */
		5, 0x24, 0x01, 0x00, (uint8_t)(n + 1u),		/* Call Mgmt */
		4, 0x24, 0x02, 0x02,				/* ACM */
	};

	interface(e, n, 1, 0x02, 0x02, 0x01);
	bytes(e, cs, sizeof(cs));
	interrupt(e, S10A_EP_CDC_NOTIFY, 16);
	interface(e, n + 1u, 2, 0x0A, 0x00, 0x00);
	bulk(e, S10A_EP_CDC_OUT, false);
	bulk(e, S10A_EP_CDC_IN, false);
}

static void msc(struct emit *e, unsigned n)
{
	interface(e, n, 2, 0x08, 0x06, 0x50);
	bulk(e, S10A_EP_BULK_OUT, false);
	bulk(e, S10A_EP_BULK_IN, false);
}

static void mtp(struct emit *e, const struct set_info *s, unsigned n,
		bool vendor)
{
	interface(e, n, 3, vendor ? 0xFF : 0x06, 0x01, 0x01);
	bulk(e, mtp_in(s), false);
	bulk(e, mtp_out(s), false);
	interrupt(e, S10A_EP_MTP_EVENT, 64);
}

size_t s10a_desc_config(enum s10a_set set, bool has_serial, unsigned index,
			bool high, bool other_speed,
			uint8_t out[S10A_CONFIG_MAX])
{
	const struct set_info *s = info(set);
	const unsigned cfg_str = first_config_string(has_serial) +
				 index * strings_per_config(s);
	unsigned iad_str = cfg_str + 1u, i;
	struct emit e = { out, 9, high != other_speed, cfg_str + 1u };

	if (index >= s->n_configs)
		return 0;
	for (i = 0; i < s->n_fns; i++)
		if (s->fns[i].iad)
			e.istr++;
	for (i = 0; i < s->n_fns; i++) {
		const struct fn_at *f = &s->fns[i];

		if (f->iad) {
			const uint8_t d[8] = { 8, 0x0B, f->first,
				(uint8_t)fn_interfaces(f->fn),
				iad_cls[f->fn][0], iad_cls[f->fn][1],
				iad_cls[f->fn][2], (uint8_t)iad_str++ };

			bytes(&e, d, sizeof(d));
		}
		switch (f->fn) {
		case S10A_FN_MIDI:
			midi(&e, f->first);
			break;
		case S10A_FN_CDC:
			cdc(&e, f->first);
			break;
		case S10A_FN_MSC:
			msc(&e, f->first);
			break;
		case S10A_FN_MTP:
		case S10A_FN_MTP_VENDOR:
		default:
			mtp(&e, s, f->first, f->fn == S10A_FN_MTP_VENDOR);
			break;
		}
	}
	if (e.n > S10A_CONFIG_MAX)
		return 0;
	out[0] = 9;
	out[1] = (uint8_t)(other_speed ? 0x07 : 0x02);
	wb_put_le16(out + 2, (uint16_t)e.n);
	out[4] = (uint8_t)s10a_desc_num_interfaces(set);
	out[5] = (uint8_t)(index + 1u);
	out[6] = (uint8_t)cfg_str;
	out[7] = 0xC0;		/* self-powered, no remote wakeup */
	out[8] = s->powers[index];
	return e.n;
}

bool s10a_set_for_state(enum s10a_state state, bool serial_flag,
			bool dev_mode, enum s10a_set *set)
{
	serial_flag = serial_flag || dev_mode;
	switch (state) {
	case S10A_STATE_MAIN:
	case S10A_STATE_UPGRADE_40:
		*set = serial_flag ? S10A_SET_MIDI_CDC : S10A_SET_MIDI;
		return true;
	case S10A_STATE_UPGRADE:
		*set = serial_flag ? S10A_SET_DISK_CDC : S10A_SET_DISK;
		return true;
	case S10A_STATE_BATTERY:
		*set = S10A_SET_DISK;
		return true;
	case S10A_STATE_FACTORY_MTP_VENDOR:
		*set = dev_mode ? S10A_SET_MTP_VENDOR_CDC : S10A_SET_MTP_VENDOR;
		return true;
	case S10A_STATE_FACTORY_MTP:
		*set = dev_mode ? S10A_SET_MTP_CDC : S10A_SET_MTP;
		return true;
	case S10A_STATE_FACTORY_MTP_OFF:
		*set = S10A_SET_SERIAL;
		return true;
	case S10A_STATE_UPGRADE_KEEP:
	default:
		return false;
	}
}

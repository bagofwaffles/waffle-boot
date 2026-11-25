/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Recovery's descriptors (src/front/fr_usb_desc.c): they parse, and they
 * are S-10a's serial set (S-10a.019, libwaffle's s10a_desc_*) in identity,
 * class layout, endpoints and power, with recovery's own product string;
 * the product and its string per device (S-02.093).
 */
#include <string.h>

#include "support/check.h"

#include "front/fr_usb.h"
#include "waffle/boot/device.h"
#include "waffle/usb/desc.h"

static unsigned le16(const uint8_t *p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8;
}

/* The string descriptor `i` decodes to `text`. */
static bool string_is(unsigned i, const char *text)
{
	uint8_t d[80];
	const size_t n = fr_usb_desc_string(&fr_dev_opxy, i, NULL, d, sizeof d);
	size_t k;

	if (n != 2u + 2u * strlen(text) || d[0] != n || d[1] != 0x03)
		return false;
	for (k = 0; text[k]; k++)
		if (le16(d + 2 + 2 * k) != (unsigned char)text[k])
			return false;
	return true;
}

static void test_device(void)
{
	uint8_t d[32], ref[18];
	char text[S10A_TEXT_MAX];

	CHECK(fr_usb_desc_device(&fr_dev_opxy, d, sizeof d) == 18u);
	CHECK(d[0] == 18 && d[1] == 0x01);
	CHECK(le16(d + 2) == 0x0200u);
	CHECK(d[4] == 0x02 && d[5] == 0x00 && d[6] == 0x00);	/* S-10a.002 */
	CHECK(d[7] == 64);
	CHECK(le16(d + 8) == 0x2367u && le16(d + 10) == 0x0021u);
	CHECK(d[16] == 0);		/* no serial number */
	CHECK(d[17] == 1);
	CHECK(string_is(d[14], "teenage engineering"));
	CHECK(string_is(d[15], "OP-XY Recovery"));

	/* The serial set's device descriptor, with no serial, is the same
	 * but for the product string, which is the same index and differs in
	 * text only. */
	s10a_desc_device(&wb_device_opxy, S10A_SET_SERIAL, false, ref);
	CHECK(memcmp(d, ref, 18) == 0);
	CHECK(s10a_desc_text(&wb_device_opxy, S10A_SET_SERIAL, NULL, ref[15], text) &&
	      strcmp(text, "OP-XY Boot") == 0);

	/* Cut to max. */
	memset(d, 0xEE, sizeof d);
	CHECK(fr_usb_desc_device(&fr_dev_opxy, d, 8) == 8u);
	CHECK(d[0] == 18 && d[7] == 64 && d[8] == 0xEE);
}

static void test_qualifier(void)
{
	uint8_t d[16], ref[10];

	CHECK(fr_usb_desc_qualifier(d, sizeof d) == 10u);
	s10a_desc_qualifier(S10A_SET_SERIAL, ref);
	CHECK(memcmp(d, ref, 10) == 0);
	CHECK(d[0] == 10 && d[1] == 0x06 && le16(d + 2) == 0x0200u);
	CHECK(d[4] == 0x02 && d[7] == 64 && d[8] == 1 && d[9] == 0);
}

struct parsed {
	unsigned n_itf, n_ep;
	uint8_t itf_class[2][3];
	uint8_t ep_addr[3], ep_attr[3], ep_interval[3];
	unsigned ep_mps[3];
	unsigned cs;		/* class-specific descriptors */
};

/* Walk a configuration: every descriptor's length inside wTotalLength. */
static bool parse(const uint8_t *d, size_t total, struct parsed *p)
{
	size_t at = 0;

	memset(p, 0, sizeof *p);
	while (at < total) {
		const uint8_t *x = d + at;

		if (x[0] < 2 || at + x[0] > total)
			return false;
		switch (x[1]) {
		case 0x02:
			if (at != 0 || x[0] != 9)
				return false;
			break;
		case 0x04:
			if (x[0] != 9 || p->n_itf >= 2 || x[2] != p->n_itf ||
			    x[3] != 0)
				return false;
			memcpy(p->itf_class[p->n_itf++], x + 5, 3);
			break;
		case 0x05:
			if (x[0] != 7 || p->n_ep >= 3)
				return false;
			p->ep_addr[p->n_ep] = x[2];
			p->ep_attr[p->n_ep] = x[3];
			p->ep_mps[p->n_ep] = le16(x + 4);
			p->ep_interval[p->n_ep++] = x[6];
			break;
		case 0x24:
			p->cs++;
			break;
		default:
			return false;
		}
		at += x[0];
	}
	return at == total;
}

static void check_config(bool high)
{
	uint8_t d[128], ref[S10A_CONFIG_MAX];
	struct parsed p;
	const unsigned bulk = high ? 512u : 64u;
	size_t n, i;

	n = fr_usb_desc_config(high, d, sizeof d);
	CHECK(n == 67u && n == FR_USB_CONFIG_LEN);
	CHECK(d[0] == 9 && d[1] == 0x02 && le16(d + 2) == n);
	CHECK(d[4] == 2 && d[5] == 1);
	CHECK(d[7] == 0xC0 && d[8] == 250);	/* 500 mA */
	CHECK(parse(d, n, &p));
	CHECK(p.n_itf == 2 && p.n_ep == 3 && p.cs == 4);
	CHECK(memcmp(p.itf_class[0], "\x02\x02\x01", 3) == 0);
	CHECK(memcmp(p.itf_class[1], "\x0A\x00\x00", 3) == 0);
	CHECK(p.ep_addr[0] == FR_USB_EP_NOTIFY && p.ep_attr[0] == 0x03 &&
	      p.ep_mps[0] == 16u && p.ep_interval[0] == (high ? 8 : 16));
	CHECK(p.ep_addr[1] == FR_USB_EP_OUT && p.ep_attr[1] == 0x02 &&
	      p.ep_mps[1] == bulk);
	CHECK(p.ep_addr[2] == FR_USB_EP_IN && p.ep_attr[2] == 0x02 &&
	      p.ep_mps[2] == bulk);

	/* S-10a's serial set byte for byte, but for the string indexes
	 * (iConfiguration at 6, iInterface at 17 and 52), which recovery
	 * leaves at 0. */
	CHECK(s10a_desc_config(S10A_SET_SERIAL, false, 0, high, false, ref) ==
	      n);
	for (i = 0; i < n; i++) {
		if (i == 6 || i == 17 || i == 52) {
			CHECK(d[i] == 0 && ref[i] != 0);
			continue;
		}
		if (d[i] != ref[i])
			printf("  byte %zu: %02X, the serial set %02X\n", i,
			       d[i], ref[i]);
		CHECK(d[i] == ref[i]);
	}
	CHECK(s10a_desc_max_power(S10A_SET_SERIAL, 1) == d[8]);
	for (i = 0; i < 3; i++)
		CHECK(s10a_desc_has_endpoint(S10A_SET_SERIAL, p.ep_addr[i]));

	/* Cut to max, the whole length still returned. */
	memset(d, 0xEE, sizeof d);
	CHECK(fr_usb_desc_config(high, d, 9) == 67u);
	CHECK(d[2] == 67 && d[9] == 0xEE);
}

static void test_config(void)
{
	check_config(false);
	check_config(true);
}

static void test_strings(void)
{
	uint8_t d[80];
	char text[S10A_TEXT_MAX];

	CHECK(fr_usb_desc_string(&fr_dev_opxy, 0, NULL, d, sizeof d) == 4u);
	CHECK(memcmp(d, "\x04\x03\x09\x04", 4) == 0);
	CHECK(string_is(1, "teenage engineering"));
	CHECK(string_is(2, "OP-XY Recovery"));
	CHECK(fr_usb_desc_string(&fr_dev_opxy, 3, NULL, d, sizeof d) == 0u);
	CHECK(fr_usb_desc_string(&fr_dev_opxy, 4, NULL, d, sizeof d) == 0u);
	CHECK(fr_usb_desc_string(&fr_dev_opxy, 255, NULL, d, sizeof d) == 0u);
	/* The product string differs from the serial set's by design. */
	CHECK(s10a_desc_text(&wb_device_opxy, S10A_SET_SERIAL, NULL, 2, text) &&
	      strcmp(text, FR_USB_PRODUCT) != 0);
	CHECK(s10a_desc_text(&wb_device_opxy, S10A_SET_SERIAL, NULL, 1, text) &&
	      strcmp(text, "teenage engineering") == 0);
	/* A serial text, if one is given, is string 3. */
	CHECK(fr_usb_desc_string(&fr_dev_opxy, 3, "AB12", d, sizeof d) == 10u);
	CHECK(d[2] == 'A' && d[8] == '2' && d[9] == 0);
	/* Cut to max. */
	CHECK(fr_usb_desc_string(&fr_dev_opxy, 2, NULL, d, 2) == 2u);
	CHECK(d[0] == 30 && d[1] == 0x03);
}

/* The string descriptor `i` on dev decodes to `text`. */
static bool dev_string_is(const struct fr_dev *dev, unsigned i, const char *text)
{
	uint8_t d[80];
	const size_t n = fr_usb_desc_string(dev, i, NULL, d, sizeof d);
	size_t k;

	if (n != 2u + 2u * strlen(text) || d[0] != n || d[1] != 0x03)
		return false;
	for (k = 0; text[k]; k++)
		if (le16(d + 2 + 2 * k) != (unsigned char)text[k])
			return false;
	return true;
}

/* S-02.093 per device: device.usb_pid and device.product_text + " Recovery";
 * NULL (a device not known) gives the default device's. Every other byte
 * is the same on both. */
static void test_per_device(void)
{
	uint8_t xy[18], f[18], none[18];

	CHECK(fr_usb_desc_device(&fr_dev_opxy, xy, sizeof xy) == 18u);
	CHECK(fr_usb_desc_device(&fr_dev_op1f, f, sizeof f) == 18u);
	CHECK(fr_usb_desc_device(NULL, none, sizeof none) == 18u);
	CHECK(le16(f + 8) == 0x2367u && le16(f + 10) == 0x0102u);
	CHECK(memcmp(none, xy, 18) == 0);
	CHECK(memcmp(f, xy, 10) == 0 && memcmp(f + 12, xy + 12, 6) == 0);
	CHECK(dev_string_is(&fr_dev_op1f, f[15], "OP-1 Recovery"));
	CHECK(dev_string_is(&fr_dev_op1f, 1, "teenage engineering"));
	CHECK(dev_string_is(NULL, 2, "OP-XY Recovery"));
	CHECK(dev_string_is(&fr_dev_opxy, 2, FR_USB_PRODUCT) &&
	      dev_string_is(&fr_dev_op1f, 2, FR_USB_PRODUCT_OP1F));
	CHECK(fr_dev_op1f.usb_pid == FR_USB_PID_OP1F && fr_dev_opxy.usb_pid == FR_USB_PID);
}

int main(void)
{
	RUN(test_device);
	RUN(test_qualifier);
	RUN(test_config);
	RUN(test_strings);
	RUN(test_per_device);
	return check_summary("front_usb");
}

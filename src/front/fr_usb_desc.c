/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Recovery's descriptors (fr_usb.h): S-10a's serial set (S-10a.019) with
 * its identity, class layout, endpoints and power, and recovery's own
 * product string. String indexes are free (S-10a.005); this set names
 * only the manufacturer (1) and the product (2), and no serial number.
 */
#include "fr_usb.h"

#define BCD_USB      0x0200u
#define BCD_DEVICE   0x0257u		/* S-10a.001 */
#define MAX_POWER    (FR_USB_MAX_MA / 2u)	/* units of 2 mA (S-10a.020) */

static const char manufacturer[] = "teenage engineering";	/* S-10a.001 */
static const char product[] = FR_USB_PRODUCT;

/* S-10a.002: Communications at the device level. */
static const uint8_t dev_class[3] = { 0x02, 0x00, 0x00 };

static size_t emit(uint8_t *out, size_t max, const uint8_t *d, size_t n)
{
	fr_copy(out, d, n < max ? n : max);
	return n < max ? n : max;
}

size_t fr_usb_desc_device(uint8_t *out, size_t max)
{
	const uint8_t d[18] = {
		18, 0x01, (uint8_t)BCD_USB, (uint8_t)(BCD_USB >> 8),
		dev_class[0], dev_class[1], dev_class[2], FR_USB_EP0,
		(uint8_t)FR_USB_VID, (uint8_t)(FR_USB_VID >> 8),
		(uint8_t)FR_USB_PID, (uint8_t)(FR_USB_PID >> 8),
		(uint8_t)BCD_DEVICE, (uint8_t)(BCD_DEVICE >> 8),
		1, 2, 0,	/* manufacturer, product, no serial */
		1,		/* one configuration */
	};

	return emit(out, max, d, sizeof d);
}

/* S-10a.003 */
size_t fr_usb_desc_qualifier(uint8_t *out, size_t max)
{
	const uint8_t d[10] = {
		10, 0x06, (uint8_t)BCD_USB, (uint8_t)(BCD_USB >> 8),
		dev_class[0], dev_class[1], dev_class[2], FR_USB_EP0, 1, 0,
	};

	return emit(out, max, d, sizeof d);
}

/*
 * S-10a.012, S-10a.013's CDC function at n = 0, S-10a.021's sizes: the
 * communications interface (Header, Union, Call Management, ACM, the
 * notification endpoint), then the data interface (bulk OUT, bulk IN).
 */
size_t fr_usb_desc_config(bool high_speed, uint8_t *out, size_t max)
{
	const unsigned bulk = high_speed ? FR_USB_BULK_HS : FR_USB_BULK;
	/* 16 ms: in frames at full speed, 2^(8-1) microframes at high speed. */
	const uint8_t interval = high_speed ? 8u : 16u;
	const uint8_t d[FR_USB_CONFIG_LEN] = {
		9, 0x02, FR_USB_CONFIG_LEN, 0, 2, 1, 0, 0xC0, MAX_POWER,
		9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
		5, 0x24, 0x00, 0x00, 0x02,	/* Header, CDC 1.20 */
		5, 0x24, 0x06, 0, 1,		/* Union */
		5, 0x24, 0x01, 0x00, 1,		/* Call Management */
		4, 0x24, 0x02, 0x02,		/* ACM: line coding, state */
		7, 0x05, FR_USB_EP_NOTIFY, 0x03, FR_USB_NOTIFY_MPS, 0,
		interval,
		9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
		7, 0x05, FR_USB_EP_OUT, 0x02, (uint8_t)bulk,
		(uint8_t)(bulk >> 8), 0,
		7, 0x05, FR_USB_EP_IN, 0x02, (uint8_t)bulk,
		(uint8_t)(bulk >> 8), 0,
	};

	(void)emit(out, max, d, sizeof d);
	return sizeof d;
}

/* S-10a.005: ASCII as UTF-16LE, no terminator; index 0 the LANGID list. */
size_t fr_usb_desc_string(unsigned i, const char *serial, uint8_t *out,
			  size_t max)
{
	const char *t;
	uint8_t d[2 + 2 * 31];
	size_t n;

	switch (i) {
	case 0: {
		static const uint8_t lang[4] = { 4, 0x03, 0x09, 0x04 };

		return emit(out, max, lang, sizeof lang);
	}
	case 1:
		t = manufacturer;
		break;
	case 2:
		t = product;
		break;
	case 3:
		/* Named by no descriptor while the device's iSerial is 0. */
		if (!serial || !serial[0])
			return 0;
		t = serial;
		break;
	default:
		return 0;
	}
	for (n = 0; n < (sizeof d - 2u) / 2u; n++) {
		/* cppcheck-suppress arrayIndexOutOfBounds ; t is NUL-terminated and nothing past it is read */
		const char c = t[n];

		if (c == '\0')
			break;
		d[2 + 2 * n] = (uint8_t)c;
		d[3 + 2 * n] = 0;
	}
	d[0] = (uint8_t)(2u + 2u * n);
	d[1] = 0x03;
	return emit(out, max, d, 2u + 2u * n);
}

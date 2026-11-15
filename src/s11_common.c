/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/s11_common.h"

/* S-11.007 */
bool s11_product_ok(uint32_t word)
{
	return ((word >> 14) & 0x3FFu) == 0x21u && ((word >> 10) & 0xFu) == 0;
}

/* S-11.020 */
const char *s11_reason_name(enum s11_reason r)
{
	switch (r) {
	case S11_REASON_SHORT: return "SHORT";
	case S11_REASON_PREAMBLE: return "PREAMBLE";
	case S11_REASON_TYPE: return "TYPE";
	case S11_REASON_MAGIC: return "MAGIC";
	case S11_REASON_PRODUCT: return "PRODUCT";
	case S11_REASON_DEVICE: return "DEVICE";
	case S11_REASON_CRC: return "CRC";
	case S11_REASON_FORMAT: return "FORMAT";
	case S11_REASON_SKU: return "SKU";
	case S11_REASON_LENGTH: return "LENGTH";
	case S11_REASON_HASH: return "HASH";
	case S11_REASON_KEY: return "KEY";
	case S11_REASON_NOKEY: return "NOKEY";
	case S11_REASON_NAME: return "NAME";
	case S11_REASON_ZIPLEN: return "ZIPLEN";
	case S11_REASON_ZIP: return "ZIP";
	case S11_REASON_MEMBER: return "MEMBER";
	case S11_REASON_SPACE: return "SPACE";
	case S11_REASON_SPACEQUERY: return "SPACEQUERY";
	case S11_REASON_SIZE: return "SIZE";
	case S11_REASON_STREAM: return "STREAM";
	case S11_REASON_NONE:
	default: return "";
	}
}

/* S-11.020, S-16.033: Bad file, except SPACE (Failure) and SPACEQUERY
 * (Internal error). */
enum s11_result s11_reason_result(enum s11_reason r)
{
	switch (r) {
	case S11_REASON_SPACE: return S11_FAILURE;
	case S11_REASON_SPACEQUERY: return S11_INTERNAL;
	case S11_REASON_NONE: return S11_OK;
	case S11_REASON_SHORT:
	case S11_REASON_PREAMBLE:
	case S11_REASON_TYPE:
	case S11_REASON_MAGIC:
	case S11_REASON_PRODUCT:
	case S11_REASON_DEVICE:
	case S11_REASON_CRC:
	case S11_REASON_FORMAT:
	case S11_REASON_SKU:
	case S11_REASON_LENGTH:
	case S11_REASON_HASH:
	case S11_REASON_KEY:
	case S11_REASON_NOKEY:
	case S11_REASON_NAME:
	case S11_REASON_ZIPLEN:
	case S11_REASON_ZIP:
	case S11_REASON_MEMBER:
	case S11_REASON_SIZE:
	case S11_REASON_STREAM:
	default: return S11_BAD_FILE;
	}
}

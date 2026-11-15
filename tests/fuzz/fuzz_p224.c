/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * micro-ecc's P-224 signature verification (S-11.012) on untrusted keys,
 * signatures and digests: the input is a public key (56 bytes, x then y),
 * a signature (56 bytes, r then s) and a digest of the remaining 0 to 64
 * bytes. A key is checked before use, as the bootloader does with Q0, and
 * also used unchecked.
 */
#include <stddef.h>
#include <stdint.h>

#include "uECC.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const uint8_t *key = data, *sig = data + 56, *digest = data + 112;
	uECC_Curve curve = uECC_secp224r1();
	size_t n;

	if (size < 112)
		return 0;
	n = size - 112 > 64 ? 64 : size - 112;
	if (uECC_valid_public_key(key, curve))
		uECC_verify(key, digest, (unsigned int)n, sig, curve);
	uECC_verify(key, digest, (unsigned int)n, sig, curve);
	return 0;
}

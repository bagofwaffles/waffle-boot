/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_INSTALL_ENGINE_H
#define WAFFLE_INSTALL_ENGINE_H

/*
 * An engine behind the package's CRCs that can give a wrong answer: the
 * CRC-16 of check 6 (s11_crypto_ops.crc16) and the members' CRC-32s
 * (s11_zip_ops.crc32). The board sets s11_crc_engine while such an engine
 * serves them and clears it after; NULL, the checks take every CRC as it
 * comes.
 */

#include <stdbool.h>

struct s11_crc_engine {
	/*
	 * A member's CRC-32 did not match its stated one (S-11.030): true
	 * when the engine computed some of it unchecked. The CRC-32 is then
	 * the software's for the rest of the boot, and the member is
	 * extracted once more.
	 */
	bool (*crc32_again)(void);
	/*
	 * Check 6 refused the package (S-11.008): the engine, if it
	 * computed some of the CRC-16 unchecked, is not used for the CRC-16
	 * again this boot. The bytes are not read again: the zip may have
	 * been decrypted in place by then (S-11.018).
	 */
	void (*crc16_refused)(void);
};

extern const struct s11_crc_engine *s11_crc_engine;

#endif /* WAFFLE_INSTALL_ENGINE_H */

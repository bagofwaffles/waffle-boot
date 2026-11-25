/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_OTP_H
#define WAFFLE_DRIVERS_BF70X_OTP_H

/*
 * OTP reads (ADSP-BF70x Hardware Reference chapter 13, "One-Time
 * Programmable Memory Controller", and chapter 36's adi_rom_otp_get()).
 *
 * The HRM says every OTP access goes through the boot ROM's API and
 * documents no register-level read: the OTPC has only a status and a
 * security state register, and the HRM gives neither the OTP memory's
 * address nor the ROM API's entry addresses. So the read itself is a
 * function the board supplies (the ROM's adi_rom_otp_get(), however the
 * board reaches it); this module checks the item and the buffer size.
 *
 * Items are the OTPCMD enumerators in the order of HRM Table 36-77, the
 * numbering S-16.095 uses. Word counts come from Table 13-1; only items
 * whose size the HRM gives consistently are readable here.
 *
 * The data sheet's memory map (Figure 3) places the OTP memory at
 * 0x38000000 (1 KB); WB_OTP_MEM_BASE records it, and the default CPLB map
 * gives it an uncached page (HRM: a cached DCPLB makes OTP reads fail).
 * Direct reads are not used here because the HRM routes every access
 * through the ROM.
 *
 * Silicon anomaly 19000061: the factory serial number cannot be read once
 * the part is locked; it has to be copied into GP0 or GP1 before locking.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WB_OTP_MEM_BASE     0x38000000u	/* data sheet Figure 3 */

#define OTPC0_STAT          0x20075004u
#define OTPC0_SECU_STATE    0x2007502Cu

#define OTPC_STAT_ADDRERR(v)        (((v) >> 13) & 3u)
#define OTPC_SECU_STATE_PARTLOCK(v) ((v) & 3u)
#define WB_OTP_PART_OPEN      0u
#define WB_OTP_PART_LOCKED    1u
#define WB_OTP_PART_UNLOCKED  2u

/* OTPCMD (HRM Table 36-77). */
#define WB_OTP_HUK            0x1u	/* 256 bits */
#define WB_OTP_GP0            0x2u
#define WB_OTP_PVT_128KEY0    0x3u	/* 128 bits each */
#define WB_OTP_PVT_128KEY1    0x4u
#define WB_OTP_PVT_128KEY2    0x5u
#define WB_OTP_PVT_128KEY3    0x6u
#define WB_OTP_EK             0x7u	/* 256 bits */
#define WB_OTP_SECURE_EMU_KEY 0x8u	/* 128 bits */
#define WB_OTP_PUBLIC_KEY0    0x9u	/* 512 bits each */
#define WB_OTP_PUBLIC_KEY1    0xAu
#define WB_OTP_BOOT_INFO      0xBu	/* 512 bits */
#define WB_OTP_TIMING         0xCu
#define WB_OTP_ANTIROLL       0xDu
#define WB_OTP_GP1            0xEu	/* 512 bits */
#define WB_OTP_ITEMS          0x12u	/* otpcmd_reserved1 */

/* The ROM's adi_rom_otp_get(): true when the item read. */
typedef bool (*wb_otp_get_fn)(uint32_t item, uint32_t *data);

#define WB_OTP_OK        0
#define WB_OTP_ERR_ARG  (-1)	/* unknown item or buffer too small */
#define WB_OTP_ERR_READ (-2)	/* the ROM call failed */

/* The item's size in 32-bit words, or 0 when the HRM leaves it unclear. */
unsigned wb_otp_words(uint32_t item);

/*
 * Read item into data (nwords words, at least wb_otp_words(item)). The
 * buffer is zeroed first, so a failed read never leaves a partial key.
 */
int wb_otp_read(wb_otp_get_fn get, uint32_t item, uint32_t *data,
		size_t nwords);

#endif /* WAFFLE_DRIVERS_BF70X_OTP_H */

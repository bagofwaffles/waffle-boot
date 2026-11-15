/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_RCU_H
#define WAFFLE_DRIVERS_BF70X_RCU_H

/*
 * Reset Control Unit registers (ADSP-BF70x Hardware Reference, chapter
 * "Reset Control Unit (RCU)", and its register appendix).
 */

#define RCU0_BASE          0x20000000u
#define RCU0_CTL           (RCU0_BASE + 0x00u)
#define RCU0_STAT          (RCU0_BASE + 0x04u)
#define RCU0_BCODE         (RCU0_BASE + 0x1Cu)
#define RCU0_SVECT0        (RCU0_BASE + 0x20u)
#define RCU0_MSG           (RCU0_BASE + 0x60u)
#define RCU0_MSG_SET       (RCU0_BASE + 0x64u)
#define RCU0_MSG_CLR       (RCU0_BASE + 0x68u)

/* RCU0_STAT: the reset-cause bits, HWRST, HBRST, SSRST and SWRST (W1C). */
#define RCU_STAT_CAUSES    0x0000000Fu

/* RCU0_MSG_CLR: every message bit. */
#define RCU_MSG_ALL        0xFFFFFFFFu

/* RCU0_CTL. */
#define RCU_CTL_SYSRST     (1u << 0)
#define RCU_CTL_SRSTREQEN  (1u << 8)

/* RCU0_BCODE: what the boot ROM skips after a system reset. */
#define RCU_BCODE_NOKERNEL   (1u << 0)
#define RCU_BCODE_NOVECTINIT (1u << 1)
#define RCU_BCODE_HALT       (1u << 2)
#define RCU_BCODE_HBTOVW     (1u << 3)
#define RCU_BCODE_NOMEMINIT  (1u << 4)
#define RCU_BCODE_NOCACHE    (1u << 5)
#define RCU_BCODE_NOFAULTS   (1u << 6)
#define RCU_BCODE_NOPREBOOT  (1u << 9)

/* RCU0_MSG: core 0's L1 has been initialized (parity) since power-on. */
#define RCU_MSG_C0L1INIT   (1u << 16)

#endif /* WAFFLE_DRIVERS_BF70X_RCU_H */

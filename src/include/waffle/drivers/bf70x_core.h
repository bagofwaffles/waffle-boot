/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_CORE_H
#define WAFFLE_DRIVERS_BF70X_CORE_H

/*
 * Core operations that are instructions rather than register loads and
 * stores (Blackfin+ Programming Reference): core synchronization, the
 * interrupt mask, the cycle counter, idle and data cache line control.
 *
 * As with waffle/drivers/mmr.h, the target gets inline instructions and
 * every other build gets external functions, which the host tests provide
 * (tests/unit/support/fake_core.c). Defining WB_CORE_EXTERN forces the
 * external form on the target too.
 */

#include <stdint.h>

/* Core event levels (PRM, Core Event Vector Table). */
#define WB_IVG_EMU   0u
#define WB_IVG_RST   1u
#define WB_IVG_NMI   2u
#define WB_IVG_EVX   3u
#define WB_IVG_HW    5u
#define WB_IVG_TMR   6u
#define WB_IVG_SEC   11u	/* the SEC's interrupt to core 0 (PRM: EVT11) */
#define WB_IVG_MAIN  15u

/* The data cache line length (PRM, L1 Data Cache: 32-byte lines). */
#define WB_DCACHE_LINE 32u

/*
 * Place a function in L1 instruction SRAM (the linker script's .l1.text),
 * for code that changes cache or CPLB settings and so must not run from
 * cacheable memory. Host builds ignore it.
 */
#if defined(__bfin__)
#define WB_L1_CODE __attribute__((section(".l1.text")))
#else
#define WB_L1_CODE
#endif

/*
 * Place zero-initialized data in L1 data SRAM (the linker script's .l1.bss,
 * cleared at start-up), which the data cache never holds: memory a DMA
 * channel reads or writes directly, such as a descriptor array (S-13b.083).
 * Host builds ignore it.
 */
#if defined(__bfin__)
#define WB_L1_BSS __attribute__((section(".l1.bss")))
#else
#define WB_L1_BSS
#endif

/*
 * L1 code (0x11A00000) and DDR (0x80000000) are further apart than a
 * PC-relative CALL or JUMP.L reaches (24 bits, +-16 MB). A function called
 * from the other region carries WB_FAR_CALL (GCC's longcall: the call loads
 * the address into a P register) on its declaration and its definition.
 * The type check builds (clang) ignore it.
 */
#if defined(__bfin__) && !defined(__clang__)
#define WB_FAR_CALL __attribute__((longcall))
#else
#define WB_FAR_CALL
#endif

#if defined(__bfin__) && !defined(WB_CORE_EXTERN)

/* Always inlined, for the reason given in waffle/drivers/mmr.h. */
#define WB_CORE_INLINE static inline __attribute__((always_inline))

/* Wait until earlier core operations, such as L1 control writes, are done. */
WB_CORE_INLINE void wb_core_csync(void)
{
	__asm__ __volatile__("csync;" ::: "memory");
}

/*
 * Disable the maskable interrupts and return the previous IMASK (CLI), or
 * load IMASK (STI). Only these two touch IMASK.
 */
WB_CORE_INLINE uint32_t wb_core_cli(void)
{
	uint32_t mask;

	__asm__ __volatile__("cli %0;" : "=d"(mask) : : "memory");
	return mask;
}

/*
 * Silicon anomaly 19000010: a CLI directly after an STI leaves interrupts
 * disabled, so two NOPs follow every STI.
 */
WB_CORE_INLINE void wb_core_sti(uint32_t mask)
{
	__asm__ __volatile__("sti %0;\n\tnop;\n\tnop;" : : "d"(mask) : "memory");
}

/*
 * The 64-bit cycle counter, CYCLES2:CYCLES. CYCLES2 is read before and
 * after CYCLES, so a carry between the two reads is seen and the read is
 * repeated. The master never writes the counter (S-13a.030); only
 * wb_core_cycles_add() does, on the slave.
 */
WB_CORE_INLINE uint64_t wb_core_cycles(void)
{
	uint32_t hi = 0, lo = 0, hi2 = 0;

	do {
		__asm__ __volatile__("%0 = CYCLES2;" : "=d"(hi));
		__asm__ __volatile__("%0 = CYCLES;" : "=d"(lo));
		__asm__ __volatile__("%0 = CYCLES2;" : "=d"(hi2));
		/* cppcheck-suppress knownConditionTrueFalse ; set by the asm */
	} while (hi != hi2);
	return ((uint64_t)hi << 32) | lo;
}

/*
 * Slave only (S-13a.030, S-09.036): add delta to the counter, modulo
 * 2^64, so a negative delta moves it back. With interrupts masked, the
 * counter is stopped (SYSCFG.CCEN clear), read, written with CYCLES2
 * after CYCLES, and started again; the few cycles it stands still are
 * lost. A deadline taken before the call moves by -delta against the new
 * count, so call it with no wait in progress.
 */
WB_CORE_INLINE void wb_core_cycles_add(uint64_t delta)
{
	uint32_t mask = wb_core_cli();
	uint32_t syscfg, lo, hi;
	uint64_t now;

	__asm__ __volatile__("%0 = SYSCFG;\n\tbitclr(%0, 1);\n\t"
			     "SYSCFG = %0;\n\tcsync;"
			     : "=d"(syscfg) : : "memory");
	__asm__ __volatile__("%0 = CYCLES;" : "=d"(lo) : : "memory");
	__asm__ __volatile__("%0 = CYCLES2;" : "=d"(hi) : : "memory");
	now = (((uint64_t)hi << 32) | lo) + delta;
	lo = (uint32_t)now;
	hi = (uint32_t)(now >> 32);
	__asm__ __volatile__("CYCLES = %0;" : : "d"(lo) : "memory");
	__asm__ __volatile__("CYCLES2 = %0;" : : "d"(hi) : "memory");
	__asm__ __volatile__("%0 = SYSCFG;\n\tbitset(%0, 1);\n\t"
			     "SYSCFG = %0;\n\tcsync;"
			     : "=d"(syscfg) : : "memory");
	wb_core_sti(mask);
}

/*
 * S-13a.030: start the cycle counter, SYSCFG.CCEN (bit 1) set by a
 * read-modify-write that keeps SYSCFG's other bits. CCEN is clear after a
 * core reset and the slave has no first stage to set it, so the INIT stage
 * and the main image each call this before their first CYCLES read or
 * timed wait. The count is not touched: a running counter goes on, a
 * stopped one starts from where it stands.
 */
WB_CORE_INLINE void wb_core_cycles_start(void)
{
	uint32_t syscfg;

	__asm__ __volatile__("%0 = SYSCFG;\n\tbitset(%0, 1);\n\t"
			     "SYSCFG = %0;\n\tcsync;"
			     : "=d"(syscfg) : : "memory");
}

/*
 * S-13a.042: dynamic branch prediction on again, SYSCFG.BPEN (bit 8) set by
 * a read-modify-write that keeps SYSCFG's other bits, once the image's own
 * event vectors are written. The table clear that the entry's BP_CFG write
 * began takes 150 CCLK cycles (PRM, "Configuring The Branch Predictor"):
 * the WB_BP_CLEAR_CYCLES NOPs before the set take at least that many on
 * their own, whatever ran between the two.
 */
#define WB_BP_CLEAR_CYCLES 150u

WB_CORE_INLINE void wb_core_bp_on(void)
{
	uint32_t syscfg;
	unsigned i;

	for (i = 0; i < WB_BP_CLEAR_CYCLES; i++)
		__asm__ __volatile__("nop;");
	__asm__ __volatile__("%0 = SYSCFG;\n\tbitset(%0, 8);\n\t"
			     "SYSCFG = %0;\n\tcsync;"
			     : "=d"(syscfg) : : "memory");
}

WB_CORE_INLINE void wb_core_idle(void)
{
	__asm__ __volatile__("idle;" ::: "memory");
}

/* Write back (FLUSH) or write back and invalidate (FLUSHINV) one line. */
WB_CORE_INLINE void wb_core_flush_line(uint32_t addr)
{
	__asm__ __volatile__("flush [%0];"
			     : : "a"((const void *)(uintptr_t)addr) : "memory");
}

WB_CORE_INLINE void wb_core_flushinv_line(uint32_t addr)
{
	__asm__ __volatile__("flushinv [%0];"
			     : : "a"((const void *)(uintptr_t)addr) : "memory");
}

#else

void wb_core_csync(void);
uint32_t wb_core_cli(void);
void wb_core_sti(uint32_t mask);
uint64_t wb_core_cycles(void);
void wb_core_cycles_start(void);
void wb_core_cycles_add(uint64_t delta);
void wb_core_bp_on(void);
void wb_core_idle(void);
void wb_core_flush_line(uint32_t addr);
void wb_core_flushinv_line(uint32_t addr);

#endif

#endif /* WAFFLE_DRIVERS_BF70X_CORE_H */

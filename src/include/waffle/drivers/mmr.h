/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_MMR_H
#define WAFFLE_DRIVERS_MMR_H

/*
 * Memory-mapped register access.
 *
 * Drivers touch hardware registers only through these functions, never
 * through raw pointers, so that host unit tests can link a fake instead
 * (tests/unit/support/fake_mmr.c) that records every access and answers
 * reads.
 *
 * On the target the accessors are inline volatile accesses. Elsewhere
 * (host builds) they are external functions that the test binary
 * provides. Defining WB_MMR_EXTERN forces the external form on the
 * target too, e.g. to trace accesses.
 *
 * Addresses must be naturally aligned for the access width.
 */

#include <stdint.h>

typedef uint32_t wb_mmr_addr;

/*
 * On the target the accessors are always inlined: code that runs from L1
 * while it changes cache settings (WB_L1_CODE) must not call a copy that
 * -Os left out of line in cacheable memory.
 */
#if defined(__bfin__)
#define WB_MMR_INLINE static inline __attribute__((always_inline))
#else
#define WB_MMR_INLINE static inline
#endif

#if defined(__bfin__) && !defined(WB_MMR_EXTERN)

/*
 * Silicon anomaly 19000058: a system MMR read can hang the core unless the
 * load is 8-byte aligned and followed by three NOPs. Every read is the
 * anomaly list's 16-bit indirect form ("Rx = [Px]" at an aligned address,
 * then NOP; NOP; NOP;); the alignment padding is NOPs (opcode 0x0000).
 * Core MMR reads, which the anomaly spares, pay the same few cycles.
 */
#define WB_MMR_READ_ASM(insn)                                              \
	".p2align 3;\n\t" insn ";\n\tnop;\n\tnop;\n\tnop;"

WB_MMR_INLINE uint8_t wb_mmr_read8(wb_mmr_addr addr)
{
	uint32_t v;

	__asm__ __volatile__(WB_MMR_READ_ASM("%0 = B[%1] (Z)")
			     : "=d"(v)
			     : "a"((const void *)(uintptr_t)addr) : "memory");
	return (uint8_t)v;
}

WB_MMR_INLINE uint16_t wb_mmr_read16(wb_mmr_addr addr)
{
	uint32_t v;

	__asm__ __volatile__(WB_MMR_READ_ASM("%0 = W[%1] (Z)")
			     : "=d"(v)
			     : "a"((const void *)(uintptr_t)addr) : "memory");
	return (uint16_t)v;
}

WB_MMR_INLINE uint32_t wb_mmr_read32(wb_mmr_addr addr)
{
	uint32_t v;

	__asm__ __volatile__(WB_MMR_READ_ASM("%0 = [%1]")
			     : "=d"(v)
			     : "a"((const void *)(uintptr_t)addr) : "memory");
	return v;
}

WB_MMR_INLINE void wb_mmr_write8(wb_mmr_addr addr, uint8_t val)
{
	*(volatile uint8_t *)(uintptr_t)addr = val;
}

WB_MMR_INLINE void wb_mmr_write16(wb_mmr_addr addr, uint16_t val)
{
	*(volatile uint16_t *)(uintptr_t)addr = val;
}

WB_MMR_INLINE void wb_mmr_write32(wb_mmr_addr addr, uint32_t val)
{
	*(volatile uint32_t *)(uintptr_t)addr = val;
}

/*
 * Wait until earlier register writes have completed before continuing
 * (system synchronization). Drivers call it wherever the hardware needs
 * a write to have taken effect before the next step.
 */
WB_MMR_INLINE void wb_mmr_sync(void)
{
	__asm__ __volatile__("ssync" ::: "memory");
}

#else

uint8_t wb_mmr_read8(wb_mmr_addr addr);
uint16_t wb_mmr_read16(wb_mmr_addr addr);
uint32_t wb_mmr_read32(wb_mmr_addr addr);
void wb_mmr_write8(wb_mmr_addr addr, uint8_t val);
void wb_mmr_write16(wb_mmr_addr addr, uint16_t val);
void wb_mmr_write32(wb_mmr_addr addr, uint32_t val);
void wb_mmr_sync(void);

#endif

/* Read-modify-write helpers: one read, then one write of the result. */

WB_MMR_INLINE void wb_mmr_modify32(wb_mmr_addr addr, uint32_t clear,
				   uint32_t set)
{
	wb_mmr_write32(addr, (wb_mmr_read32(addr) & ~clear) | set);
}

WB_MMR_INLINE void wb_mmr_set32(wb_mmr_addr addr, uint32_t bits)
{
	wb_mmr_modify32(addr, 0, bits);
}

WB_MMR_INLINE void wb_mmr_clear32(wb_mmr_addr addr, uint32_t bits)
{
	wb_mmr_modify32(addr, bits, 0);
}

WB_MMR_INLINE void wb_mmr_modify16(wb_mmr_addr addr, uint16_t clear,
				   uint16_t set)
{
	wb_mmr_write16(addr,
		       (uint16_t)((wb_mmr_read16(addr) & ~clear) | set));
}

#endif /* WAFFLE_DRIVERS_MMR_H */

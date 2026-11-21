/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The hand-over to the OS on the OP-XY (S-05.008-S-05.016): the teardown,
 * the configuration block, the load function and the kernel call. From
 * the kernel call on, everything the bootloader runs or touches is in the
 * L2 handover reserve (S-01.013, scenario M6): the load function below and
 * opxy_handover_entry.S, the block, the header buffer, the stack and what
 * the OS gets (opxy_handover_os). tools/
 * layout.py checks that nothing in that section refers outside it.
 */
#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_ho_copy.h"

#include <stddef.h>
#include <string.h>

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_coremmr.h"
#include "waffle/drivers/bf70x_cplb.h"
#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_dmc.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/bf70x_rcu.h"
#include "waffle/drivers/bf70x_sec.h"
#include "waffle/drivers/bf70x_spi.h"
#include "waffle/drivers/bf70x_time.h"
#include "waffle/drivers/bf70x_timer.h"
#include "waffle/drivers/bf70x_uart.h"
#include "waffle/drivers/mmr.h"
#include "waffle/devhost.h"
#include "waffle/s01_memmap.h"

/* The test OS (waffle/testos.h) never hands over and may not load into the
 * reserve (S-01.017): these become ordinary DDR sections there. */
#if defined(__bfin__) && !defined(OPXY_TEST_OS)
#define HO_TEXT __attribute__((section(".handover.text"), noinline))
#define HO_DATA __attribute__((section(".handover.data"), aligned(4)))
#define HO_STACK __attribute__((section(".handover.stack"), aligned(4)))
#else
#define HO_TEXT
#define HO_DATA
#define HO_STACK
#endif

/* S-05.016's PINT rows (HRM Table A-40, A-41). */
#define PINT0_MSK_CLR PINT_MSK_CLR(0u)
#define PINT1_MSK_CLR PINT_MSK_CLR(1u)

/* The kernel's stack, in the reserve (S-05.008, S-05.015): at least 4 KiB,
 * linked first in the reserve, so its downward path meets neither the
 * configuration block nor the header buffer (tools/layout.py checks). */
#define HO_STACK_BYTES 0x1000u

/* S-05.014: what opxy_handover_entry.S gives the OS, in the reserve since it
 * is read after the load. The teardown fills it for its CPU. */
HO_DATA struct opxy_os_entry opxy_handover_os;
_Static_assert(offsetof(struct opxy_os_entry, fp) == 4u &&
	       offsetof(struct opxy_os_entry, evt) == 8u,
	       "opxy_handover_entry.S reads these offsets");

static HO_DATA uint8_t config_block[S05_CONFIG_SIZE];
static HO_DATA uint8_t header_buffer[S05_LDR_HEADER];
static HO_STACK uint32_t ho_stack[HO_STACK_BYTES / 4u];

/* opxy_handover_entry.S: SP into the reserve, the kernel, opxy_handover_os,
 * RCU0_SVECT0, the OS. */
WB_FAR_CALL void opxy_handover_enter(uint32_t config_addr, uint32_t sp);

static HO_TEXT void reg_write(uint32_t addr, uint32_t v)
{
	*(volatile uint32_t *)(uintptr_t)addr = v;
}

static HO_TEXT uint32_t reg_read(uint32_t addr)
{
	return *(volatile uint32_t *)(uintptr_t)addr;
}

/*
 * OPXY_HO_SLAVE_MARKS: the slave's hand-over steps as high pulses on PA4, for
 * the master's line window (the slave cannot talk once its bridge is down).
 * Reserve code with direct register writes, since it also runs inside the
 * kernel call and after it. ho_marks_cpu is the slave's cycles per microsecond
 * while the option is set, else 0: the master never sets it, so its own load
 * function and entry make no pulses.
 */
static HO_DATA uint32_t ho_marks_cpu;
static HO_DATA uint32_t ho_marks_load;
/* This CPU's cycles per microsecond, set by the teardown, for the bounded
 * waits that run in the reserve. */
static HO_DATA uint32_t ho_cpu_us;

/* 32-bit cycle deltas, wrap-safe, so no library call leaves the reserve. */
static HO_TEXT void ho_wait_us(uint32_t us)
{
	const uint32_t start = (uint32_t)wb_core_cycles();
	const uint32_t n = us * ho_marks_cpu;

	while ((uint32_t)wb_core_cycles() - start < n)
		;
}

/* S-05.016: the DDR controller idle, at most 1 ms; from the reserve, so no
 * fetch of this loop reaches DDR. */
static HO_TEXT void ho_dmc_idle(void)
{
	const uint32_t start = (uint32_t)wb_core_cycles();
	const uint32_t n = 1000u * ho_cpu_us;

	while ((wb_mmr_read32(DMC0_STAT) & DMC_STAT_IDLE) == 0u &&
	       (uint32_t)wb_core_cycles() - start < n)
		;
}

static HO_TEXT void ho_pulse(uint32_t step)
{
	if (ho_marks_cpu == 0u)
		return;
	/* wb_mmr_write32 is always inlined on the target (waffle/drivers/mmr.h) */
	wb_mmr_write32(PORT_DATA_SET(WB_PORT_A), 1u << 4);	/* PA4 high */
	ho_wait_us(step * OPXY_HO_MARK_MS * 1000u);
	wb_mmr_write32(PORT_DATA_CLR(WB_PORT_A), 1u << 4);	/* PA4 low */
	ho_wait_us(OPXY_HO_MARK_MS * 1000u);
}

/*
 * A timing build (OPXY_BOOT_TIMING): the slave's own steps after its server
 * stop as low pulses of OPXY_HO_BEACON_US on its attention line PB2, which
 * the master reads as PC5 while it waits for PA6 (S-05.032): its teardown
 * begun, the kernel call next, the kernel returned, the jump next. PB2 is
 * high again before the jump, so the OS finds it as without them, and PA4,
 * the ready line, is not touched. ho_beacon_on is set by the slave's
 * teardown alone.
 */
#if OPXY_BOOT_TIMING
static HO_DATA uint32_t ho_beacon_on;

static HO_TEXT void ho_beacon(void)
{
	const uint32_t start = (uint32_t)wb_core_cycles();
	const uint32_t n = OPXY_HO_BEACON_US * ho_cpu_us;

	if (ho_beacon_on == 0u)
		return;
	wb_mmr_write32(PORT_DATA_CLR(WB_PORT_B), 1u << 2);	/* PB2 low */
	while ((uint32_t)wb_core_cycles() - start < n)
		;
	wb_mmr_write32(PORT_DATA_SET(WB_PORT_B), 1u << 2);	/* PB2 high */
}
#define HO_BEACON(on) (ho_beacon_on = (on))
#else
#define ho_beacon() ((void)0)
#define HO_BEACON(on) ((void)0)
#endif

/* The hand-over options in force; 0 (the normal hand-over) until a developer
 * request sets them (waffle/devhost.h). */
volatile uint32_t opxy_ho_flags;
volatile uint32_t opxy_ho_trace;
volatile uint32_t opxy_ho_slave_delay_ms;

/* A hand-over milestone, recorded only on a trace run (waffle/devhost.h). */
static void ho_mark(uint32_t code)
{
	if (OPXY_HO_BIT(OPXY_HO_TRACE))
		opxy_ho_trace = code;
}

/* S-05.014's CPLBs and caches, installed by opxy_ho_finish() from the
 * reserve after the kernel returns. */
HO_DATA struct opxy_ho_cache opxy_ho_cache;

/*
 * One side of opxy_ho_cache, in the PRM's order (wb_cplb_setup()): the
 * control value without ENCPLB, which configures the cache and initializes
 * its tags, the default, the sixteen descriptor pairs, a CSYNC, then the
 * whole value. The reserve is in L2's uncached page of both tables, so this
 * runs after the kernel call (S-05.015). MMRs only; wb_mmr_* is always inlined
 * on the target.
 */
static void HO_TEXT ho_side(wb_mmr_addr ctl, wb_mmr_addr dflt_reg, wb_mmr_addr addr0,
			    /* cppcheck-suppress constParameter ; a const array arg trips ISO C's pointer-qualifier rule at the call */
			    wb_mmr_addr data0, uint32_t value, uint32_t dflt, uint32_t cplb[][2])
{
	unsigned i;

	wb_mmr_write32(ctl, value & ~(uint32_t)L1IM_ICTL_ENCPLB);
	wb_mmr_sync();
	wb_mmr_write32(dflt_reg, dflt);
	for (i = 0; i < 16u; i++) {
		wb_mmr_write32(data0 + 4u * i, cplb[i][0]);
		wb_mmr_write32(addr0 + 4u * i, cplb[i][1]);
	}
	wb_core_csync();
	wb_mmr_write32(ctl, value);
	wb_mmr_sync();
}

/* S-05.011, S-05.014, S-05.016: the last C before the jump. */
void HO_TEXT opxy_ho_finish(void)
{
	ho_beacon();
	ho_pulse(OPXY_HO_MARK_RETURNED);
	if (opxy_ho_cache.on != 0u) {
		ho_side(L1IM_ICTL, L1IM_ICPLB_DFLT, L1IM_ICPLB_ADDR(0), L1IM_ICPLB_DATA(0),
			opxy_ho_cache.ictl, opxy_ho_cache.idflt, opxy_ho_cache.icplb);
		ho_side(L1DM_DCTL, L1DM_DCPLB_DFLT, L1DM_DCPLB_ADDR(0), L1DM_DCPLB_DATA(0),
			opxy_ho_cache.dctl, opxy_ho_cache.ddflt, opxy_ho_cache.dcplb);
		wb_core_csync();
	}
	ho_dmc_idle();
	ho_pulse(OPXY_HO_MARK_JUMP);
	ho_beacon();
}

/*
 * S-05.031's pages, which S-05.014's control value turns on: L2 in four
 * 64 KB pages with the last (the reserve's) uncached, the boot ROM, DDR, the
 * SPI2 window; on the data side DDR in four 64 MB pages, and 4 KB uncached
 * pages at the display bank and the OTP. Data pages are write-through, so no
 * line is ever dirty, with DIRTY set as the PRM asks when writes are not
 * tracked. Unlocked: the OS installs its own. L1 takes the defaults.
 */
#define HO_I(cache, size) (CPLB_VALID | CPLB_UREAD | (cache) | WB_CPLB_PSIZE(size))
#define HO_D(cache, size) (CPLB_VALID | CPLB_UREAD | DCPLB_UWRITE | DCPLB_SWRITE | \
			   DCPLB_DIRTY | (cache) | WB_CPLB_PSIZE(size))
#define HO_DRO(cache, size) (CPLB_VALID | CPLB_UREAD | (cache) | WB_CPLB_PSIZE(size))

const struct wb_cplb opxy_ho_icplb[] = {
	{ 0x08000000u, HO_I(ICPLB_CACHE, WB_CPLB_64K) },
	{ 0x08010000u, HO_I(ICPLB_CACHE, WB_CPLB_64K) },
	{ 0x08020000u, HO_I(ICPLB_CACHE, WB_CPLB_64K) },
	{ 0x08030000u, HO_I(ICPLB_NOCACHE, WB_CPLB_64K) },
	{ 0x04000000u, HO_I(ICPLB_CACHE, WB_CPLB_1M) },
	{ 0x80000000u, HO_I(ICPLB_CACHE, WB_CPLB_256M) },
	{ 0x40000000u, HO_I(ICPLB_CACHE, WB_CPLB_64M) },
	{ 0x44000000u, HO_I(ICPLB_CACHE, WB_CPLB_64M) },
};
const struct wb_cplb opxy_ho_dcplb[] = {
	{ 0x08000000u, HO_D(DCPLB_WT, WB_CPLB_64K) },
	{ 0x08010000u, HO_D(DCPLB_WT, WB_CPLB_64K) },
	{ 0x08020000u, HO_D(DCPLB_WT, WB_CPLB_64K) },
	{ 0x08030000u, HO_D(DCPLB_NOCACHE, WB_CPLB_64K) },
	{ 0x04000000u, HO_DRO(DCPLB_WT, WB_CPLB_1M) },
	{ 0x80000000u, HO_D(DCPLB_WT, WB_CPLB_64M) },
	{ 0x84000000u, HO_D(DCPLB_WT, WB_CPLB_64M) },
	{ 0x88000000u, HO_D(DCPLB_WT, WB_CPLB_64M) },
	{ 0x8C000000u, HO_D(DCPLB_WT, WB_CPLB_64M) },
	{ 0x40000000u, HO_DRO(DCPLB_WT, WB_CPLB_64M) },
	{ 0x44000000u, HO_DRO(DCPLB_WT, WB_CPLB_64M) },
	{ 0x70000000u, HO_D(DCPLB_NOCACHE, WB_CPLB_4K) },
	{ 0x38000000u, HO_DRO(DCPLB_NOCACHE, WB_CPLB_4K) },
};
const unsigned opxy_ho_n_icplb = sizeof(opxy_ho_icplb) / sizeof(opxy_ho_icplb[0]);
const unsigned opxy_ho_n_dcplb = sizeof(opxy_ho_dcplb) / sizeof(opxy_ho_dcplb[0]);
/* S-05.031: L1 fetches and L1 data take the defaults. */
#define HO_IDFLT 0x00000300u
#define HO_DDFLT 0x00000F00u

/* S-05.014: 0x207, the CPLBs and caches on with read parity checking; the
 * other bits as found (bit 0 reads 1). One bank of L1 data serves as cache. */
#define HO_ICTL_OWNED (L1IM_ICTL_ENCPLB | L1IM_ICTL_CFG | L1IM_ICTL_RDCHK | \
		       L1IM_ICTL_CBYPASS | L1IM_ICTL_CPRIORST)
#define HO_DCTL_OWNED (L1DM_DCTL_ENCPLB | L1DM_DCTL_CFG_MASK | L1DM_DCTL_DCBS | \
		       L1DM_DCTL_CBYPASS | L1DM_DCTL_RDCHK | L1DM_DCTL_ENX)

static void ho_table(uint32_t out[16][2], const struct wb_cplb *t, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		out[i][0] = t[i].data;
		out[i][1] = t[i].addr;
	}
}

/*
 * S-05.014: SP, FP and the vectors the OS gets, and the cache table. With no
 * option set this is the normal hand-over state; an option bit leaves a piece
 * as the kernel or the bootloader left it instead.
 */
static void ho_entry(struct opxy_os_entry *e)
{
	if (OPXY_HO_BIT(OPXY_HO_NO_SPFP)) {
		e->sp = (uint32_t)(uintptr_t)&ho_stack[HO_STACK_BYTES / 4u] - 12u;
		e->fp = 0;
	}
	if (OPXY_HO_BIT(OPXY_HO_NO_EVT))
		for (unsigned i = 0; i < OPXY_OS_EVTS; i++)
			e->evt[i] = wb_mmr_read32(0x1FC02008u + 4u * i);
	memset(&opxy_ho_cache, 0, sizeof(opxy_ho_cache));
	if (OPXY_HO_BIT(OPXY_HO_CACHE_OFF))
		return;
	opxy_ho_cache.on = 1u;
	opxy_ho_cache.ictl = (wb_mmr_read32(L1IM_ICTL) & ~(uint32_t)HO_ICTL_OWNED) |
			     L1IM_ICTL_ENCPLB | L1IM_ICTL_CFG | L1IM_ICTL_RDCHK;
	opxy_ho_cache.idflt = HO_IDFLT;
	ho_table(opxy_ho_cache.icplb, opxy_ho_icplb, opxy_ho_n_icplb);
	opxy_ho_cache.dctl = (wb_mmr_read32(L1DM_DCTL) & ~(uint32_t)HO_DCTL_OWNED) |
			     L1DM_DCTL_ENCPLB | L1DM_DCTL_CFG_A | L1DM_DCTL_RDCHK;
	opxy_ho_cache.ddflt = HO_DDFLT;
	ho_table(opxy_ho_cache.dcplb, opxy_ho_dcplb, opxy_ho_n_dcplb);
}

/*
 * S-05.010: a store to L1 instruction memory, with L1DM_DCTL.ENX set. The PRM
 * (Extended Data Access) allows it through DAG0 only and at an address that is
 * a multiple of its size; a lone store instruction is issued on DAG0, never
 * in a parallel slot.
 */
static HO_TEXT void ho_l1_word(uint32_t addr, uint32_t v)
{
#if defined(__bfin__)
	__asm__ volatile("[%0] = %1;" : : "a"(addr), "d"(v) : "memory");
#else
	*(volatile uint32_t *)(uintptr_t)addr = v;
#endif
}

static HO_TEXT void ho_l1_byte(uint32_t addr, uint32_t v)
{
#if defined(__bfin__)
	__asm__ volatile("B[%0] = %1;" : : "a"(addr), "d"(v) : "memory");
#else
	*(volatile uint8_t *)(uintptr_t)addr = (uint8_t)v;
#endif
}

static HO_TEXT uint32_t ho_src(uint32_t a)
{
	return *(const volatile uint8_t *)(uintptr_t)a;
}

/*
 * S-05.009-S-05.012: one call of the load function. Every copy is by the CPU;
 * no memory-DMA channel is touched (S-05.010). Into L1 instruction memory
 * with ENX set: bytes up to a word boundary, aligned words, then bytes, and a
 * CSYNC before the return. Into DDR or L2, a block wholly inside one of them:
 * opxy_ho_copy()'s words. Everything else, L1 data among it, is a byte loop.
 * The caches are off since the kernel call (opxy_kernel_call), so the bytes
 * reach memory and no cache line holds them (S-05.011).
 */
HO_TEXT void opxy_load_fn(uint32_t cfg)
{
	const uint32_t src = reg_read(cfg + S05_CFG_PSOURCE);
	const uint32_t dst = reg_read(cfg + S05_CFG_PDEST);
	const uint32_t n = reg_read(cfg + S05_CFG_COUNT);
	const uint32_t from = S01_STAGING_START + src;
	uint32_t i = 0;

	if (ho_marks_load == 0u) {
		ho_marks_load = 1u;
		ho_pulse(OPXY_HO_MARK_LOAD);
	}
	if (n == 0u)
		return;
	if (dst >= S01_L1_CODE_START && dst < S01_L1_CODE_END) {
		const uint32_t saved = wb_mmr_read32(L1DM_DCTL);

		wb_mmr_write32(L1DM_DCTL, saved | L1DM_DCTL_ENX);
		wb_mmr_sync();
		wb_core_csync();
		for (; i < n && ((dst + i) & 3u) != 0u; i++)
			ho_l1_byte(dst + i, ho_src(from + i));
		for (; i + 4u <= n; i += 4u)
			ho_l1_word(dst + i, ho_src(from + i) |
					    ho_src(from + i + 1u) << 8 |
					    ho_src(from + i + 2u) << 16 |
					    ho_src(from + i + 3u) << 24);
		for (; i < n; i++)
			ho_l1_byte(dst + i, ho_src(from + i));
		wb_mmr_sync();
		wb_mmr_write32(L1DM_DCTL, saved);
		wb_mmr_sync();
		wb_core_csync();
	} else if (opxy_ho_words_ok(dst, n)) {
		opxy_ho_copy(dst, from, n);
	} else {
		for (; i < n; i++)
			*(volatile uint8_t *)(uintptr_t)(dst + i) =
				*(const volatile uint8_t *)(uintptr_t)(from + i);
	}
	reg_write(cfg + S05_CFG_PSOURCE, src + n);
}

uint32_t opxy_handover_config(void)
{
	s05_config_build(config_block, (uint32_t)(uintptr_t)header_buffer,
			 (uint32_t)(uintptr_t)opxy_load_fn);
	/* cppcheck-suppress CastAddressToIntegerAtReturn ; a 32-bit target address */
	return (uint32_t)(uintptr_t)config_block;
}

/*
 * S-05.016's SEC row: nothing pending, IEN and SEN clear on every source
 * but those of keep, which get their SEC0_SCTL when set is true and are
 * left as they are otherwise.
 */
static void sec_quiet(const struct opxy_sec_keep *keep, unsigned n, bool set)
{
	uint32_t sid;
	unsigned i;

	for (sid = 0; sid < WB_SEC_SOURCES; sid++) {
		for (i = 0; i < n && keep[i].sid != sid; i++)
			;
		if (i == n) {
			wb_mmr_clear32(SEC0_SCTL(sid), SEC_SCTL_IEN | SEC_SCTL_SEN);
			wb_mmr_write32(SEC0_SSTAT(sid), SEC_SSTAT_PND);
		} else if (set) {
			/* Clear any pending bit before enabling the source, so it
			 * cannot latch to the core between the two writes. */
			wb_mmr_write32(SEC0_SSTAT(sid), SEC_SSTAT_PND);
			wb_mmr_write32(SEC0_SCTL(sid), keep[i].sctl);
		}
		/* A kept source left as found (set false) is untouched. */
	}
	wb_mmr_sync();
}

/* S-05.016, the master's rows but the SEC's. The options of a developer
 * hand-over (waffle/devhost.h) leave some as found; 0 is the normal boot.
 * Each UART's queued bytes leave the pin before it closes (S-13b.072). */
static void teardown_master(struct opxy_master *m)
{
	/* S-05.016 without second-cpu: SPI0 and the bridge's pin interrupts
	 * were never opened and stay as the reset left them (S-13b §6) */
	const bool link = wb_has(opxy_dev(m), WB_F_SECOND_CPU);

	(void)wb_core_cli();
	if (!OPXY_HO_BIT(OPXY_HO_NO_TIMER_STOP)) {
		wb_gptimer_stop_all();
		wb_coretmr_stop();
	}
	if (link)
		wb_spi_disable(OPXY_SPI_LINK);
	if (link && OPXY_HO_BIT(OPXY_HO_MASTER_STOCK)) {
		/* SPI0 off, the rest to the OPXY_HO_SPI0_* values; SLVSEL twice,
		 * as the driver writes it */
		wb_mmr_write32(SPI_RXCTL(OPXY_SPI_LINK), OPXY_HO_SPI0_RXCTL);
		wb_mmr_write32(SPI_TXCTL(OPXY_SPI_LINK), OPXY_HO_SPI0_TXCTL);
		wb_mmr_write32(SPI_CLK(OPXY_SPI_LINK), OPXY_HO_SPI0_CLK);
		wb_mmr_write32(SPI_DLY(OPXY_SPI_LINK), OPXY_HO_SPI0_DLY);
		wb_mmr_write32(SPI_SLVSEL(OPXY_SPI_LINK), OPXY_HO_SPI0_SLVSEL);
		wb_mmr_write32(SPI_SLVSEL(OPXY_SPI_LINK), OPXY_HO_SPI0_SLVSEL);
		wb_mmr_sync();
	}
	if (link) {
		wb_dma_stop(WB_DMA_SPI0_TX);
		wb_dma_stop(WB_DMA_SPI0_RX);
	}
	(void)wb_uart_disable(OPXY_UART_PSOC, &m->hw.uart[OPXY_UART_PSOC],
			      &m->hw.time);
	wb_dma_stop(10u);
	wb_dma_stop(11u);
	(void)wb_uart_disable(OPXY_UART_NORDIC, &m->hw.uart[OPXY_UART_NORDIC],
			      &m->hw.time);
	wb_dma_stop(12u);
	wb_dma_stop(13u);
	if (link) {
		wb_mmr_write32(PINT0_MSK_CLR, 1u << 22);
		wb_mmr_write32(PINT1_MSK_CLR, 1u << 5);
	}
	/* S-05.016: SPI2 disabled, its master-mode bit left set. */
	wb_mmr_write32(SPI_CTL(OPXY_SPI_FLASH), SPI_CTL_MSTR);
	wb_mmr_write32(SPI_RXCTL(OPXY_SPI_FLASH), 0u);
	wb_mmr_write32(SPI_TXCTL(OPXY_SPI_FLASH), 0u);
	wb_mmr_sync();
	if (!OPXY_HO_BIT(OPXY_HO_NO_USB_STOP))
		opxy_usb_stop();
	if (!OPXY_HO_BIT(OPXY_HO_DMA_KEEP)) {
		wb_dma_stop(20u);
		wb_dma_stop(21u);
	}
	if (OPXY_HO_BIT(OPXY_HO_CRC1_STOCK))
		wb_mmr_write32(CRC1_CTL, OPXY_HO_CRC1_CTL);
}

/*
 * S-05.016's DMA row: every channel stopped, and its status showing no run
 * (at most 1 ms each), the display's kept with OPXY_HO_DMA_KEEP. Then the DDR
 * controller idle (S-05.016), at most 1 ms.
 */
static void quiet_dma(const struct wb_time *t)
{
	unsigned ch;

	for (ch = 0; ch < WB_DMA_CHANNELS; ch++) {
		if (OPXY_HO_BIT(OPXY_HO_DMA_KEEP) && (ch == 20u || ch == 21u))
			continue;
		wb_dma_stop(ch);
		(void)wb_poll32(t, DMA_STAT(ch), DMA_STAT_RUN_MASK, 0u, 1000u);
	}
	(void)wb_poll32(t, DMC0_STAT, DMC_STAT_IDLE, DMC_STAT_IDLE, 1000u);
}

/*
 * S-05.032: after a release reply of 0, PA6 low, then high, within
 * T_OSSTART. After the reply session PA6 stays high until the slave's server
 * stop drives it low (S-09.011, S-05.020), and it stays low through the
 * slave's load. Once PA6 reads low, PINT0's latch for PA6 is cleared: in level
 * mode the hardware latches whenever the pin is high, whatever the mask
 * (HRM, PORT chapter), so a later high of any width sets it, and a set latch
 * means PA6 rose after the low was seen. The pin interrupt stays masked.
 */
#define PA6_PINT_BIT (1u << 22)		/* byte 2 = PA.L, bit 6 */

int opxy_os_wait(struct opxy_master *m)
{
	const uint64_t end = wb_time_deadline(&m->hw.time, OPXY_T_OSSTART_US);

	if (OPXY_HO_BIT(OPXY_HO_NO_OS_WAIT) || OPXY_HO_BIT(OPXY_HO_STAY))
		return 0;		/* developer options: no wait */
	wb_mmr_clear32(PINT_ASSIGN(0u), PINT_ASSIGN_B2MAP);	/* PA.L on byte 2 */
	wb_mmr_write32(PINT_EDGE_CLR(0u), PA6_PINT_BIT);	/* level mode */
	wb_mmr_write32(PINT_INV_CLR(0u), PA6_PINT_BIT);		/* high is active */
	wb_mmr_sync();
	OPXY_TIMING_PC5(true);
	while (wb_gpio_get(OPXY_PA6)) {
		OPXY_TIMING_PC5(false);
		if (wb_time_passed(end))
			return -1;
	}
	OPXY_TIME(BOOT_TIME_SERVER_STOP);
	wb_mmr_write32(PINT_LATCH(0u), PA6_PINT_BIT);		/* W1C */
	wb_mmr_sync();
	while ((wb_mmr_read32(PINT_LATCH(0u)) & PA6_PINT_BIT) == 0u) {
		OPXY_TIMING_PC5(false);
		if (wb_time_passed(end))
			return -1;
	}
	OPXY_TIME(BOOT_TIME_SLAVE_OS);
	return 0;
}

/* S-05.016, the master's rows, for the OS. */
void opxy_teardown_master(void *ctx)
{
	struct opxy_master *m = ctx;

	OPXY_TIME(BOOT_TIME_TEARDOWN);
	ho_marks_cpu = 0u;		/* OPXY_HO_SLAVE_MARKS is the slave's alone */
	HO_BEACON(0u);
	ho_cpu_us = m->hw.time.cycles_per_us;
	ho_mark(OPXY_HO_M_TEARDOWN);
	teardown_master(m);
	/* S-05.037, S-13b.062: the MSI controller closed after the last eMMC
	 * access; no command reaches the eMMC */
	if (wb_has(opxy_dev(m), WB_F_EMMC_ON_MAIN) && !OPXY_HO_BIT(OPXY_HO_MSI_ASFOUND))
		(void)opxy_storage_close(&m->st);
	quiet_dma(&m->hw.time);
	ho_mark(OPXY_HO_M_PERIPH);
	/* S-05.016's SEC row; S-05.037 on the OP-1F: source 1 alone kept,
	 * source 72 (MSI0) neither pending nor enabled (provisional) */
	if (!OPXY_HO_BIT(OPXY_HO_SEC_ASFOUND))
		sec_quiet(opxy_sec_keep_master, opxy_dev(m)->id == WB_DEV_OP1F ? OPXY_SEC_KEEP_1F :
										 OPXY_SEC_KEEP_MASTER, true);
	ho_mark(OPXY_HO_M_SEC);
	/* S-05.038: the OP-1F's vectors; SP and FP the master's */
	opxy_handover_os = opxy_dev(m)->id == WB_DEV_OP1F ? opxy_os_entry_op1f :
							      opxy_os_entry_master;
	ho_entry(&opxy_handover_os);
	ho_mark(OPXY_HO_M_ENTRY);
	/* the record for an OS that reads it, once nothing more is timed */
	OPXY_TIME(BOOT_TIME_KERNEL_CALL); OPXY_TIMING_LEAVE();
}

/* S-05.016, the slave's rows: the bridge stopped by S-05.020 step 3, and
 * the MSI controller before the SEC, with the eMMC as S-13b.058 leaves
 * it. A slave whose eMMC never came up does not hand over. */
/* The slave's jump delay (opxy_ho_slave_delay_ms, a timing knob), bounded. */
static void slave_jump_delay(const struct opxy_slave *s)
{
	uint32_t ms = opxy_ho_slave_delay_ms;

	if (ms == 0u)
		return;
	if (ms > 5000u)
		ms = 5000u;
	wb_delay_us(&s->hw.time, ms * 1000u);
}

/*
 * S-05.020 step 3: PA4 a GPIO driven low from the server's stop through the
 * load, so the master's PA6 reads low until the slave's OS takes the line
 * high (S-05.032). Called before SPI2 is disabled, which would leave the line
 * undriven (it reads high on a unit). OPXY_HO_PA4_SPI2 hands it back to SPI2
 * instead, as S-05.031's environment has it.
 */
void opxy_ho_leave_pa4(void)
{
	if (OPXY_HO_BIT(OPXY_HO_PA4_SPI2) && !OPXY_HO_BIT(OPXY_HO_SLAVE_MARKS)) {
		(void)wb_gpio_peripheral(OPXY_PA4);
		return;
	}
	(void)wb_gpio_output(OPXY_PA4, false);	/* DATA low, then out */
	(void)wb_gpio_gpio(OPXY_PA4);		/* then off SPI2: low */
}

void opxy_teardown_slave(void *ctx)
{
	struct opxy_slave *s = ctx;

	(void)wb_core_cli();
	ho_marks_cpu = OPXY_HO_BIT(OPXY_HO_SLAVE_MARKS) ? s->hw.time.cycles_per_us : 0u;
	ho_marks_load = 0u;
	ho_cpu_us = s->hw.time.cycles_per_us;
	HO_BEACON(1u);
	ho_beacon();
	ho_mark(OPXY_HO_M_TEARDOWN);
	ho_pulse(OPXY_HO_MARK_TEARDOWN);
	if (OPXY_HO_BIT(OPXY_HO_NO_SLAVE_TEAR)) {
		opxy_handover_os = opxy_os_entry_slave;
		ho_entry(&opxy_handover_os);
		ho_mark(OPXY_HO_M_ENTRY);
		ho_pulse(OPXY_HO_MARK_ENTRY);
		slave_jump_delay(s);
		ho_pulse(OPXY_HO_MARK_DELAYED);
		ho_beacon();
		return;
	}
	if (!OPXY_HO_BIT(OPXY_HO_NO_TIMER_STOP)) {
		wb_gptimer_stop_all();
		wb_coretmr_stop();
	}
	wb_spi_disable(OPXY_SPI_BRIDGE_SLAVE);
	wb_dma_stop(WB_DMA_SPI2_TX);
	wb_dma_stop(WB_DMA_SPI2_RX);
	if (!OPXY_HO_BIT(OPXY_HO_NO_USB_STOP))
		opxy_usb_stop();
	if (s->st.emmc_up && !OPXY_HO_BIT(OPXY_HO_MSI_ASFOUND))
		(void)wb_msi_leave(&s->hw.msi, &opxy_msi_leave);
	quiet_dma(&s->hw.time);
	ho_mark(OPXY_HO_M_PERIPH);
	ho_pulse(OPXY_HO_MARK_MSI);
	if (!OPXY_HO_BIT(OPXY_HO_SEC_ASFOUND))
		sec_quiet(opxy_sec_keep_slave, OPXY_SEC_KEEP_SLAVE, true);
	ho_mark(OPXY_HO_M_SEC);
	ho_pulse(OPXY_HO_MARK_SEC);
	opxy_handover_os = opxy_os_entry_slave;
	ho_entry(&opxy_handover_os);
	ho_mark(OPXY_HO_M_ENTRY);
	ho_pulse(OPXY_HO_MARK_ENTRY);
	slave_jump_delay(s);
	ho_pulse(OPXY_HO_MARK_DELAYED);
	ho_beacon();
}

/*
 * S-05.008, S-05.011, S-05.015: interrupts off, the caches off (the
 * CPLB-disable leaves them invalid for the OS's own enable), then into
 * the reserve for good.
 */
void opxy_kernel_call(void *ctx, uint32_t config_addr)
{
	(void)ctx;
	(void)wb_core_cli();
	wb_cplb_disable();
#if defined(__bfin__)
	opxy_handover_enter(config_addr,
			    (uint32_t)(uintptr_t)&ho_stack[HO_STACK_BYTES / 4u]);
#else
	(void)config_addr;
	(void)ho_stack;
#endif
}

/* S-05.013. opxy_handover_entry.S reads it itself; this is for the ops. */
uint32_t opxy_read_svect0(void *ctx)
{
	(void)ctx;
	return wb_mmr_read32(RCU0_SVECT0);
}

void opxy_jump(void *ctx, uint32_t entry)
{
	(void)ctx;
	(void)entry;
	/* Reached only if the kernel call returned into C: never on the
	 * target, where opxy_handover_entry.S jumps itself. */
	for (;;)
		wb_core_idle();
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reset entry for the ADSP-BF703 (Blackfin+ Programming Reference, chapter
 * "Events and Sequencing": supervisor mode, the event vector table and
 * returning from the reset event to run at IVG15).
 *
 * The front's _fr_entry jumps here (src/front/fr_handoff.h) in supervisor
 * mode, in the reset event (IVG1), as the boot ROM left it, with the
 * front's stack in L2. This code:
 *   - empties the branch predictor and switches it off, before any
 *     conditional branch, call or return (S-13a.042); wb_sec_init() turns
 *     it on again after the last vector write;
 *   - turns the data and instruction CPLBs and caches off before its first
 *     memory access (S-01.021), as wb_cplb_disable() does;
 *   - starts the cycle counter, SYSCFG.CCEN (S-13a.030);
 *   - writes the OS entry, 0x11A00000, to RCU0_SVECT0 (S-05.013);
 *   - moves SP to the top of the main stack the linker script reserves in
 *     DDR, before anything is pushed;
 *   - clears the circular buffer lengths and loop counters the C ABI
 *     assumes are zero;
 *   - points EVT2 to EVT14 at the fault entry and EVT15 at the main entry;
 *   - raises IVG15 and returns from reset, so that the main entry runs at
 *     the lowest priority level where every interrupt can preempt it;
 *   - there, calls wb_start_c() and then main() with interrupt nesting on.
 *
 * Symbols from C carry a leading underscore. Calls go through a P register
 * so that the target may be anywhere in the address space.
 */

/*
 * ICU_EVT0 = 0x1FC02000 (S-01.012; waffle/drivers/bf70x_coremmr.h). The
 * PRM's chapter 4 table prints 0x1FE02000, a misprint: a vector written
 * there is never taken.
 */
#define EVT_HI 0x1FC0
#define EVT_LO 0x2000

/*
 * L1DM_DCTL and L1IM_ICTL (S-01.012), and the complements of the bits
 * wb_cplb_disable() clears and of RDCHK, off until the L1 pass is done
 * (S-13a.040): DCTL ENCPLB, CFG, RDCHK, ENX (0x0001020E) and ICTL ENCPLB,
 * CFG, RDCHK (0x00000206). tests/unit/test_drv_start.c checks them
 * against waffle/drivers/bf70x_cplb.h.
 */
#define CTL_HI      0x1FC0
#define DCTL_LO     0x0004
#define ICTL_LO     0x1004
#define DCTL_KEEP_H 0xFFFE
#define DCTL_KEEP_L 0xFDF1
#define ICTL_KEEP_H 0xFFFF
#define ICTL_KEEP_L 0xFDF9

/*
 * BP_CFG (S-01.012) and the value S-13a.042 gives: CLRBP with the branch
 * types' enables (PRM, "Configuring The Branch Predictor"). SYSCFG.BPEN is
 * bit 8. tests/unit/test_drv_start.c checks them.
 */
#define BPCFG_HI    0x1FC0
#define BPCFG_LO    0xA000
#define BPCLR_H     0x0277
#define BPCLR_L     0x8001
#define BPEN_BIT    8

/*
 * RCU0_SVECT0 (S-01.012) and the entry S-05.013 has the main image write
 * there: the start of L1 code. tests/unit/test_drv_start.c checks them.
 */
#define SVECT0_HI   0x2000
#define SVECT0_LO   0x0020
#define SVECT_H     0x11A0
#define SVECT_L     0x0000

	.section .wb.reset, "ax"
	.p2align 2

	.global _wb_reset
	.type _wb_reset, STT_FUNC
_wb_reset:
	/*
	 * S-13a.042: this image was loaded over code the first stage and the
	 * front ran, and the predictor keeps entries by address (anomaly
	 * 19000047). The anomaly's steps: CSYNC, clear the tables, BPEN off.
	 * BPEN is set again only once the vectors are ours: wb_sec_init(),
	 * after its EVT11 write, by wb_core_bp_on().
	 */
	CSYNC;
	P0.L = BPCFG_LO;
	P0.H = BPCFG_HI;
	R0.L = BPCLR_L;
	R0.H = BPCLR_H;
	[P0] = R0;
	R0 = SYSCFG;
	BITCLR(R0, BPEN_BIT);
	SYSCFG = R0;
_wb_bp_off:

	/*
	 * S-01.021: the bootloader is entered with the data CPLBs on and
	 * entries for L2, the ROM and the on-chip windows only, so the first
	 * DDR load or store (the stack included) would miss, EXCAUSE 0x26,
	 * with no handler of ours yet. Nothing below touches memory but these
	 * core MMRs until both CPLB sets are off; wb_start_c() then sets up
	 * the bootloader's own entries (S-13a). Already off is fine too, and
	 * no entry the first stage installed is used.
	 */
	P0.L = DCTL_LO;
	P0.H = CTL_HI;
	SSYNC;
	R0 = [P0];
	R1.L = DCTL_KEEP_L;
	R1.H = DCTL_KEEP_H;
	R0 = R0 & R1;
	[P0] = R0;
	SSYNC;
	P0.L = ICTL_LO;
	R0 = [P0];
	R1.L = ICTL_KEEP_L;
	R1.H = ICTL_KEEP_H;
	R0 = R0 & R1;
	[P0] = R0;
	SSYNC;
	CSYNC;
_wb_cplb_off:				/* S-01.021: the first memory use may follow */

	/*
	 * S-13a.030: start the cycle counter (SYSCFG.CCEN) before any time
	 * use. CCEN is clear after a core reset, and nothing but this sets it
	 * on the slave; read-modify-write, the count left as it is.
	 */
	R0 = SYSCFG;
	BITSET(R0, 1);
	SYSCFG = R0;
	CSYNC;

	/*
	 * S-05.013, S-05.027: RCU0_SVECT0 = 0x11A00000, written once here on
	 * both CPUs, before any log line; nothing writes it again, and the
	 * hand-over reads the entry from it after the kernel call.
	 */
	P0.L = SVECT0_LO;
	P0.H = SVECT0_HI;
	R0.L = SVECT_L;
	R0.H = SVECT_H;
	[P0] = R0;
	SSYNC;

	SP.L = _wb_stack_end;
	SP.H = _wb_stack_end;
	FP = SP;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	L3 = R0;
	LC0 = R0;
	LC1 = R0;

	/* Event vectors. EVT0 (emulation), EVT1 (reset), EVT4 (reserved) stay. */
	P0.L = EVT_LO;
	P0.H = EVT_HI;
	R1.L = _wb_fault_entry;
	R1.H = _wb_fault_entry;
	[P0 + 8] = R1;			/* EVT2 NMI */
	[P0 + 12] = R1;			/* EVT3 exception */
	[P0 + 20] = R1;			/* EVT5 hardware error */
	[P0 + 24] = R1;			/* EVT6 core timer */
	[P0 + 28] = R1;
	[P0 + 32] = R1;
	[P0 + 36] = R1;
	[P0 + 40] = R1;
	[P0 + 44] = R1;			/* EVT11 SEC, until wb_sec_init() */
	[P0 + 48] = R1;
	[P0 + 52] = R1;
	[P0 + 56] = R1;			/* EVT14 */
	R1.L = _wb_main_entry;
	R1.H = _wb_main_entry;
	[P0 + 60] = R1;			/* EVT15 */
	CSYNC;

	/* Enable IVG15 only, make it pending and leave the reset event. */
	P1.L = _wb_reset_wait;
	P1.H = _wb_reset_wait;
	R0 = 0x8000 (Z);
	STI R0;
	RAISE 15;
	RETI = P1;
	RTI;
_wb_reset_wait:
	JUMP _wb_reset_wait;
	.size _wb_reset, . - _wb_reset

	.global _wb_main_entry
	.type _wb_main_entry, STT_FUNC
_wb_main_entry:
	/* Saving RETI lets higher levels preempt this one. */
	[--SP] = RETI;
	SP += -12;			/* the callee's argument area (C ABI) */
	P0.L = _wb_start_c;
	P0.H = _wb_start_c;
	CALL (P0);
	P0.L = _main;
	P0.H = _main;
	CALL (P0);
_wb_main_done:
	IDLE;
	JUMP _wb_main_done;
	.size _wb_main_entry, . - _wb_main_entry

	/*
	 * Exceptions, NMI, hardware errors and any interrupt nothing has
	 * claimed: wb_core_fault(SEQSTAT, RETX), which is not expected to
	 * return.
	 */
	.global _wb_fault_entry
	.type _wb_fault_entry, STT_FUNC
_wb_fault_entry:
	R0 = SEQSTAT;
	R1 = RETX;
	SP += -12;
	P0.L = _wb_core_fault;
	P0.H = _wb_core_fault;
	CALL (P0);
_wb_fault_hang:
	IDLE;
	JUMP _wb_fault_hang;
	.size _wb_fault_entry, . - _wb_fault_entry

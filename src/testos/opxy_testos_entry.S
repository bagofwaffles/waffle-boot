/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The test OS's entry (wiki/Test-OS.md, waffle/testos.h): the stream's
 * entry point, reached through the loading bootloader's read of RCU0_SVECT0
 * after its kernel call (S-05.013), on either CPU (S-05.022). Not a reset
 * entry: by S-05.014 the core is in supervisor mode at IVG15 already
 * (IPEND 0x8000), IMASK is 0x8800, SP is 4-byte aligned in the handover
 * reserve, and RETS may hold anything. Nothing of the bootloader runs any
 * more (S-05.015), the clocks and the DDR controller are as its INIT stage
 * left them (S-05.016), and the stream has been loaded under its load
 * function (S-05.009 to S-05.012). This code is the first byte of the
 * image's one DDR block and runs from DDR (bf703_testos.ld): the loading
 * bootloader's hand-over may run from L1 instruction SRAM after the load,
 * so the image loads nothing into L1 or L2.
 *
 * It takes the same road as src/drivers/bf70x/start.S, less the return
 * from the reset event, which has no counterpart here:
 *   - empties the branch predictor and switches it off first, as start.S
 *     does (S-13a.042), since it is entered with prediction on;
 *   - turns the data and instruction CPLBs and caches off before its first
 *     memory access: S-05.011 says no cache line is valid for the image's
 *     bytes, but says nothing of the CPLB tables, and a page the loading
 *     bootloader installed might not cover the test OS's own memory;
 *   - copies .l1.text, the code that must run from L1 (cplb.c's cache and
 *     CPLB set-up, the SEC entry), from its load address in the DDR block
 *     to L1 code (bf703_testos.ld's _wb_l1_text_*): with the data CPLBs
 *     off and L1DM_DCTL.ENX set for the stores (PRM "Extended Data
 *     Access"), word by word through P registers (DAG0), single-issue,
 *     then ENX clear again and a CSYNC before any L1 code is fetched. The
 *     loading bootloader's own code may have been there: it is done with
 *     (S-05.015);
 *   - keeps SP, IPEND, IMASK and RETS as found, in opxy_testos_entry, for
 *     the log, and moves SP to the test OS's own stack before anything is
 *     pushed: the stack it was entered on is the loading bootloader's, in
 *     the handover reserve, which the test OS does not write (S-05.024);
 *   - starts the cycle counter (S-13a.030) and clears the circular buffer
 *     lengths and loop counters;
 *   - points EVT2 to EVT14 at start.S's fault entry and EVT15 at its main
 *     entry, leaves only IVG15 enabled, calls the first milestone paint
 *     (opxy_testos_entry_paint(): red on the panel the loading bootloader
 *     initialized, waffle/testos.h's milestone paints), and jumps to the
 *     main entry, which runs wb_start_c() (the L1 parity pass of S-13a.040
 *     if this is the first start since power-on, which rewrites the words
 *     just copied with themselves, from DDR code; the sections; the CPLB
 *     table and the caches, from the copy in L1) and main() at IVG15 as
 *     after a reset.
 *
 * Where the loading bootloader takes the entry from. S-05.013 says it
 * reads RCU0_SVECT0 after the kernel returns and jumps there, computing
 * nothing itself. What puts the entry in RCU0_SVECT0 is the ROM kernel,
 * from the stream's FIRST block's target address (HRM "Boot Termination
 * and Application Execution", and "Ignore Block": with IGNORE set beside
 * FIRST the entry is still taken); the kernel model
 * (tests/oracle/S-05-os-handoff/kernel_model.py) takes it from the FINAL
 * block's target instead. The stream names this symbol in both, and in
 * the FINAL block's argument too (tools/mkldr.py --no-init), and
 * tools/testos_layout.py fails the build if any differs from the ELF
 * entry or the entry is not the DDR block's first byte. The ROM's default
 * application entry, 0x11A00000 (HRM Table 36-10), is not an address this
 * image can hold anything at (above), so a loader that jumped there
 * whatever the stream says would not enter it; a run under such a
 * loader is what tells.
 *
 * SPEC-Q: S-05.014, S-05.016 and S-16.010 describe the state waffle-boot's
 * own hand-over leaves; what another loading bootloader leaves at its
 * kernel call (the CPLB tables, the SEC's enabled sources, the DMA
 * channels, the display record) is assumed close to it, as S-16.010
 * assumes. The entry above relies on no more than S-05.014 and turns off
 * what it can. The memory its hand-over uses, which S-01.017 does not
 * cover, may include L1 instruction SRAM, so this image stays out of L1.
 */

/* Only the test OS build (make test-os) assembles this; the normal image
 * gets an empty object. */
#if defined(OPXY_TEST_OS)

#define EVT_HI   0x1FC0
#define EVT_LO   0x2000
#define IMASK_LO 0x2104

/* L1DM_DCTL and L1IM_ICTL, and the complements of the bits
 * wb_cplb_disable() clears: DCTL ENCPLB, CFG, ENX and ICTL ENCPLB, CFG.
 * Unlike start.S, RDCHK is kept: the loading bootloader's L1 pass is done
 * and read parity checking stays on (S-13a.040, S-05.014).
 * tests/unit/test_drv_start.c checks them against
 * waffle/drivers/bf70x_cplb.h. */
#define CTL_HI      0x1FC0
#define DCTL_LO     0x0004
#define ICTL_LO     0x1004
#define DCTL_KEEP_H 0xFFFE
#define DCTL_KEEP_L 0xFFF1
#define ICTL_KEEP_H 0xFFFF
#define ICTL_KEEP_L 0xFFF9
/* L1DM_DCTL.ENX (waffle/drivers/bf70x_cplb.h L1DM_DCTL_ENX), bit 16. */
#define DCTL_ENX_BIT 16

/* BP_CFG, its clear value and SYSCFG.BPEN, as start.S has them. */
#define BPCFG_HI    0x1FC0
#define BPCFG_LO    0xA000
#define BPCLR_H     0x0277
#define BPCLR_L     0x8001
#define BPEN_BIT    8

	.section .wb.reset, "ax"
	.p2align 2

	.global _wb_testos_entry
	.type _wb_testos_entry, STT_FUNC
_wb_testos_entry:
	/* The predictor emptied and off before any branch, as start.S does
	 * (S-13a.042): the loading bootloader enters the OS with prediction
	 * on, and this image runs over code it ran, its .l1.text copied over
	 * its L1 code below (anomaly 19000047). wb_sec_init() turns it on
	 * again after the vectors. P0, R0 and R1 are free: S-05.017 passes
	 * nothing in registers. */
	CSYNC;
	P0.L = BPCFG_LO;
	P0.H = BPCFG_HI;
	R0.L = BPCLR_L;
	R0.H = BPCLR_H;
	[P0] = R0;
	R0 = SYSCFG;
	BITCLR(R0, BPEN_BIT);
	SYSCFG = R0;

	/* CPLBs and caches off: only these core MMRs are touched until
	 * both sets are off. */
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

	/* .l1.text into L1 code (bf703_testos.ld): ENX on for the stores to
	 * L1 instruction SRAM, P registers only (DAG0), one instruction per
	 * load or store, as crt.c's L1 pass does. */
	P0.L = DCTL_LO;
	P0.H = CTL_HI;
	R0 = [P0];
	BITSET(R0, DCTL_ENX_BIT);
	[P0] = R0;
	SSYNC;
	P1.L = _wb_l1_text_load;
	P1.H = _wb_l1_text_load;
	P2.L = _wb_l1_text_start;
	P2.H = _wb_l1_text_start;
	P3.L = _wb_l1_text_end;
	P3.H = _wb_l1_text_end;
	CC = P2 < P3 (IU);
	IF !CC JUMP .Lcopied;
.Lcopy:
	R0 = [P1++];
	[P2++] = R0;
	CC = P2 < P3 (IU);
	IF CC JUMP .Lcopy;
.Lcopied:
	SSYNC;
	R0 = [P0];
	BITCLR(R0, DCTL_ENX_BIT);
	[P0] = R0;
	SSYNC;
	CSYNC;

	/* The entry state, for the log (S-05.014). IMASK and IPEND are core
	 * MMRs (ICU_IMASK 0x1FC02104, ICU_IPEND 0x1FC02108). */
	P0.L = _opxy_testos_entry;
	P0.H = _opxy_testos_entry;
	[P0] = SP;
	P1.L = IMASK_LO;
	P1.H = EVT_HI;
	R0 = [P1 + 4];			/* IPEND */
	[P0 + 4] = R0;
	R0 = [P1];			/* IMASK */
	[P0 + 8] = R0;
	R0 = RETS;
	[P0 + 12] = R0;

	/* S-13a.030: the cycle counter, read-modify-write. */
	R0 = SYSCFG;
	BITSET(R0, 1);
	SYSCFG = R0;
	CSYNC;

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

	/* Event vectors, as start.S sets them. EVT0, EVT1, EVT4 stay. */
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

	/* IVG15 alone enabled; the core is at IVG15 already (IPEND 0x8000),
	 * so the main entry runs as it does after start.S's RTI. */
	R0 = 0x8000 (Z);
	STI R0;

	/* The first paint (waffle/testos.h's milestone paints): the strap,
	 * then red over the panel the loading bootloader initialized, before
	 * wb_start_c(). C on the stack set above, with the callee's argument
	 * area (C ABI); it touches no static data of the image, since .bss is
	 * cleared only by wb_start_c(). */
	SP += -12;
	P1.L = _opxy_testos_entry_paint;
	P1.H = _opxy_testos_entry_paint;
	CALL (P1);
	SP += 12;

	P1.L = _wb_main_entry;
	P1.H = _wb_main_entry;
	JUMP (P1);
	.size _wb_testos_entry, . - _wb_testos_entry

#endif /* OPXY_TEST_OS */

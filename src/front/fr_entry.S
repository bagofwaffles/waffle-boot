/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's two entries.
 *
 * _fr_init, the stream's INIT block target (S-01.010), called by the boot
 * ROM with its configuration in R0. As S-01.007 and S-01.021 require, it
 * turns the data CPLBs off before its first store (core MMRs only until
 * then), keeps the entry SP, L1DM_DCTL and L1DM_DCPLB_DFLT in L1 data A,
 * moves to its own stack there, runs fr_init_stage(), and puts the ROM's SP
 * and L1DM_DCTL back before the RTS: the ROM's stack sees no write.
 * fr_entry.stack.json declares that for check_stack.py.
 *
 * _fr_entry, the stream's entry (its FIRST and FINAL blocks' target): the
 * branch predictor emptied and switched off first (S-13a.042), then IMASK
 * bits 5 to 14 cleared (S-01.024: the event table is still the first
 * stage's, and the front polls), the same cache and CPLB set-down as
 * start.S's _wb_reset, the front's own stack in L2, fr_main(), which returns
 * the full bootloader's entry, and a jump to it. The event level and the
 * event table are left as the ROM left them; the full bootloader's entry
 * writes its own vectors before it sets IMASK (fr_handoff.h).
 */

#define CTL_HI     0x1FC0
#define DCTL_LO    0x0004
#define DFLT_LO    0x0010
#define ICTL_LO    0x1004
#define ENCPLB_BIT 1
/* start.S's masks: the data and instruction caches and CPLBs off. */
#define DCTL_KEEP_H 0xFFFE
#define DCTL_KEEP_L 0xFDF1
#define ICTL_KEEP_H 0xFFFF
#define ICTL_KEEP_L 0xFDF9
/* BP_CFG (0x1FC0A000) and the value that clears the predictor's tables. */
#define BPCFG_HI    0x1FC0
#define BPCFG_LO    0xA000
#define BPCLR_H     0x0277
#define BPCLR_L     0x8001
#define BPEN_BIT    8
/* IMASK with bits 5 to 14 clear. */
#define IMASK_KEEP_H 0xFFFF
#define IMASK_KEEP_L 0x801F

	.section .fr_init_text, "ax"
	.p2align 2

	.global _fr_init
	.type _fr_init, STT_FUNC
_fr_init:
	P0.L = DCTL_LO;
	P0.H = CTL_HI;
	SSYNC;
	R1 = [P0];
	R2 = R1;
	BITCLR(R2, ENCPLB_BIT);
	[P0] = R2;
	SSYNC;
	CSYNC;
	P1.L = DFLT_LO;
	P1.H = CTL_HI;
	R2 = [P1];
	P0.L = _fr_init_dctl;
	P0.H = _fr_init_dctl;
	[P0] = R1;
	P0.L = _fr_init_dflt;
	P0.H = _fr_init_dflt;
	[P0] = R2;
	P0.L = _fr_init_sp;
	P0.H = _fr_init_sp;
	[P0] = SP;
	SP.L = _fr_init_stack_end;
	SP.H = _fr_init_stack_end;
	[--SP] = RETS;
	SP += -12;
	P0.L = _fr_init_stage;
	P0.H = _fr_init_stage;
	CALL (P0);			/* R0: the ROM's configuration */
	SP += 12;
	RETS = [SP++];
	P0.L = _fr_init_sp;
	P0.H = _fr_init_sp;
	SP = [P0];
	P0.L = _fr_init_dctl;
	P0.H = _fr_init_dctl;
	R1 = [P0];
	P0.L = DCTL_LO;
	P0.H = CTL_HI;
	SSYNC;
	[P0] = R1;
	SSYNC;
	CSYNC;
	RTS;
	.size _fr_init, . - _fr_init

	.section .text.fr_entry, "ax"
	.p2align 2

	.global _fr_entry
	.type _fr_entry, STT_FUNC
_fr_entry:
	/* S-13a.042: before any conditional branch, call or return. */
	CSYNC;
	P0.L = BPCFG_LO;
	P0.H = BPCFG_HI;
	R0.L = BPCLR_L;
	R0.H = BPCLR_H;
	[P0] = R0;
	R0 = SYSCFG;
	BITCLR(R0, BPEN_BIT);
	SYSCFG = R0;
	/* S-01.024: before the first peripheral write. */
	CLI R0;
	R1.L = IMASK_KEEP_L;
	R1.H = IMASK_KEEP_H;
	R0 = R0 & R1;
	STI R0;
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
	R0 = SYSCFG;			/* the cycle counter on */
	BITSET(R0, 1);
	SYSCFG = R0;
	CSYNC;
	SP.L = _fr_stack_end;
	SP.H = _fr_stack_end;
	FP = SP;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	L3 = R0;
	LC0 = R0;
	LC1 = R0;
	SP += -12;
	P0.L = _fr_main;
	P0.H = _fr_main;
	CALL (P0);			/* R0: the full bootloader's entry */
	P0 = R0;
	JUMP (P0);
	.size _fr_entry, . - _fr_entry

/* The INIT stage's stack, in L1 data A, not loaded. */
	.section .fr_init_stack, "aw", @nobits
	.p2align 3
_fr_init_stack:
	.space 1024
_fr_init_stack_end:

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The last instructions of the bootloader (S-05.008, S-05.013, S-05.014),
 * in the L2 handover reserve with the load function they hand the kernel:
 *
 *   opxy_handover_enter(config_addr in R0, sp in R1)
 *
 * moves SP into the reserve, calls the boot kernel at 0x04000008 with the
 * configuration block's address, and when it returns writes EVT2 to EVT15
 * and SP and FP from opxy_handover_os (S-05.014), turns the CPLBs and
 * caches on, reads RCU0_SVECT0 and jumps there with IMASK = 0x8800. The caller runs at IVG15 with nothing
 * else active and interrupts off, so IPEND reads 0x8000 at the OS's first
 * instruction.
 *
 * S-05.013: when the kernel returns, RCU0_SVECT0 is the entry. R0 is not a result: on
 * a unit it read 0x000900D8 after a load that had gone through, so it is not looked at.
 */

	.section .handover.text, "ax"
	.p2align 2

	.global _opxy_handover_enter
	.type _opxy_handover_enter, STT_FUNC
_opxy_handover_enter:
	CLI R2;
	SP = R1;
	R2 = 0;
	FP = R2;
	SP += -12;			/* the callee's argument area */
	P0.L = 0x0008;			/* S-05.008: the boot kernel */
	P0.H = 0x0400;
	CALL (P0);			/* R0: the configuration block */
	P1.L = _opxy_handover_os;	/* S-05.014: SP, FP, EVT2 to EVT15 */
	P1.H = _opxy_handover_os;
	R1 = [P1++];
	R2 = [P1++];
	P2.L = 0x2008;			/* ICU_EVT2 (start.S) */
	P2.H = 0x1FC0;
	P0 = 14;
.Levt:
	R0 = [P1++];
	[P2++] = R0;
	P0 += -1;
	CC = P0 == 0;
	IF !CC JUMP .Levt;
	CSYNC;
#if !defined(OPXY_TEST_OS)
	/* S-05.011, S-05.014, S-05.016: the CPLBs and caches of
	 * opxy_ho_cache, then the DDR controller idle. The helper is in the
	 * reserve and touches only MMRs; it runs on the reserve stack, and
	 * clobbers R1 and R2 (SP, FP), reloaded after. */
	CALL _opxy_ho_finish;
	P1.L = _opxy_handover_os;
	P1.H = _opxy_handover_os;
	R1 = [P1];
	R2 = [P1 + 4];
#endif
	SP = R1;
	FP = R2;
	/* S-05.014, S-05.015: clear any latched IVG5-IVG14 (ICU_ILAT bits 5-14,
	 * W1C) before STI, so a source latched after the teardown's CLI or during
	 * the kernel call is not taken at EVT11 in the OS's own bytes before its
	 * first instruction. */
	P1.L = 0x210C;			/* ICU_ILAT (0x1FC0210C) */
	P1.H = 0x1FC0;
	R0 = 0x7FE0 (Z);		/* IVG5 to IVG14 */
	[P1] = R0;
	CSYNC;
	P0.L = 0x0020;			/* S-05.013: RCU0_SVECT0 */
	P0.H = 0x2000;
	P0 = [P0];
	R1 = 0x8800 (Z);		/* S-05.014: IMASK */
	STI R1;
	JUMP (P0);
	.size _opxy_handover_enter, . - _opxy_handover_enter

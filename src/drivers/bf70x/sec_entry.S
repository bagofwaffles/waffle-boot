/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * IVG11 entry: every SEC interrupt (S-13a.051). It saves everything the C
 * ABI lets a function change, clears the circular buffer lengths C
 * assumes are zero, calls wb_sec_dispatch() and returns with RTI. Nesting
 * stays off (RETI is not pushed, SYSCFG.SNEN is never set), so one SEC
 * interrupt is served at a time. That also keeps clear of silicon anomaly
 * 19000054 (dynamic branch prediction in a self-nested IVG11 handler,
 * revision 1.0), whose workaround is needed only with self-nesting.
 *
 * Registers are pushed and popped as the Blackfin+ PRM's register file
 * and interrupt service examples show; the accumulators go through R0.
 */

	.section .l1.text, "ax"
	.p2align 2

	.global _wb_sec_entry
	.type _wb_sec_entry, STT_FUNC
_wb_sec_entry:
	[--SP] = ASTAT;
	[--SP] = RETS;
	[--SP] = (R7:0, P5:0);
	[--SP] = I0;
	[--SP] = I1;
	[--SP] = I2;
	[--SP] = I3;
	[--SP] = B0;
	[--SP] = B1;
	[--SP] = B2;
	[--SP] = B3;
	[--SP] = L0;
	[--SP] = L1;
	[--SP] = L2;
	[--SP] = L3;
	[--SP] = M0;
	[--SP] = M1;
	[--SP] = M2;
	[--SP] = M3;
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	[--SP] = LC1;
	[--SP] = LT1;
	[--SP] = LB1;
	R0 = A0.W;
	[--SP] = R0;
	R0.L = A0.X;
	[--SP] = R0;
	R0 = A1.W;
	[--SP] = R0;
	R0.L = A1.X;
	[--SP] = R0;

	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	L3 = R0;
	SP += -12;			/* the callee's argument area (C ABI) */
	P0.L = _wb_sec_dispatch;
	P0.H = _wb_sec_dispatch;
	CALL (P0);
	SP += 12;

	R0 = [SP++];
	A1.X = R0.L;
	R0 = [SP++];
	A1.W = R0;
	R0 = [SP++];
	A0.X = R0.L;
	R0 = [SP++];
	A0.W = R0;
	LB1 = [SP++];
	LT1 = [SP++];
	LC1 = [SP++];
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	M3 = [SP++];
	M2 = [SP++];
	M1 = [SP++];
	M0 = [SP++];
	L3 = [SP++];
	L2 = [SP++];
	L1 = [SP++];
	L0 = [SP++];
	B3 = [SP++];
	B2 = [SP++];
	B1 = [SP++];
	B0 = [SP++];
	I3 = [SP++];
	I2 = [SP++];
	I1 = [SP++];
	I0 = [SP++];
	(R7:0, P5:0) = [SP++];
	RETS = [SP++];
	ASTAT = [SP++];
	RTI;
	.size _wb_sec_entry, . - _wb_sec_entry

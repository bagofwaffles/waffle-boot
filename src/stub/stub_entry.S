/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The stub's entry, the descriptor's main entry (src/front/fr_handoff.h):
 * the front jumps here as it jumped to the main image, in the reset event
 * with its own stack in L2. The stub moves to its own stack, clears the
 * registers the C ABI takes as zero, runs wb_stub_main() and jumps to the
 * address it returns, the main image's entry, with the front's state
 * otherwise as it found it (stub.c). On a refusal (0) it stops here, with
 * nothing of the main image run: the start log counts an unfinished start
 * (S-02.095).
 */

	.section .text.wb_stub_entry, "ax"
	.p2align 2

	.global _wb_stub_entry
	.type _wb_stub_entry, STT_FUNC
_wb_stub_entry:
	SP.L = _wb_stub_stack_end;
	SP.H = _wb_stub_stack_end;
	FP = SP;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	L3 = R0;
	LC0 = R0;
	LC1 = R0;
	SP += -12;
	P0.L = _wb_stub_main;
	P0.H = _wb_stub_main;
	CALL (P0);
	CC = R0 == 0;
	IF CC JUMP _wb_stub_stop;
	P0 = R0;
	JUMP (P0);
_wb_stub_stop:
	JUMP _wb_stub_stop;
	.size _wb_stub_entry, . - _wb_stub_entry

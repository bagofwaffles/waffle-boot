/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ADSP-BF703 link map for the boot loader (S-01.010-S-01.017).
 *
 * Memory from the ADSP-BF70x Hardware Reference memory map and S-01:
 *   - L1 code SRAM 0x11A00000, 48 KB; the 16 KB above it becomes the
 *     instruction cache when the CPLB set-up enables it (S-01.012);
 *   - L1 data A 0x11800000 and B 0x11900000, 16 KB of SRAM each; the next
 *     16 KB of each is the data cache;
 *   - L1 scratch 0x11B00000: nothing is placed there. The reboot record
 *     and the ROM's stack are at its top (S-01.007, S-01.008) and the
 *     first stage's stack may be below them (S-01.009);
 *   - L2 from 0x08002000 up to the handover reserve at 0x0803C000
 *     (S-01.013). The 8 KB below it are the ROM's boot reserve (HRM
 *     Table 36-1, S-01.008): its stack and working state while it loads
 *     a stream; it refuses a block aimed there. tools/layout.py fails a
 *     section or a stream block that meets it;
 *   - the L2 handover reserve, up to the fatal record at 0x0803DFD4
 *     (S-16.104): the code and data that run from the kernel call on;
 *   - the first 3 MiB of the DDR handover reserve, from 0x8803D000
 *     (S-01.013): the main image, its data and its stack (S-13a, Terms).
 *     The rest of DDR is the heap, the RAM disk and the frame buffers
 *     (waffle/board/opxy.h, S-01.016); the region's end keeps the image
 *     out of them.
 *
 * The frozen front (src/front/fr_handoff.h, front.ld) owns L1 code
 * 0x11A04000-0x11A0C000, L1 data A 0x11802000-0x11804000 and L2
 * 0x08002000-0x08031000: the regions here stop short of them, and
 * tools/mkldr.py refuses an image that meets them. The front holds the INIT
 * stage (S-01.010) and the stream's entry; this image is entered at
 * _wb_reset by the front's jump, with the hand-off record written.
 *
 * The loader stream places each section at its run address, so nothing is
 * copied at start-up; .bss and .l1.bss are cleared by wb_start_c(), .noinit
 * is not.
 */

OUTPUT_FORMAT("elf32-bfin")
OUTPUT_ARCH(bfin)
ENTRY(_wb_reset)

MEMORY
{
	l1_code    (rx)  : ORIGIN = 0x11A00000, LENGTH = 0x4000
	l1_data_a  (rw)  : ORIGIN = 0x11800000, LENGTH = 0x2000
	l1_data_b  (rw)  : ORIGIN = 0x11900000, LENGTH = 0x4000
	l2         (rwx) : ORIGIN = 0x08031000, LENGTH = 0xB000
	handover   (rwx) : ORIGIN = 0x0803C000, LENGTH = 0x1FD4
	ddr        (rwx) : ORIGIN = 0x8803D000, LENGTH = 0x00300000
}

/* The main stack, in DDR after .bss. */
MAIN_STACK = 0x10000;

SECTIONS
{
	/* --- L1, loaded before the INIT block --- */

	.l1.text :
	{
		KEEP(*(.wb.reset))
		*(.l1.text .l1.text.*)
	} > l1_code

	.l1.data :
	{
		*(.l1.data .l1.data.*)
		/* The AES tables: L1 is not cached, so a lookup takes the
		 * same time whatever its index. */
		*libbearssl.a:aes_big_dec.o(.rodata .rodata.*)
		*libbearssl.a:aes_common.o(.rodata .rodata.*)
	} > l1_data_a


	.l1.bss (NOLOAD) :
	{
		. = ALIGN(4);
		_wb_l1_bss_start = .;
		*(.l1.bss .l1.bss.*)
		. = ALIGN(4);
		_wb_l1_bss_end = .;
	} > l1_data_b

	/* --- The handover reserve (S-01.013) --- */

	/* The stack first, at the reserve's bottom, so that its downward path
	 * meets nothing of the hand-over (S-05.008). */
	.handover.stack (NOLOAD) :
	{
		. = ALIGN(4);
		*(.handover.stack .handover.stack.*)
	} > handover

	.handover.text :
	{
		KEEP(*(.handover.text .handover.text.*))
	} > handover

	.handover.data :
	{
		. = ALIGN(4);
		*(.handover.data .handover.data.*)
	} > handover

	/* --- DDR, loaded after the INIT block --- */

	.text :
	{
		*(.text .text.*)
	} > ddr

	.rodata :
	{
		/* read by tools/mkldr.py; the running-slot inference compares
		 * these bytes first (src/board/opxy_master.c) */
		KEEP(*(.rodata.opxy_bl_version))
		*(.rodata .rodata.*)
	} > ddr

	.data :
	{
		/* the end of what the running-slot comparison reads */
		_wb_data_start = .;
		. = ALIGN(4);
		*(.data .data.*)
		*(.sdata .sdata.*)
		. = ALIGN(4);
	} > ddr

	.bss (NOLOAD) :
	{
		. = ALIGN(4);
		_wb_bss_start = .;
		*(.sbss .sbss.*)
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(4);
		_wb_bss_end = .;
	} > ddr

	/* Buffers whose every byte is written before it is read (WB_NOINIT):
	 * not cleared at start-up. */
	.noinit (NOLOAD) :
	{
		. = ALIGN(32);
		*(.noinit .noinit.*)
		. = ALIGN(4);
	} > ddr

	.stack (NOLOAD) :
	{
		. = ALIGN(8);
		_wb_stack_start = .;
		. += MAIN_STACK;
		_wb_stack_end = .;
	} > ddr

	_wb_ddr_image_start = ORIGIN(ddr);

	/* L2 below the reserve: free for later use (DMA buffers). */
	_wb_l2_free = ORIGIN(l2);

	/DISCARD/ :
	{
		*(.comment)
		*(.note .note.*)
		*(.eh_frame .eh_frame_hdr)
	}
}

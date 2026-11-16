/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ADSP-BF703 link map for the test OS (wiki/Test-OS.md): the bootloader's
 * held mode built as an OS image that another bootloader loads under its
 * kernel call (S-05.008 to S-05.014). bf703.ld is the bootloader's map;
 * this one differs where an OS image must (S-01.017, S-05.024) and where
 * another loading bootloader's hand-over may differ from waffle-boot's:
 *   - every byte the stream loads goes to DDR, in one block, from
 *     OPXY_TESTOS_IMAGE_START (waffle/board/opxy.h), 0x89000000: the
 *     loading bootloader's hand-over code may run from L1 instruction SRAM
 *     after it has loaded the image's blocks, so a load block there could
 *     overwrite the code that is handing over to it; it may use L2 too,
 *     and DDR just above S-01.013's reserve end (waffle/board/opxy.h). So
 *     no block's destination is in L1 or L2 or below 0x89000000, and
 *     tools/testos_layout.py fails the build if one is;
 *   - the entry is the first byte of that block: _wb_testos_entry
 *     (src/testos/opxy_testos_entry.S), so the ELF entry, the FIRST
 *     block's target, the FINAL block's target and the FINAL block's
 *     argument are all OPXY_TESTOS_IMAGE_START (S-05.013 by every reading
 *     of where the entry comes from, tools/mkldr.py);
 *   - the one piece of code that must run from L1, .l1.text (cplb.c's
 *     CPLB and cache set-up, which the PRM says must not be in cacheable
 *     memory while it changes the cache mode, and the SEC's IVG11 entry),
 *     keeps its L1 run address but is loaded in DDR with the rest
 *     (AT> ddr) and copied into L1 by the entry shim before anything else
 *     runs; _wb_l1_text_load, _wb_l1_text_start and _wb_l1_text_end are
 *     the shim's bounds. .l1.data.* is ordinary DDR data, and .l1.bss
 *     keeps its symbols over an empty DDR range;
 *   - no INIT stage: the clocks and the DDR controller are up when the
 *     image runs (S-05.016), so the stream has no INIT block
 *     (tools/mkldr.py --no-init);
 *   - nothing in the handover reserve, L2 0x0803C000-0x0803E000 or DDR
 *     0x8803D000-0x88A65000 (S-01.013): the loading bootloader's kernel
 *     call runs there. The DDR image ends before the test OS's heap,
 *     which runs on to the staging area;
 *   - nothing in the staging area 0x8A000000-0x8A800000 (S-01.014), where
 *     the image itself was staged, nor in the ROM's boot reserve
 *     0x08000000-0x08002000 or the reboot record and the L1 scratch above
 *     it 0x11B01F00-0x11B02000 (S-01.008): tools/testos_layout.py checks
 *     every section, loaded or not, and check_stream.py's os profile the
 *     stream;
 *   - no L2 at all: no handover sections. opxy_handover.c's code and data become
 *     ordinary DDR sections under OPXY_TEST_OS, and the hand-over entry's
 *     .handover.text is placed with .text; none of it is ever run.
 * The RAM disk, the frame buffers and the display record are where opxy.h
 * puts them for the bootloader (S-01.016).
 */

OUTPUT_FORMAT("elf32-bfin")
OUTPUT_ARCH(bfin)
ENTRY(_wb_testos_entry)

MEMORY
{
	l1_code    (rx)  : ORIGIN = 0x11A00000, LENGTH = 0xC000
	ddr        (rwx) : ORIGIN = 0x89000000, LENGTH = 0x00300000
}

/* The main stack, in DDR after .bss. */
MAIN_STACK = 0x10000;

SECTIONS
{
	/* --- DDR, above the handover reserve: the one block of the stream --- */

	.text :
	{
		/* The entry shim first: the stream's entry is the block's first
		 * byte. start.S's .wb.reset (the main and fault entries) and
		 * everything else follow. */
		KEEP(*libwaffle.a:opxy_testos_entry.o(.wb.reset))
		KEEP(*(.wb.reset))
		*(.text .text.*)
		*(.handover.text .handover.text.*)
	} > ddr

	.rodata :
	{
		*(.rodata .rodata.*)
	} > ddr

	.data :
	{
		. = ALIGN(4);
		*(.data .data.*)
		*(.sdata .sdata.*)
		*(.l1.data .l1.data.*)
		*(.handover.data .handover.data.*)
		. = ALIGN(4);
	} > ddr

	/* Run from L1 code, loaded in DDR right after .data: the entry shim
	 * copies [_wb_l1_text_load, +size) to _wb_l1_text_start. */
	.l1.text : ALIGN(4)
	{
		_wb_l1_text_start = .;
		*(.l1.text .l1.text.*)
		. = ALIGN(4);
		_wb_l1_text_end = .;
	} > l1_code AT> ddr
	_wb_l1_text_load = LOADADDR(.l1.text);

	.bss (NOLOAD) :
	{
		. = ALIGN(4);
		_wb_bss_start = .;
		*(.sbss .sbss.*)
		*(.bss .bss.*)
		*(COMMON)
		*(.handover.stack .handover.stack.*)
		. = ALIGN(4);
		_wb_bss_end = .;
	} > ddr

	/* start_bf703.c clears it like bf703.ld's; nothing is placed in it. */
	.l1.bss (NOLOAD) :
	{
		. = ALIGN(4);
		_wb_l1_bss_start = .;
		*(.l1.bss .l1.bss.*)
		. = ALIGN(4);
		_wb_l1_bss_end = .;
	} > ddr

	.stack (NOLOAD) :
	{
		. = ALIGN(8);
		_wb_stack_start = .;
		. += MAIN_STACK;
		_wb_stack_end = .;
	} > ddr

	/DISCARD/ :
	{
		*(.comment)
		*(.note .note.*)
		*(.eh_frame .eh_frame_hdr)
	}
}

_wb_ddr_image_start = ORIGIN(ddr);
ASSERT(ADDR(.text) == ORIGIN(ddr), "the entry shim must be the first byte of the DDR image")
ASSERT(_wb_testos_entry == ORIGIN(ddr), "the entry must be the first byte of the DDR image")

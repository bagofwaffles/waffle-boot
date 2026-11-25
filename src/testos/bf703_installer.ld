/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ADSP-BF703 link map for the installer's images (make installer,
 * installer-survey; wiki/Installer-Reference.md): bf703_testos.ld's test
 * OS in the one shape both devices' own loaders take (S-05.043), that of
 * the OP-1F's OS 1.7.3 (S-05.040, S-05.041):
 *   - the stream's entry, its FIRST and FINAL targets, is 0x11A00000 and
 *     its FINAL argument 0 (tools/mkldr.py --os-1f); there the stream
 *     loads one small L1 code block, _wb_testos_l1_entry
 *     (src/testos/testos_entry.S), which jumps to the entry shim at
 *     the DDR image's first byte;
 *   - every other loaded byte is one DDR run from
 *     OPXY_TESTOS_SHARED_IMAGE_START (waffle/board/values.h), 0x8D000000,
 *     inside OS 1.7.3's second DDR range 0x8C03D002-0x8FCCDD38: above the
 *     OS frame's rows (S-05.016) and clear of the handover reserve, the
 *     staging area and the OP-1F's RAM disk 0x80004000-0x87F04000. On the
 *     OP-XY the image and its heap lie where the RAM disk's part 1 was,
 *     so there the disk is in three parts around them (board/values.h);
 *   - .l1.text runs in L1 code after the entry block, loaded in DDR and
 *     copied by the shim, as in bf703_testos.ld.
 * tools/testos_layout.py --device both checks every destination against
 * OS 1.7.3's ranges (tests/oracle/S-05-os-handoff/vectors/os_destinations_1f.json)
 * and the image and heap against both devices' RAM disks.
 */

OUTPUT_FORMAT("elf32-bfin")
OUTPUT_ARCH(bfin)
ENTRY(_wb_testos_l1_entry)

MEMORY
{
	l1_entry   (rx)  : ORIGIN = 0x11A00000, LENGTH = 0x100
	l1_code    (rx)  : ORIGIN = 0x11A00100, LENGTH = 0xBF00
	ddr        (rwx) : ORIGIN = 0x8D000000, LENGTH = 0x00300000
}

/* The main stack, in DDR after .bss. */
MAIN_STACK = 0x10000;

SECTIONS
{
	/* --- L1 code at the stream's entry: the one L1 block --- */

	.l1.entry :
	{
		KEEP(*(.wb.l1entry))
	} > l1_entry

	/* --- DDR, in OS 1.7.3's second range: the DDR image --- */

	.text :
	{
		/* The entry shim first: the stream's entry is the block's first
		 * byte. start.S's .wb.reset (the main and fault entries) and
		 * everything else follow. */
		KEEP(*libwaffle.a:testos_entry.o(.wb.reset))
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
		*(.noinit .noinit.*)
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
ASSERT(_wb_testos_entry == ORIGIN(ddr), "the shim must be the first byte of the DDR image")
ASSERT(_wb_testos_l1_entry == ORIGIN(l1_entry), "the entry must be the first byte of L1 code")

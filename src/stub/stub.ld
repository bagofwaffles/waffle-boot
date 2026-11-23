/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The stub's link map (waffle/pack.h): linked alone, from its own objects,
 * its own copy of zlib's inflate and libgcc, into the L2 the main image
 * leaves free between the shared block and its handover reserve
 * (WB_STUB_START to WB_STUB_END). Loaded: code, constants and data. Not
 * loaded: .bss, cleared by the stub, and the stack, below the stub's record
 * at the region's top.
 */
OUTPUT_FORMAT("elf32-bfin")
OUTPUT_ARCH(bfin)
ENTRY(_wb_stub_entry)

MEMORY
{
	stub (rwx) : ORIGIN = 0x08031000, LENGTH = 0xAFE0
}

STUB_STACK = 0x1000;

SECTIONS
{
	/* The first 1 KiB page is uncached for instructions (stub.c): the
	 * entry and the cache changes, then constants and data; the rest of the
	 * code from the next page on, or from where they end. */
	.stub :
	{
		KEEP(*(.text.wb_stub_entry))
		KEEP(*(.text.stub_ctl))
		_wb_stub_ctl_end = .;
		. = ALIGN(4);
		*(.rodata .rodata.*)
		. = ALIGN(4);
		*(.data .data.* .sdata .sdata.*)
		. = ALIGN(4);
	} > stub
	ASSERT(_wb_stub_ctl_end <= 0x08031400, "the stub's cache changes leave its uncached page")

	.stub_text MAX(ADDR(.stub) + SIZEOF(.stub), 0x08031400) :
	{
		*(.text .text.*)
		. = ALIGN(4);
	} > stub

	.stub_bss (NOLOAD) :
	{
		. = ALIGN(8);
		_wb_stub_bss_start = .;
		*(.sbss .sbss.* .bss .bss.* COMMON)
		. = ALIGN(4);
		_wb_stub_bss_end = .;
	} > stub

	.stub_stack (NOLOAD) :
	{
		. = ALIGN(8);
		. += STUB_STACK;
		_wb_stub_stack_end = .;
	} > stub

	.debug_info     0 : { *(.debug_info) }
	.debug_abbrev   0 : { *(.debug_abbrev) }
	.debug_line     0 : { *(.debug_line) }
	.debug_str      0 : { *(.debug_str) }
	.debug_loc      0 : { *(.debug_loc) }
	.debug_ranges   0 : { *(.debug_ranges) }
	.debug_aranges  0 : { *(.debug_aranges) }
	.debug_frame    0 : { *(.debug_frame) }
	.debug_macro    0 : { *(.debug_macro) }
	.debug_pubnames 0 : { *(.debug_pubnames) }
	.debug_pubtypes 0 : { *(.debug_pubtypes) }
	.debug_line_str 0 : { *(.debug_line_str) }
	.debug_rnglists 0 : { *(.debug_rnglists) }
	.debug_loclists 0 : { *(.debug_loclists) }

	/DISCARD/ :
	{
		*(.comment)
		*(.note .note.*)
		*(.eh_frame .eh_frame_hdr)
	}
}

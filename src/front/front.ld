/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The frozen front's link map (handoff.h). The front is linked alone,
 * from its own objects and libgcc, at fixed addresses that bf703.ld leaves
 * free, so its bytes do not depend on the rest of the image:
 *   - L1 code 0x11A04000-0x11A0C000: the INIT stage and all it calls,
 *     libgcc included (S-01.010: nothing outside L1 is loaded before it
 *     runs), and the loops that must be quick (.fr_fast);
 *   - L1 data A 0x11802000-0x11804000: what the INIT stage reads and
 *     writes, and its stack (S-01.007, S-01.021), and the quick loops'
 *     data (.bss.fr_fast), clear of 0x11807000-0x11808000 (S-02.091
 *     rule 4);
 *   - L2 0x08002000-0x08030000: the rest; its .bss and stack are not
 *     loaded and are cleared by the front.
 */
OUTPUT_FORMAT("elf32-bfin")
OUTPUT_ARCH(bfin)
ENTRY(_fr_entry)

MEMORY
{
	fr_l1code (rx)  : ORIGIN = 0x11A04000, LENGTH = 0x8000
	fr_l1data (rw)  : ORIGIN = 0x11802000, LENGTH = 0x2000
	fr_l2     (rwx) : ORIGIN = 0x08002000, LENGTH = 0x2E000
}

FR_STACK = 0x4000;

SECTIONS
{
	.fr_l1code :
	{
		KEEP(*(.fr_init_text .fr_init_text.*))
		*(.fr_fast .fr_fast.*)
		*libgcc.a:*(.text .text.*)
	} > fr_l1code

	.fr_l1data :
	{
		KEEP(*(.fr_init_data .fr_init_data.*))
		*libgcc.a:*(.rodata .rodata.* .data .data.*)
	} > fr_l1data

	.fr_l1stack (NOLOAD) :
	{
		. = ALIGN(8);
		KEEP(*(.fr_init_stack))
	} > fr_l1data

	/* Not loaded and not cleared: the panel's row and what draws it. */
	.fr_l1bss (NOLOAD) :
	{
		. = ALIGN(4);
		*(.bss.fr_fast)
	} > fr_l1data

	.fr_l2 :
	{
		KEEP(*(.text.fr_entry))
		*(.text .text.*)
		. = ALIGN(4);
		*(.rodata .rodata.*)
		. = ALIGN(4);
		*(.data .data.* .sdata .sdata.*)
		. = ALIGN(4);
	} > fr_l2

	.fr_bss (NOLOAD) :
	{
		. = ALIGN(4);
		_fr_bss_start = .;
		*(.sbss .sbss.* .bss .bss.* COMMON)
		. = ALIGN(4);
		_fr_bss_end = .;
	} > fr_l2

	.fr_stack (NOLOAD) :
	{
		. = ALIGN(8);
		. += FR_STACK;
		_fr_stack_end = .;
	} > fr_l2

	/* Not loaded: the debugger's, named so that the link's
	 * --orphan-handling=error passes them. */
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

	/DISCARD/ :
	{
		*(.comment)
		*(.note .note.*)
		*(.eh_frame .eh_frame_hdr)
	}
}

/*
 * The link of the riscv64 UEFI loader.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * The image is linked for address 0 and laid out so that a section's
 * address is also its place in the file: the first page holds the PE
 * header (written by platform/riscv64/tools/make-bootriscv64.py), then the
 * code and the read-only data, then, from the next page, the data with the
 * relocations and the zero-filled data.  The tool rounds each part to whole
 * pages in the file and in memory.  The relocations
 * are the image's own, which its entry applies.
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
	. = 0;
	__image_base = .;

	. = 0x1000;
	.text : {
		KEEP(*(.text.start))
		*(.text .text.*)
		*(.rodata .rodata.* .srodata .srodata.*)
	}
	.dynsym : { *(.dynsym) }
	.dynstr : { *(.dynstr) }
	.hash : { *(.hash) }
	.gnu.hash : { *(.gnu.hash) }

	.data : ALIGN(4096) {
		*(.data .data.* .sdata .sdata.*)
	}
	.got : { *(.got .got.*) }
	.dynamic : { *(.dynamic) }
	.rela.dyn : {
		__rela_start = .;
		*(.rela.dyn .rela.*)
		__rela_end = .;
	}

	.bss : {
		*(.sbss .sbss.* .bss .bss.*)
		*(COMMON)
	}

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) }
}

ASSERT(ADDR(.text) == 0x1000, "the code starts after the PE header's page")

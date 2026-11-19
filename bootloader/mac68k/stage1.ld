/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Links the 68040 Macintosh boot block.  The ROM chooses where the block
 * is in memory, so the code is position independent and is linked at 0.
 */

OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)
SECTIONS
{
	. = 0;
	.text : { *(.text .text.*) *(.rodata .rodata.*) }
	/DISCARD/ : { *(.data .data.*) *(.bss .bss.*) *(.comment) *(.note*) *(.eh_frame*) *(.gnu.attributes) }
}
ASSERT(SIZEOF(.text) <= 1024, "the boot block exceeds 1024 bytes")

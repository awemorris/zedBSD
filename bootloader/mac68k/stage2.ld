/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Links the 68040 Macintosh second stage at 0.  The ROM's heap chooses
 * where it runs; the link keeps the relocations (--emit-relocs), and the
 * tool that finishes the stage lists the 32-bit addresses its entry adds
 * its own address to.
 */

OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)
SECTIONS
{
	. = 0;
	.text : { KEEP(*(.text.start)) *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : ALIGN(4) { *(.data .data.*) }
	__image_end = .;
	.bss (NOLOAD) : ALIGN(4) { __bss_start = .; *(.bss .bss.*) *(COMMON) . = ALIGN(4); __bss_end = .; }
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu.attributes) }
}
ASSERT(_start == 0, "the second stage's entry must be its first byte")
ASSERT(__bss_end <= 0x00040000, "the second stage exceeds the boot block's limit")

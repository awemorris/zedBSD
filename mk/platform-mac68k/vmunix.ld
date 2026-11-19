/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD 68040 Macintosh kernel link layout.
 *
 * The image is linked and loaded at KERNEL_PHYS_LOAD and runs there: the
 * supervisor reaches RAM at its own addresses.  The first 8 KiB stay free
 * (the kernel has its own vector table).  The loader places its boot
 * information right after the end of the last segment, which __kernel_end
 * marks.
 */

OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)

KERNEL_PHYS_LOAD = 0x00002000;

PHDRS
{
	text PT_LOAD;
	data PT_LOAD;
}

SECTIONS
{
	. = KERNEL_PHYS_LOAD;
	__kernel_vma_start = .;

	.text :
	{
		__kernel_text_start = .;
		KEEP(*(.text.boot))
		*(.text .text.*)
		__kernel_text_end = .;
	} :text

	.rodata : ALIGN(4096)
	{
		__kernel_rodata_start = .;
		*(.rodata .rodata.*)
		__kernel_rodata_end = .;
	} :text

	.data : ALIGN(4096)
	{
		__kernel_data_start = .;
		*(.data .data.*)
	} :data

	.bss (NOLOAD) : ALIGN(16)
	{
		__bss_start = .;
		*(.bss.boot)
		*(.vfs_bss)
		*(.bss .bss.* COMMON)
		. = ALIGN(4096);
		KEEP(*(.kernel_heap))
		. = ALIGN(16);
		__bss_end = .;
	} :data

	__kernel_end = .;
	__kernel_vma_end = .;

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

ASSERT(_start == KERNEL_PHYS_LOAD, "_start must be the image's first byte")

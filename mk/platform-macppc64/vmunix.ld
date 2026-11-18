/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD ppc64 kernel link layout (PowerPC Mac).
 *
 * The image runs at KERNEL_PHYS_LOAD, where Open Firmware enters it at its
 * first byte, in 32-bit mode.  Its load addresses start at zero, the way
 * the emulator's loader expects: it adds KERNEL_PHYS_LOAD to each
 * segment's load address.  Every section keeps the same distance from
 * zero as from the image's first byte.
 *
 * The data and the bss share one loadable segment.  Open Firmware claims
 * each segment's range in turn and takes memory for itself right after
 * the range it claimed last; a separate bss segment could find that
 * memory inside its range and fail to load.  The TOC the code addresses
 * its data through is in the data.
 */

OUTPUT_FORMAT("elf64-powerpc")
OUTPUT_ARCH(powerpc:common64)
ENTRY(_start)

PHDRS
{
	text PT_LOAD;
	rodata PT_LOAD;
	data PT_LOAD;
}

KERNEL_PHYS_LOAD = 0x01000000;

SECTIONS
{
	. = KERNEL_PHYS_LOAD;
	__kernel_vma_start = .;

	.text : AT(ADDR(.text) - KERNEL_PHYS_LOAD)
	{
		__kernel_text_start = .;
		KEEP(*(.text.boot))
		*(.text .text.*)
		__kernel_text_end = .;
	} :text

	.rodata : AT(ADDR(.rodata) - KERNEL_PHYS_LOAD) ALIGN(4096)
	{
		__kernel_rodata_start = .;
		*(.rodata .rodata.*)
		__kernel_rodata_end = .;
	} :rodata

	.data : AT(ADDR(.data) - KERNEL_PHYS_LOAD) ALIGN(4096)
	{
		__kernel_data_start = .;
		*(.data .data.*)
		*(.toc .got .toc1)
	} :data

	.bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_PHYS_LOAD) ALIGN(4096)
	{
		__bss_start = .;
		*(.bss.boot)
		*(.vfs_bss)
		*(.bss .bss.* .sbss .sbss.* COMMON)
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
ASSERT(__kernel_end < 0x04000000, "the image must end below the firmware's own memory")

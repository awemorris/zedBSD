/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The image is linked in the kernel window at KERNEL_VIRT_BASE for
 * the physical address KERNEL_PHYS_LOAD, the first megapage after the
 * firmware's.  It runs wherever its first byte is loaded, every
 * section keeps the same distance from the window's base as from the
 * load address.
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

KERNEL_VIRT_BASE = 0xffffffff80000000;
KERNEL_PHYS_LOAD = 0x80200000;

SECTIONS
{
	. = KERNEL_VIRT_BASE;
	__kernel_vma_start = .;

	.text : AT(KERNEL_PHYS_LOAD)
	{
		__kernel_text_start = .;
		KEEP(*(.text.boot))
		*(.hightext)
		*(.text .text.*)
		__kernel_text_end = .;
	}

	.rodata : AT(ADDR(.rodata) - KERNEL_VIRT_BASE + KERNEL_PHYS_LOAD) ALIGN(4096)
	{
		__kernel_rodata_start = .;
		*(.rodata .rodata.* .srodata .srodata.*)
		__kernel_rodata_end = .;
	}

	.data : AT(ADDR(.data) - KERNEL_VIRT_BASE + KERNEL_PHYS_LOAD) ALIGN(4096)
	{
		__kernel_data_start = .;
		*(.data .data.* .sdata .sdata.*)
	}

	.bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VIRT_BASE + KERNEL_PHYS_LOAD) ALIGN(4096)
	{
		__bss_start = .;
		*(.bss.boot)
		*(.bss .bss.* .sbss .sbss.* COMMON)
		. = ALIGN(8);
		__bss_end = .;
	}

	. = ALIGN(4096);
	__kernel_vma_end = .;

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

ASSERT(_start == KERNEL_VIRT_BASE, "_start must be the image's first and lowest-loaded byte")
ASSERT(__kernel_vma_end - __kernel_vma_start <= 0x1000000, "the boot table maps 16 MiB of image")

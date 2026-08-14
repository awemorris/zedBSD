/* zedBSD amd64/PC-AT kernel link layout. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(amd64_start)

PHDRS {
	text PT_LOAD FLAGS(5);
	rodata PT_LOAD FLAGS(4);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = 0xffffffff80200000;
	__kernel_virt_start = .;
	__kernel_phys_start = 0x00200000;

	.text : AT(__kernel_phys_start) {
		__kernel_text_virt_start = .;
		KEEP(*(.text.entry))
		*(.text*)
		. = ALIGN(4096);
		__kernel_text_virt_end = .;
	} :text
	__kernel_text_phys_start = LOADADDR(.text);
	__kernel_text_phys_end = LOADADDR(.text) + SIZEOF(.text);

	.rodata : AT(__kernel_phys_start +
		(ADDR(.rodata) - __kernel_virt_start)) {
		__kernel_rodata_virt_start = .;
		*(.rodata*)
		. = ALIGN(4096);
		__kernel_rodata_virt_end = .;
	} :rodata
	__kernel_rodata_phys_start = LOADADDR(.rodata);
	__kernel_rodata_phys_end = LOADADDR(.rodata) + SIZEOF(.rodata);

	.data : AT(__kernel_phys_start +
		(ADDR(.data) - __kernel_virt_start)) {
		__kernel_data_virt_start = .;
		*(.data*)
	} :data
	__kernel_image_end = .;
	.bss (NOLOAD) : AT(__kernel_phys_start +
		(ADDR(.bss) - __kernel_virt_start)) {
		. = ALIGN(16);
		*(.vfs_bss*) *(.kernel_heap*) *(.bss*) *(COMMON)
		. = ALIGN(4096);
		__kernel_data_virt_end = .;
	} :data
	__kernel_data_phys_start = LOADADDR(.data);
	__kernel_data_phys_end = LOADADDR(.bss) + SIZEOF(.bss);
	__kernel_virt_end = .;
	__kernel_phys_end = __kernel_phys_start +
		(__kernel_virt_end - __kernel_virt_start);
	__low_start = __kernel_virt_start;
	__low_end = __kernel_virt_end;
	__high_start = __kernel_virt_end;
	__high_end = __kernel_virt_end;

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }

	ASSERT(SIZEOF_HEADERS <= 512,
	       "ELF/program headers exceed the Stage 2 first-sector window")
	ASSERT((LOADADDR(.text) & 0x1ff) == 0,
	       "ELF64 load address must be sector aligned")
	ASSERT(__kernel_phys_end < 0x40000000,
	       "amd64 bootstrap kernel exceeds the initial one-GiB map")
	ASSERT(__kernel_phys_end <= 0x01200000,
	       "amd64 kernel exceeds the static W^X page-table window")
}

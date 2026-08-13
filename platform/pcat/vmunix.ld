/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
OUTPUT_FORMAT(elf32-i386)
ENTRY(text_start_phys)
PHDRS { kernel PT_LOAD FLAGS(7); }
SECTIONS
{
	. = 0x80200000;
	__low_start = .;
	.multiboot : AT(ADDR(.multiboot) - 0x80000000) {
		KEEP(*(.multiboot))
	} :kernel
	.text : AT(ADDR(.text) - 0x80000000) {
		KEEP(*/hal/i386/locore.o(.text*))
		*(.text.entry) *(.text*)
	} :kernel
	.rodata : AT(ADDR(.rodata) - 0x80000000) { *(.rodata*) } :kernel
	.data : AT(ADDR(.data) - 0x80000000) { *(.data*) } :kernel
	__low_image_end = .;
	__low_bss_start = .;
	.bss (NOLOAD) : AT(ADDR(.bss) - 0x80000000) {
		. = ALIGN(4096);
		*(.vfs_bss) *(.bss*) *(COMMON)
		. = ALIGN(4096);
		KEEP(*(.kernel_heap))
	} :kernel
	__low_bss_end = .;
	__low_end = .;
	__high_start = .;
	__high_image_end = .;
	__high_bss_start = .;
	__high_bss_end = .;
	__high_end = .;
	text_start_phys = ABSOLUTE(text_start - 0x80000000);
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
	ASSERT((ADDR(.multiboot) - 0x80200000) < 8192,
	       "Multiboot header is outside the first 8 KiB")
	ASSERT(__low_end <= 0x88000000, "PC/AT kernel exceeds direct map")
}

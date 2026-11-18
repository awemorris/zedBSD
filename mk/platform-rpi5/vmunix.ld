OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

KERNEL_DIRECT_BASE = 0xffff000000000000;
KERNEL_PHYS_LOAD = 0x00080000;

SECTIONS
{
	. = KERNEL_DIRECT_BASE + KERNEL_PHYS_LOAD;
	__kernel_vma_start = .;
	__kernel_phys_start = KERNEL_PHYS_LOAD;

	.text : AT(KERNEL_PHYS_LOAD) ALIGN(4096)
	{
		__kernel_text_start = .;
		KEEP(*(.text.boot))
		*(.hightext)
		*(.text .text.*)
		__kernel_text_end = .;
	}
	__kernel_text_phys_start = LOADADDR(.text);
	__kernel_text_phys_end = LOADADDR(.text) + SIZEOF(.text);
	.rodata : AT(ADDR(.rodata) - KERNEL_DIRECT_BASE) ALIGN(4096)
	{
		__kernel_rodata_start = .;
		*(.rodata .rodata.*)
		__kernel_rodata_end = .;
	}
	__kernel_rodata_phys_start = LOADADDR(.rodata);
	__kernel_rodata_phys_end = LOADADDR(.rodata) + SIZEOF(.rodata);
	.data : AT(ADDR(.data) - KERNEL_DIRECT_BASE) ALIGN(4096)
	{
		__kernel_data_start = .;
		*(.data .data.*)
		__kernel_data_loaded_end = .;
	}
	__arm64_image_end = .;

	.bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_DIRECT_BASE) ALIGN(4096)
	{
		__bss_start = .;
		*(.bss.boot)
		*(.bss .bss.* COMMON)
		__bss_end = .;
		__kernel_data_end = .;
	}

	__kernel_vma_end = .;
	__kernel_phys_end = __kernel_vma_end - KERNEL_DIRECT_BASE;
	__kernel_data_phys_start = LOADADDR(.data);
	__kernel_data_phys_end = __kernel_phys_end;

	/*
	 * The Image header's sizes.  The image size is the memory the image
	 * takes once it runs, the BSS included, so a bootloader keeps the
	 * device tree and the initrd out of it.  The file size covers what
	 * the bootloader reads from the file: every loaded section, the
	 * orphans the linker puts after .data among them, comes before the
	 * BSS, and the raw file ends at most a page short of it.
	 */
	__arm64_image_size = ALIGN(__kernel_vma_end, 4096) - __kernel_vma_start;
	__arm64_image_file_size = __bss_start - __kernel_vma_start;

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

ASSERT((_start & 63) == 0, "arm64 Image header must be 64-byte aligned")
ASSERT(_start == __kernel_vma_start, "the Image header must start the image")
ASSERT(__kernel_phys_end < 0x40000000, "bootstrap map covers only the first GiB")

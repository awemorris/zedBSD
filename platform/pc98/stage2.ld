/* PC-98 single-image higher-half kernel. */
OUTPUT_FORMAT(elf32-i386)
ENTRY(text_start)

PHDRS
{
	kernel PT_LOAD FLAGS(7);
}

SECTIONS
{
	. = 0x80100000;
	__kernel_vma_start = .;
	__kernel_phys_start = __kernel_vma_start - 0x80000000;

	.header : AT(ADDR(.header) - 0x80000000) {
		KEEP(*(.header))
	} :kernel
	.text : AT(ADDR(.text) - 0x80000000) {
		*(.text.entry)
		*(.hightext)
		*(.text*)
	} :kernel
	.rodata : AT(ADDR(.rodata) - 0x80000000) {
		*(.rodata*)
	} :kernel
	.data : AT(ADDR(.data) - 0x80000000) {
		*(.data*)
	} :kernel
	__kernel_image_end = .;

	. = ALIGN(4096);
	__kernel_bss_start = .;
	.bss (NOLOAD) : AT(ADDR(.bss) - 0x80000000) SUBALIGN(16) {
		/* CR3 and every page table must remain page-aligned without
		 * raising the PT_LOAD file alignment above the loader's 512-byte
		 * contract.  Place locore first at an explicit page boundary. */
		. = ALIGN(4096);
		*/hal/i386/locore.o(.bss* COMMON)
		*(.vfs_bss)
		*(.net_bss)
		*(EXCLUDE_FILE (*/hal/i386/locore.o) .bss*)
		*(COMMON)
	} :kernel
	.kernel_heap (NOLOAD) : AT(ADDR(.kernel_heap) - 0x80000000) SUBALIGN(16) {
		__kernel_heap_start = .;
		KEEP(*(.kernel_heap))
		__kernel_heap_end = .;
	} :kernel
	__kernel_bss_end = .;
	__kernel_vma_end = .;
	__kernel_phys_end = __kernel_vma_end - 0x80000000;

	ASSERT(SIZEOF_HEADERS <= 512, "ELF headers exceed first sector")
	ASSERT(__kernel_phys_start == 0x00100000,
	       "PC-98 kernel must start at physical 1 MiB")
	ASSERT(__kernel_phys_end < 0x00f00000,
	       "PC-98 kernel reaches the 15 MiB memory hole")
	ASSERT(text_start >= __kernel_vma_start &&
	       text_start < __kernel_image_end,
	       "PC-98 entry is not file-backed")

	PROVIDE(noct_jisx0208_to_ucs = hal_pc98_jisx0208_to_ucs);
}

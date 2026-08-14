OUTPUT_FORMAT("elf64-sparc")
OUTPUT_ARCH(sparc:v9)
ENTRY(_start)

KERNEL_DIRECT_BASE = 0xfffff80000000000;
KERNEL_PHYS_LOAD = 0x00400000;
KERNEL_WINDOW_END = 0x00800000;
SPARCV9_PAGE_SIZE = 8192;
SPARCV9_TRAP_ALIGN = 32768;
SPARCV9_TRAP_SIZE = 32768;

PHDRS
{
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = KERNEL_DIRECT_BASE + KERNEL_PHYS_LOAD;
	__kernel_vma_start = .;
	__kernel_phys_start = KERNEL_PHYS_LOAD;

	.text : AT(KERNEL_PHYS_LOAD) ALIGN(SPARCV9_PAGE_SIZE)
	{
		__kernel_text_start = .;
		KEEP(*(.text.boot))
		*(.text .text.*)
		__kernel_text_end = .;
	} :text

	.trap_table : AT(ADDR(.trap_table) - KERNEL_DIRECT_BASE)
		ALIGN(SPARCV9_TRAP_ALIGN)
	{
		__trap_table_start = .;
		KEEP(*(.trap_table))
		__trap_table_end = .;
	} :text

	.rodata : AT(ADDR(.rodata) - KERNEL_DIRECT_BASE)
		ALIGN(SPARCV9_PAGE_SIZE)
	{
		__kernel_rodata_start = .;
		*(.rodata .rodata.*)
		__kernel_rodata_end = .;
	} :text

	.data : AT(ADDR(.data) - KERNEL_DIRECT_BASE)
		ALIGN(SPARCV9_PAGE_SIZE)
	{
		__kernel_data_start = .;
		*(.data .data.*)
		__kernel_data_loaded_end = .;
	} :data

	.bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_DIRECT_BASE)
		ALIGN(SPARCV9_PAGE_SIZE)
	{
		__bss_start = .;
		*(.bss.boot)
		*(.bss .bss.* COMMON)
		__bss_end = .;
	} :data

	.kernel_heap (NOLOAD) : AT(ADDR(.kernel_heap) - KERNEL_DIRECT_BASE)
		ALIGN(SPARCV9_PAGE_SIZE)
	{
		*(.kernel_heap)
	} :data

	. = ALIGN(SPARCV9_PAGE_SIZE);
	__kernel_vma_end = .;
	__kernel_phys_end = __kernel_vma_end - KERNEL_DIRECT_BASE;
	__kernel_data_end = __kernel_vma_end;
	__kernel_text_phys_start = LOADADDR(.text);
	__kernel_text_phys_end = LOADADDR(.text) + SIZEOF(.text);
	__kernel_rodata_phys_start = LOADADDR(.rodata);
	__kernel_rodata_phys_end = LOADADDR(.rodata) + SIZEOF(.rodata);
	__kernel_data_phys_start = LOADADDR(.data);
	__kernel_data_phys_end = __kernel_phys_end;
	__sparcv9_handoff_phys = KERNEL_WINDOW_END - SPARCV9_PAGE_SIZE;
	__sparcv9_handoff_vma = KERNEL_DIRECT_BASE + __sparcv9_handoff_phys;

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

ASSERT((__kernel_vma_start & (SPARCV9_PAGE_SIZE - 1)) == 0,
       "kernel start is not 8 KiB aligned")
ASSERT((__trap_table_start & (SPARCV9_TRAP_ALIGN - 1)) == 0,
       "SPARC V9 trap table is not 32 KiB aligned")
ASSERT((__trap_table_end - __trap_table_start) == SPARCV9_TRAP_SIZE,
       "SPARC V9 trap table must contain two correctly spaced banks")
ASSERT(__kernel_phys_end <= __sparcv9_handoff_phys,
       "kernel bootstrap image overlaps the handoff page")

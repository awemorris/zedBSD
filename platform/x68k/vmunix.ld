OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)

KERNEL_DIRECT_BASE = 0x80000000;
BOOTSTRAP_PHYS = 0x00010000;
KERNEL_PHYS_LOAD = 0x00100000;
X68K_PAGE_SIZE = 4096;
X68K_INITIAL_PHYS_END = 0x00c00000;

PHDRS
{
	bootstrap PT_LOAD FLAGS(5);
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = BOOTSTRAP_PHYS;
	__bootstrap_phys_start = .;
	.bootstrap : ALIGN(4)
	{
		KEEP(*(.bootstrap))
		KEEP(*(.bootstrap.*))
	} :bootstrap
	. = ALIGN(X68K_PAGE_SIZE);
	__bootstrap_phys_end = .;

	. = KERNEL_DIRECT_BASE + KERNEL_PHYS_LOAD;
	__kernel_vma_start = .;
	__kernel_phys_start = KERNEL_PHYS_LOAD;

	.text : AT(ADDR(.text) - KERNEL_DIRECT_BASE)
		ALIGN(X68K_PAGE_SIZE)
	{
		__kernel_text_start = .;
		. = ALIGN(1024);
		KEEP(*(.vectors))
		*(.text.boot)
		*(.text .text.*)
		__kernel_text_end = .;
	} :text

	.rodata : AT(ADDR(.rodata) - KERNEL_DIRECT_BASE)
		ALIGN(X68K_PAGE_SIZE)
	{
		*(.rodata .rodata.*)
	} :text

	.data : AT(ADDR(.data) - KERNEL_DIRECT_BASE)
		ALIGN(X68K_PAGE_SIZE)
	{
		*(.data .data.*)
	} :data

	.bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_DIRECT_BASE)
		ALIGN(X68K_PAGE_SIZE)
	{
		__bss_start = .;
		*(.bss .bss.* COMMON)
		__bss_end = .;
	} :data

	.kernel_heap (NOLOAD) : AT(ADDR(.kernel_heap) - KERNEL_DIRECT_BASE)
		ALIGN(X68K_PAGE_SIZE)
	{
		*(.kernel_heap)
	} :data

	. = ALIGN(X68K_PAGE_SIZE);
	__kernel_vma_end = .;
	__kernel_phys_end = __kernel_vma_end - KERNEL_DIRECT_BASE;

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu.attributes) }
}

ASSERT((__bootstrap_phys_start & (X68K_PAGE_SIZE - 1)) == 0,
       "bootstrap is not page aligned")
ASSERT(__bootstrap_phys_end <= KERNEL_PHYS_LOAD,
       "bootstrap overlaps the high kernel physical load")
ASSERT((__kernel_vma_start - __kernel_phys_start) == KERNEL_DIRECT_BASE,
       "kernel VMA/LMA does not match the direct map")
ASSERT(__kernel_phys_end <= X68K_INITIAL_PHYS_END,
       "kernel exceeds the initial 12 MiB physical window")
ASSERT((x68k_machine_reset - KERNEL_DIRECT_BASE) < 0x00400000,
       "reboot trampoline exceeds its temporary identity map")

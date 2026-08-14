OUTPUT_FORMAT("elf64-sparc")
OUTPUT_ARCH(sparc:v9)
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(7);
}

SECTIONS
{
	. = 0x4000;
	.text :
	{
		KEEP(*(.text.entry))
		*(.text .text.*)
		*(.rodata .rodata.*)
	} :text
	.data : { *(.data .data.*) } :text
	.bss (NOLOAD) :
	{
		__bss_start = .;
		*(.bss .bss.* COMMON)
		__bss_end = .;
	} :text
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

ASSERT(SIZEOF(.text) + SIZEOF(.data) <= 7680,
       "SPARC V9 stage1 exceeds the OpenBoot 15-sector limit")
ASSERT(__bss_end < 0xc000, "SPARC V9 stage1 overlaps its stack")

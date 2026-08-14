OUTPUT_FORMAT("elf64-sparc")
OUTPUT_ARCH(sparc:v9)
ENTRY(_start)

SECTIONS
{
	. = 0x00100000;
	.text :
	{
		KEEP(*(.text.entry))
		*(.text .text.*)
		*(.rodata .rodata.*)
	}
	.data : { *(.data .data.*) }
	.bss (NOLOAD) :
	{
		__bss_start = .;
		*(.bss.stack)
		*(.bss .bss.* COMMON)
		__bss_end = .;
	}
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

ASSERT(__bss_end <= 0x00300000,
       "SPARC V9 stage2 exceeds its claimed OpenBoot memory")

OUTPUT_FORMAT("elf64-sparc")
OUTPUT_ARCH(sparc:v9)
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
	stack PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
	. = 0x00400000;
	.text : ALIGN(8192)
	{
		*(.text .text.*)
		*(.rodata .rodata.*)
	} :text
	. = ALIGN(8192);
	.data :
	{
		*(.data .data.*)
	} :data
	.bss :
	{
		*(.bss .bss.*)
		*(COMMON)
	} :data
	.stack 0 (NOLOAD) :
	{
		. += 0x100000;
	} :stack
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

ASSERT(. < 0x80000000, "SPARC V9 user image exceeds the initial 2 GiB ABI")


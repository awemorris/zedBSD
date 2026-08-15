OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
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
	.text : ALIGN(4096) { *(.text .text.*) *(.rodata .rodata.*) } :text
	. = ALIGN(4096);
	.data : { *(.data .data.*) } :data
	.bss : { *(.bss .bss.*) *(COMMON) } :data
	.stack 0 (NOLOAD) : { . += 0x100000; } :stack
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu.attributes) }
}

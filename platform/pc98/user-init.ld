OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
PHDRS {
	text PT_LOAD FLAGS(5);
	stack PT_GNU_STACK FLAGS(6);
}
SECTIONS
{
	. = 0x00400000;
	.text : { *(.text*) *(.rodata*) } :text
	.stack 0 (NOLOAD) : { . += 0x100000; } :stack
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

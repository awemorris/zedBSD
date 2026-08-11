OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); }
SECTIONS
{
	. = 0x00400000;
	.text : { *(.text*) *(.rodata*) } :text
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

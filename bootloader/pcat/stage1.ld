OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
SECTIONS
{
	. = 0;
	.text : { *(.text) }
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
}

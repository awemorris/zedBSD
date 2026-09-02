OUTPUT_FORMAT(elf32-i386)
ENTRY(stage2_entry)
SECTIONS
{
	. = 0;
	.text : { *(.text) *(.rodata*) *(.data*) *(.bss*) }
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
}

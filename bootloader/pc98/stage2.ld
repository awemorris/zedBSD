OUTPUT_FORMAT(elf32-i386)
ENTRY(stage2_entry)
stage2_c_stack_top = 0x9000;
stage2_c_stack_reserve = 0x1000;
SECTIONS {
	. = 0;
	.text : {
		*(.text)
		*(.rodata*)
		*(.data*)
		*(.bss*)
		stage2_end = .;
	}
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
}
ASSERT(stage2_end <= stage2_c_stack_top - stage2_c_stack_reserve,
       "PC-98 BOOTZBSD stage 2 overlaps the 16-bit C stack reserve")

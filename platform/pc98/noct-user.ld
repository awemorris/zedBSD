ENTRY(_start)
PHDRS {
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}
SECTIONS {
	. = 0x00400000;
	.text : ALIGN(4096) { *(.text .text.*) *(.rodata .rodata.*) } :text
	. = ALIGN(4096);
	.data : { *(.data .data.*) } :data
	.bss : { *(.bss .bss.*) *(COMMON) } :data
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

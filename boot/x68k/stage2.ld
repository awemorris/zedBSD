OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)
SECTIONS
{
	. = 0x00020000;
	__stage2_start = .;
	.text : { *(.text .text.*) *(.rodata .rodata.*) }
	. = ALIGN(4);
	.data : { *(.data .data.*) }
	.bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
	__stage2_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu.attributes) }
}
ASSERT(__stage2_end <= 0x00040000, "X68k stage 2 exceeds reserved RAM")

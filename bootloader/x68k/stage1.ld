OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)
SECTIONS
{
	. = 0x00002400;
	.text : { *(.text .text.*) *(.rodata .rodata.*) }
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu.attributes) }
}
ASSERT(SIZEOF(.text) <= 1024, "X68k primary boot exceeds 1024 bytes")

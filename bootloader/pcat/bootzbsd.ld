OUTPUT_FORMAT(elf32-i386)
ENTRY(stage2_entry)
SECTIONS
{
	. = 0;
	.text :
	{
		*(.text)
		*(.rodata*)
		*(.data*)
		*(.bss*)
		stage2_end = .;
	}
	ASSERT(stage2_end < 0xd000,
	       "BOOTZBSD image overlaps its protected 8 KiB C stack")
	/DISCARD/ : { *(*) }
}

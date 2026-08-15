OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
PHDRS {
  load PT_LOAD FLAGS(7);
}
SECTIONS
{
  . = 0x00100000;
  .text : AT(0x00100000) { *(.text) *(.rodata*) } :load
  .bss : { *(.bss*) } :load
  /DISCARD/ : { *(*) }
}


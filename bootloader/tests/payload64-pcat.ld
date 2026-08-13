OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
PHDRS { load PT_LOAD FLAGS(7); }
SECTIONS {
  . = 0xffffffff80200000;
  .text : AT(0x00200000) { *(.text) *(.rodata*) } :load
  .bss : { *(.bss*) } :load
  /DISCARD/ : { *(*) }
}


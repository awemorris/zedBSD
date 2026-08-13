OUTPUT_FORMAT(elf64-x86-64)
ENTRY(stage2_entry)
SECTIONS { . = 0; .text : { *(.text) *(.rodata*) *(.data*) *(.bss*) } /DISCARD/ : { *(*) } }

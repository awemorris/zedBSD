/* Unified Stage 0 link layout. */
OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
SECTIONS { . = 0; .text : { *(.text) } /DISCARD/ : { *(*) } }


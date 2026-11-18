/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Links the PC/AT ELF32 test kernel at its 2 MiB physical address.
 */

OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
PHDRS { load PT_LOAD FLAGS(7); }
SECTIONS {
  . = 0x00200000;
  .text : AT(0x00200000) { *(.text) *(.rodata*) } :load
  .bss : { *(.bss*) } :load
  /DISCARD/ : { *(*) }
}


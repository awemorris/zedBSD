/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Links the PC-98 ELF32 test kernel at its 1 MiB physical address.
 */

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


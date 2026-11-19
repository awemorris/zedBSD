#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""Finish the 68040 Macintosh second stage: its image and its relocations.

The second stage is linked at 0 with its relocations kept (ld
--emit-relocs) and runs wherever the ROM's heap puts it.  This tool
writes the loadable bytes from address 0, then, on a 4-byte boundary,
the big-endian offsets of every 32-bit absolute address in them, and
fills the stage's header (bootloader/mac68k/boot-layout.h): the offset
and the count of that list, and the memory the stage needs (its image,
its BSS and the list, which the BSS may cover).  The entry adds its own
address at each offset before it clears the BSS.

PC-relative relocations need nothing; an absolute one against an
absolute symbol stays as it is; any other kind is refused, since the
entry cannot fix it.

  finalize-mac68k-stage2.py --input zedboot.elf --output zedboot

Values referenced: the System V ABI (the ELF32 header, program headers,
section headers, the symbol table and RELA entries); binutils
include/elf/m68k.h (R_68K_NONE 0, R_68K_32 1, R_68K_PC32 4, R_68K_PC16 5,
R_68K_PC8 6).
"""

import argparse
import struct
import sys
from pathlib import Path

PT_LOAD = 1
SHT_SYMTAB = 2
SHT_RELA = 4
SHF_ALLOC = 0x2
SHN_UNDEF = 0
SHN_ABS = 0xFFF1

R_68K_NONE = 0
R_68K_32 = 1
R_68K_PC = {4, 5, 6}

# The header's fields (boot-layout.h).
MAGIC = 0x5A425332
MAGIC_OFFSET = 4
RELOC_OFFSET_OFFSET = 8
RELOC_COUNT_OFFSET = 12
MEMORY_OFFSET = 16
HEADER_SIZE = 20
LIMIT = 0x40000


def fail(message):
    """Stops with a message."""
    raise SystemExit(f"finalize-mac68k-stage2: {message}")


def sections(data):
    """The section headers: (type, flags, address, offset, size, link, info, entry size)."""
    shoff, = struct.unpack_from(">I", data, 32)
    shentsize, shnum = struct.unpack_from(">HH", data, 46)
    found = []
    for index in range(shnum):
        at = shoff + index * shentsize
        _, kind, flags, address, offset, size, link, info, _, entsize = struct.unpack_from(">IIIIIIIIII", data, at)
        found.append((kind, flags, address, offset, size, link, info, entsize))
    return found


def image(data):
    """The loadable bytes from address 0, and the end of memory (with the BSS)."""
    phoff, = struct.unpack_from(">I", data, 28)
    phentsize, phnum = struct.unpack_from(">HH", data, 42)
    body = bytearray()
    memory_end = 0
    for index in range(phnum):
        kind, offset, vaddr, _, filesz, memsz, _, _ = struct.unpack_from(">IIIIIIII", data, phoff + index * phentsize)
        if kind != PT_LOAD:
            continue
        if vaddr + filesz > len(body):
            body.extend(b"\0" * (vaddr + filesz - len(body)))
        body[vaddr:vaddr + filesz] = data[offset:offset + filesz]
        memory_end = max(memory_end, vaddr + memsz)
    return body, memory_end


def relocations(data, table):
    """The offsets of the 32-bit absolute addresses in the loaded sections."""
    symtab = [s for s in table if s[0] == SHT_SYMTAB]
    if len(symtab) != 1:
        fail("the stage has no single symbol table")
    _, _, _, sym_offset, _, _, _, sym_entsize = symtab[0]
    offsets = []
    for kind, _, _, offset, size, _, info, entsize in table:
        if kind != SHT_RELA:
            continue
        if not table[info][1] & SHF_ALLOC:
            continue
        for at in range(offset, offset + size, entsize):
            r_offset, r_info, _ = struct.unpack_from(">IIi", data, at)
            r_type = r_info & 0xFF
            r_sym = r_info >> 8
            if r_type == R_68K_NONE or r_type in R_68K_PC:
                continue
            if r_type != R_68K_32:
                fail(f"relocation type {r_type} at {r_offset:#x} cannot be fixed at run time")
            shndx, = struct.unpack_from(">H", data, sym_offset + r_sym * sym_entsize + 14)
            if shndx == SHN_ABS:
                continue
            if shndx == SHN_UNDEF:
                fail(f"relocation at {r_offset:#x} names an undefined symbol")
            offsets.append(r_offset)
    return sorted(set(offsets))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    data = Path(args.input).read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 2:
        fail(f"{args.input}: not a big-endian ELF32 file")
    if struct.unpack_from(">H", data, 18)[0] != 4:
        fail(f"{args.input}: not an m68k file")

    body, memory_end = image(data)
    offsets = relocations(data, sections(data))
    if len(body) < HEADER_SIZE or struct.unpack_from(">I", body, MAGIC_OFFSET)[0] != MAGIC:
        fail("the stage does not start with its header")
    for offset in offsets:
        if offset + 4 > len(body):
            fail(f"relocation at {offset:#x} lies outside the image")

    # The list after the image, and the header's fields.
    list_offset = (len(body) + 3) & ~3
    body.extend(b"\0" * (list_offset - len(body)))
    for offset in offsets:
        body += struct.pack(">I", offset)
    memory = max(len(body), memory_end)
    memory = (memory + 3) & ~3
    if memory > LIMIT:
        fail(f"the stage needs {memory} bytes, more than {LIMIT}")
    struct.pack_into(">III", body, RELOC_OFFSET_OFFSET, list_offset, len(offsets), memory)

    output = Path(args.output)
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_bytes(bytes(body))
    temporary.replace(output)
    print(f"{output}: {len(body)} bytes, {len(offsets)} relocations, {memory} bytes of memory")


if __name__ == "__main__":
    main()

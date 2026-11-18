#!/usr/bin/env python3
"""Turn the linked riscv64 loader into BOOTRISCV64.EFI.

  make-bootriscv64.py --elf LOADER.elf --binary LOADER.bin OUTPUT

LOADER.elf is the loader linked for address 0 by bootloader/uefi/riscv64/
loader.ld, LOADER.bin its flat image from address 0x1000 (llvm-objcopy -O
binary).  The tool writes a PE32+ header into the first page, so that a
section's relative virtual address is also its place in the file, and
appends the flat image after it, padded to a whole page.

The header names two sections: .text (the code and the read-only data) and
.data (the data, the relocations and, past the file's bytes, the
zero-filled data).  It carries no base relocations: the image applies its
own relocations at its entry.
"""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import os
import struct
from pathlib import Path

PAGE = 0x1000
HEADER_BYTES = PAGE

# The PE32+ header's fields and values.
#
# Values referenced: Microsoft PE/COFF specification (the MS-DOS stub's
# e_lfanew, the COFF file header, the PE32+ optional header and its data
# directories, the section table, IMAGE_SUBSYSTEM_EFI_APPLICATION, the
# section characteristics); Linux 6.8.12 include/linux/pe.h
# (IMAGE_FILE_MACHINE_RISCV64 0x5064).
MACHINE_RISCV64 = 0x5064
OPTIONAL_MAGIC_PE32PLUS = 0x20B
SUBSYSTEM_EFI_APPLICATION = 10
# Executable image; line numbers, local symbols and debugging data stripped;
# large addresses allowed.
FILE_CHARACTERISTICS = 0x0002 | 0x0004 | 0x0008 | 0x0020 | 0x0200
SECTION_CODE = 0x00000020
SECTION_INITIALIZED_DATA = 0x00000040
SECTION_EXECUTE = 0x20000000
SECTION_READ = 0x40000000
SECTION_WRITE = 0x80000000
DATA_DIRECTORY_COUNT = 16
OPTIONAL_HEADER_BYTES = 24 + 88 + DATA_DIRECTORY_COUNT * 8
PE_OFFSET = 0x40

SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_NOBITS = 8
SHF_ALLOC = 0x2


def page_up(value: int) -> int:
    return (value + PAGE - 1) // PAGE * PAGE


def read_elf(path: Path) -> tuple[dict[str, tuple[int, int, int, int]], dict[str, int]]:
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise SystemExit(f"{path}: not a little-endian ELF64 file")
    machine = struct.unpack_from("<H", data, 18)[0]
    if machine != 243:
        raise SystemExit(f"{path}: not a RISC-V ELF file")
    shoff = struct.unpack_from("<Q", data, 40)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 58)
    headers = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + index * shentsize)
               for index in range(shnum)]
    names_offset = headers[shstrndx][4]

    def name_at(table_offset: int, offset: int) -> str:
        end = data.index(b"\0", table_offset + offset)
        return data[table_offset + offset:end].decode("ascii")

    sections = {}
    symbols = {}
    for header in headers:
        name = name_at(names_offset, header[0])
        sections[name] = (header[1], header[2], header[3], header[5])
        if header[1] == SHT_SYMTAB:
            strings = headers[header[6]][4]
            for offset in range(header[4], header[4] + header[5], 24):
                st_name, _, _, _, st_value, _ = struct.unpack_from("<IBBHQQ", data, offset)
                if st_name:
                    symbols[name_at(strings, st_name)] = st_value
    return sections, symbols


def section_header(name: bytes, virtual_size: int, address: int, raw_size: int,
                   raw_offset: int, characteristics: int) -> bytes:
    return struct.pack("<8sIIIIIIHHI", name, virtual_size, address, raw_size,
                       raw_offset, 0, 0, 0, 0, characteristics)


def build(args: argparse.Namespace) -> None:
    sections, symbols = read_elf(args.elf)
    for required in (".text", ".data"):
        if required not in sections:
            raise SystemExit(f"{args.elf}: no {required} section")
    if "_start" not in symbols:
        raise SystemExit(f"{args.elf}: no _start")
    text_start = sections[".text"][2]
    data_start = sections[".data"][2]
    if text_start != HEADER_BYTES or data_start % PAGE:
        raise SystemExit(f"{args.elf}: unexpected layout")

    # The end of the file's bytes and of the image in memory, from the
    # allocated sections.
    file_end = 0
    memory_end = 0
    for kind, flags, address, size in sections.values():
        if not flags & SHF_ALLOC or size == 0:
            continue
        memory_end = max(memory_end, address + size)
        if kind != SHT_NOBITS:
            file_end = max(file_end, address + size)
    image = args.binary.read_bytes()
    if HEADER_BYTES + len(image) != file_end:
        raise SystemExit(f"{args.binary}: flat image does not end where the ELF's data does")
    text_size = data_start - text_start
    data_raw = page_up(file_end) - data_start
    data_virtual = page_up(memory_end) - data_start
    image_size = data_start + data_virtual
    entry = symbols["_start"]
    if not text_start <= entry < data_start:
        raise SystemExit(f"{args.elf}: entry outside the code")

    header = bytearray(HEADER_BYTES)
    header[0:2] = b"MZ"
    struct.pack_into("<I", header, 0x3C, PE_OFFSET)
    offset = PE_OFFSET
    header[offset:offset + 4] = b"PE\0\0"
    offset += 4
    struct.pack_into("<HHIIIHH", header, offset, MACHINE_RISCV64, 2, 0, 0, 0,
                     OPTIONAL_HEADER_BYTES, FILE_CHARACTERISTICS)
    offset += 20
    optional = offset
    struct.pack_into("<HBBIIIII", header, offset, OPTIONAL_MAGIC_PE32PLUS, 0, 0,
                     text_size, data_raw, data_virtual - data_raw, entry, text_start)
    offset += 24
    struct.pack_into("<QIIHHHHHHIIIIHHQQQQII", header, offset,
                     0, PAGE, PAGE, 0, 0, 0, 0, 0, 0, 0,
                     image_size, HEADER_BYTES, 0, SUBSYSTEM_EFI_APPLICATION, 0,
                     0, 0, 0, 0, 0, DATA_DIRECTORY_COUNT)
    offset += 88
    # Every data directory is empty, the base relocations' included.
    offset += DATA_DIRECTORY_COUNT * 8
    if offset != optional + OPTIONAL_HEADER_BYTES:
        raise SystemExit("PE optional header size mismatch")
    header[offset:offset + 40] = section_header(
        b".text", text_size, text_start, text_size, text_start,
        SECTION_CODE | SECTION_EXECUTE | SECTION_READ)
    offset += 40
    header[offset:offset + 40] = section_header(
        b".data", data_virtual, data_start, data_raw, data_start,
        SECTION_INITIALIZED_DATA | SECTION_READ | SECTION_WRITE)

    body = image + bytes(page_up(file_end) - file_end)
    if args.output.exists() and not args.force:
        raise SystemExit(f"output exists (use --force): {args.output}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    partial = args.output.with_name(args.output.name + ".partial")
    partial.write_bytes(bytes(header) + body)
    os.replace(partial, args.output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)
    build(parser.parse_args())


if __name__ == "__main__":
    main()

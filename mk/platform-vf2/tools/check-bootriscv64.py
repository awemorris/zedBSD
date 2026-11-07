#!/usr/bin/env python3
"""Check BOOTRISCV64.EFI and the loader ELF it was made from.

  check-bootriscv64.py --elf LOADER.elf BOOTRISCV64.EFI

The PE image must be a RISC-V 64 EFI application whose two sections lie in
the file where their relative virtual addresses say, page aligned, with the
entry in the code and the zero-filled data inside .data's memory size.  The
ELF must hold only R_RISCV_RELATIVE dynamic relocations, which the image's
entry applies, and no undefined symbol.
"""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import struct
from pathlib import Path

# The header fields and values the image and the ELF are checked against.
#
# Values referenced: Microsoft PE/COFF specification (header layout,
# IMAGE_SUBSYSTEM_EFI_APPLICATION); Linux 6.8.12 include/linux/pe.h
# (IMAGE_FILE_MACHINE_RISCV64); riscv-non-isa/riscv-elf-psabi-doc
# riscv-elf.adoc (R_RISCV_RELATIVE).
PAGE = 0x1000
MACHINE_RISCV64 = 0x5064
OPTIONAL_MAGIC_PE32PLUS = 0x20B
SUBSYSTEM_EFI_APPLICATION = 10
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_DYNSYM = 11
R_RISCV_RELATIVE = 3


def fail(path: Path, message: str) -> None:
    raise SystemExit(f"{path}: {message}")


def check_pe(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < PAGE or len(data) % PAGE or data[:2] != b"MZ":
        fail(path, "not a page-sized MZ image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        fail(path, "no PE signature")
    machine, count, _, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
    if machine != MACHINE_RISCV64:
        fail(path, f"machine is 0x{machine:x}, not RISC-V 64")
    if count != 2:
        fail(path, "expected two sections")
    optional = pe + 24
    magic, _, _, _, _, _, entry, _ = struct.unpack_from("<HBBIIIII", data, optional)
    if magic != OPTIONAL_MAGIC_PE32PLUS:
        fail(path, "not PE32+")
    (_, section_alignment, file_alignment, _, _, _, _, _, _, _, image_size,
     headers_size, _, subsystem, _, _, _, _, _, _, directories) = struct.unpack_from(
        "<QIIHHHHHHIIIIHHQQQQII", data, optional + 24)
    if section_alignment != PAGE or file_alignment != PAGE or headers_size != PAGE:
        fail(path, "alignment or header size is not a page")
    if subsystem != SUBSYSTEM_EFI_APPLICATION:
        fail(path, "not an EFI application")
    if directories < 6:
        fail(path, "too few data directories")
    relocations = struct.unpack_from("<II", data, optional + 112 + 5 * 8)
    if relocations != (0, 0):
        fail(path, "the image carries base relocations")
    table = optional + optional_size
    sections = []
    for index in range(count):
        (name, virtual_size, address, raw_size, raw_offset, _, _, _, _,
         characteristics) = struct.unpack_from("<8sIIIIIIHHI", data, table + index * 40)
        sections.append((name.rstrip(b"\0"), virtual_size, address, raw_size, raw_offset,
                         characteristics))
    text, data_section = sections
    if text[0] != b".text" or data_section[0] != b".data":
        fail(path, "sections are not .text and .data")
    end = PAGE
    for name, virtual_size, address, raw_size, raw_offset, _ in sections:
        if address != raw_offset or address % PAGE or raw_size % PAGE:
            fail(path, f"{name.decode()} is not page aligned at its own offset")
        if address != end:
            fail(path, f"{name.decode()} does not follow the previous section")
        if virtual_size < raw_size:
            fail(path, f"{name.decode()} is shorter in memory than in the file")
        if raw_offset + raw_size > len(data):
            fail(path, f"{name.decode()} lies outside the file")
        end = address + raw_size
    if end != len(data):
        fail(path, "the file holds bytes past the sections")
    if (text[5] & 0x20000000) == 0 or (data_section[5] & 0x80000000) == 0:
        fail(path, "section permissions are wrong")
    if not text[2] <= entry < text[2] + text[1]:
        fail(path, "entry outside .text")
    if image_size != data_section[2] + data_section[1]:
        fail(path, "image size does not end at .data's end")


def check_elf(path: Path) -> None:
    data = path.read_bytes()
    shoff = struct.unpack_from("<Q", data, 40)[0]
    shentsize, shnum, _ = struct.unpack_from("<HHH", data, 58)
    headers = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + index * shentsize)
               for index in range(shnum)]
    for header in headers:
        kind, offset, size, entsize = header[1], header[4], header[5], header[9]
        if kind == SHT_RELA:
            for position in range(offset, offset + size, entsize):
                _, info, _ = struct.unpack_from("<QQq", data, position)
                if info & 0xFFFFFFFF != R_RISCV_RELATIVE or info >> 32:
                    fail(path, f"dynamic relocation of type {info & 0xFFFFFFFF}")
        if kind in (SHT_SYMTAB, SHT_DYNSYM):
            for position in range(offset + 24, offset + size, 24):
                st_name, st_info, _, st_shndx, _, _ = struct.unpack_from("<IBBHQQ", data, position)
                if st_shndx == 0 and st_name != 0 and (st_info >> 4) != 0:
                    fail(path, "undefined symbol in the loader")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    check_elf(args.elf)
    check_pe(args.image)
    print(f"{args.image}: RISC-V 64 EFI application OK")


if __name__ == "__main__":
    main()

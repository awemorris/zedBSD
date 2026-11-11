#!/usr/bin/env python3
"""Validate the freestanding PE32+ x64 UEFI application."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import struct
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit("BOOTX64.EFI check: " + message)


def check(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < 512 or len(data) > 1024 * 1024:
        fail("file size is outside the 512-byte..1-MiB policy")
    if data[:2] != b"MZ":
        fail("missing DOS MZ header")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe > len(data) - 24 or data[pe:pe + 4] != b"PE\0\0":
        fail("missing PE signature")
    machine, sections, _, _, _, optional_size, _ = struct.unpack_from(
        "<HHIIIHH", data, pe + 4)
    if machine != 0x8664 or sections == 0:
        fail("image is not x86-64 PE")
    optional = pe + 24
    if optional_size < 112 or optional + optional_size > len(data):
        fail("truncated PE32+ optional header")
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        fail("optional header is not PE32+")
    entry = struct.unpack_from("<I", data, optional + 16)[0]
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    size_of_image = struct.unpack_from("<I", data, optional + 56)[0]
    subsystem = struct.unpack_from("<H", data, optional + 68)[0]
    directory_count = struct.unpack_from("<I", data, optional + 108)[0]
    if subsystem != 10:
        fail("PE subsystem is not EFI_APPLICATION")
    if entry == 0 or entry >= size_of_image:
        fail("entry RVA is outside the image")
    import_rva = import_size = 0
    if directory_count > 1:
        import_rva, import_size = struct.unpack_from(
            "<II", data, optional + 112 + 8)
    relocations = False
    if directory_count > 5:
        reloc_rva, reloc_size = struct.unpack_from(
            "<II", data, optional + 112 + 5 * 8)
        relocations = reloc_rva != 0 and reloc_size != 0
    section_table = optional + optional_size
    if section_table + sections * 40 > len(data):
        fail("truncated section table")
    entry_executable = False
    names = []
    section_extents = []
    for index in range(sections):
        offset = section_table + index * 40
        name = data[offset:offset + 8].rstrip(b"\0").decode(
            "ascii", errors="replace")
        virtual_size, virtual_address, raw_size, raw_offset = \
            struct.unpack_from("<IIII", data, offset + 8)
        characteristics = struct.unpack_from("<I", data, offset + 36)[0]
        names.append(name)
        section_extents.append((virtual_address, raw_size, raw_offset))
        span = max(virtual_size, 1)
        if virtual_address <= entry < virtual_address + span and \
                characteristics & 0x20000000:
            entry_executable = True
    if not entry_executable:
        fail("entry RVA is not in an executable section")
    if import_rva != 0 or import_size != 0:
        import_data = None
        for virtual_address, raw_size, raw_offset in section_extents:
            if virtual_address <= import_rva and \
                    import_size <= raw_size - (import_rva - virtual_address):
                start = raw_offset + import_rva - virtual_address
                import_data = data[start:start + import_size]
                break
        if import_data is None or any(import_data):
            fail("UEFI loader has nonempty imports")
    if not relocations and image_base != 0:
        fail("nonzero-base image has no base relocation directory")
    print("BOOTX64.EFI check: PASS "
          f"({len(data)} bytes, sections={','.join(names)}, "
          f"relocations={'yes' if relocations else 'PIC/base-0'})")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    check(parser.parse_args().image)


if __name__ == "__main__":
    main()

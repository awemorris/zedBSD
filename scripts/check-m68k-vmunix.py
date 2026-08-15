#!/usr/bin/env python3
"""Validate the zedBSD MC68030 bootstrap/high-kernel ELF contract."""

import argparse
import struct
from pathlib import Path

ET_EXEC = 2
EM_68K = 4
PT_LOAD = 1
PF_X = 1
PF_W = 2
KERNEL_DIRECT_BASE = 0x80000000
BOOTSTRAP_PHYS = 0x00010000
KERNEL_PHYS_LOAD = 0x00100000
INITIAL_PHYS_END = 0x00C00000
PAGE_SIZE = 4096


def fail(path: Path, message: str) -> None:
    raise SystemExit(f"{path}: m68k kernel check: {message}")


def checked_range(path: Path, start: int, size: int, limit: int,
                  what: str) -> None:
    if start < 0 or size < 0 or start > limit or size > limit - start:
        fail(path, f"{what} is outside the file")


def sections(path: Path, data: bytes, header: tuple) -> list[tuple]:
    offset, size, count = header[6], header[11], header[12]
    if size != 40 or count == 0:
        fail(path, "invalid section table")
    checked_range(path, offset, size * count, len(data), "section table")
    return [struct.unpack_from(">IIIIIIIIII", data, offset + i * size)
            for i in range(count)]


def symbols(path: Path, data: bytes, all_sections: list[tuple]) -> dict[str, int]:
    result = {}
    for section in all_sections:
        section_type, offset, size, link, entry_size = (
            section[1], section[4], section[5], section[6], section[9]
        )
        if section_type in (4, 9) and size:
            fail(path, "final image contains relocation sections")
        if section_type != 2:
            continue
        if entry_size != 16 or link >= len(all_sections):
            fail(path, "invalid symbol table")
        strings = all_sections[link]
        checked_range(path, offset, size, len(data), "symbol table")
        checked_range(path, strings[4], strings[5], len(data),
                      "symbol string table")
        string_data = data[strings[4]:strings[4] + strings[5]]
        for pos in range(offset, offset + size, entry_size):
            name, value, _, _, _, _ = struct.unpack_from(">IIIBBH", data, pos)
            if name >= len(string_data):
                fail(path, "symbol name is outside the string table")
            end = string_data.find(b"\0", name)
            if end < 0:
                fail(path, "unterminated symbol name")
            symbol_name = string_data[name:end].decode("ascii", "strict")
            if symbol_name:
                result[symbol_name] = value
    return result


def check(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x02\x01":
        fail(path, "expected current big-endian ELF32")
    header = struct.unpack_from(">16sHHIIIIIHHHHHH", data)
    e_type, machine, entry = header[1], header[2], header[4]
    phoff, ehsize, phentsize, phnum = header[5], header[8], header[9], header[10]
    if e_type != ET_EXEC or machine != EM_68K or ehsize != 52:
        fail(path, "expected ELF32/MSB/EM_68K ET_EXEC")
    if phentsize != 32 or phnum < 2:
        fail(path, "invalid program-header table")
    checked_range(path, phoff, phentsize * phnum, len(data),
                  "program-header table")

    low_entry = False
    high_load = False
    highest_physical = 0
    for index in range(phnum):
        program = struct.unpack_from(">IIIIIIII", data,
                                     phoff + index * phentsize)
        p_type, offset, vaddr, paddr, filesz, memsz, flags, align = program
        if p_type != PT_LOAD:
            continue
        checked_range(path, offset, filesz, len(data),
                      f"PT_LOAD {index} file range")
        if filesz > memsz or align < PAGE_SIZE or align & (align - 1):
            fail(path, f"PT_LOAD {index} has invalid size/alignment")
        if flags & PF_X and flags & PF_W:
            fail(path, f"PT_LOAD {index} is writable and executable")
        if vaddr < KERNEL_DIRECT_BASE:
            if vaddr != paddr:
                fail(path, f"PT_LOAD {index} low VA/PA differ")
            if flags & PF_X and vaddr <= entry < vaddr + memsz:
                low_entry = True
        else:
            high_load = True
            if vaddr - paddr != KERNEL_DIRECT_BASE:
                fail(path, f"PT_LOAD {index} high VA/PA differ")
            if paddr < KERNEL_PHYS_LOAD:
                fail(path, f"PT_LOAD {index} precedes kernel load base")
        highest_physical = max(highest_physical, paddr + memsz)
    if not low_entry or not high_load:
        fail(path, "missing executable low entry or high kernel mapping")
    if highest_physical > INITIAL_PHYS_END:
        fail(path, "image exceeds the initial 12 MiB physical window")

    symbol_values = symbols(path, data, sections(path, data, header))
    expected = {
        "__bootstrap_phys_start": BOOTSTRAP_PHYS,
        "__kernel_phys_start": KERNEL_PHYS_LOAD,
        "__kernel_vma_start": KERNEL_DIRECT_BASE + KERNEL_PHYS_LOAD,
    }
    for name, value in expected.items():
        if symbol_values.get(name) != value:
            fail(path, f"{name} is {symbol_values.get(name)!r}, expected {value:#x}")
    for name in ("__bootstrap_phys_end", "__kernel_phys_end",
                 "__kernel_vma_end", "__bss_start", "__bss_end"):
        if name not in symbol_values:
            fail(path, f"missing required linker symbol {name}")

    print(f"{path}: m68k kernel check: PASS "
          f"(entry={entry:#x}, physical_end={highest_physical:#x})")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    check(args.elf)


if __name__ == "__main__":
    main()

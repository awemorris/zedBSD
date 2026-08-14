#!/usr/bin/env python3
"""Validate the zedBSD SPARC V9 kernel ELF and bootstrap window."""

import argparse
import struct
from pathlib import Path

ET_EXEC = 2
EM_SPARCV9 = 43
PT_LOAD = 1
PF_X = 1
KERNEL_DIRECT_BASE = 0xFFFFF80000000000
KERNEL_PHYS_LOAD = 0x00400000
KERNEL_WINDOW_END = 0x00800000
HANDOFF_SIZE = 8192
PAGE_SIZE = 8192
TRAP_ALIGN = 32768
TRAP_SIZE = 32768


def fail(path: Path, message: str) -> None:
    raise SystemExit(f"{path}: SPARC V9 kernel check: {message}")


def checked_range(path: Path, start: int, size: int, limit: int,
                  what: str) -> None:
    if start < 0 or size < 0 or start > limit or size > limit - start:
        fail(path, f"{what} is outside the file")


def section_names(path: Path, data: bytes, header: tuple) -> dict[str, tuple]:
    section_offset = header[6]
    section_size = header[11]
    section_count = header[12]
    string_index = header[13]
    if section_size != 64 or section_count == 0:
        fail(path, "invalid section table")
    checked_range(path, section_offset, section_size * section_count,
                  len(data), "section table")
    if string_index >= section_count:
        fail(path, "invalid section-name string table index")
    sections = [
        struct.unpack_from(">IIQQQQIIQQ", data,
                           section_offset + index * section_size)
        for index in range(section_count)
    ]
    strings = sections[string_index]
    checked_range(path, strings[4], strings[5], len(data),
                  "section-name string table")
    string_data = data[strings[4]:strings[4] + strings[5]]
    result = {}
    for section in sections:
        name_offset = section[0]
        if name_offset >= len(string_data):
            fail(path, "section name is outside the string table")
        end = string_data.find(b"\0", name_offset)
        if end < 0:
            fail(path, "unterminated section name")
        name = string_data[name_offset:end].decode("ascii", "strict")
        result[name] = section
    return result


def check(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        fail(path, "not an ELF file")
    if data[4:7] != b"\x02\x02\x01":
        fail(path, "expected current ELF64 big-endian")
    header = struct.unpack_from(">16sHHIQQQIHHHHHH", data, 0)
    e_type, e_machine, entry = header[1], header[2], header[4]
    phoff, ehsize, phentsize, phnum = (
        header[5], header[8], header[9], header[10]
    )
    if e_type != ET_EXEC or e_machine != EM_SPARCV9 or ehsize != 64:
        fail(path, "expected ELF64/MSB/EM_SPARCV9 ET_EXEC")
    if phentsize != 56 or phnum == 0:
        fail(path, "invalid program-header table")
    checked_range(path, phoff, phentsize * phnum, len(data),
                  "program-header table")
    if entry < KERNEL_DIRECT_BASE + KERNEL_PHYS_LOAD:
        fail(path, f"entry is outside the high kernel mapping: {entry:#x}")

    loads = []
    executable_entry = False
    for index in range(phnum):
        program = struct.unpack_from(
            ">IIQQQQQQ", data, phoff + index * phentsize
        )
        p_type, flags, offset, vaddr, paddr, filesz, memsz, align = program
        if p_type != PT_LOAD:
            continue
        loads.append(program)
        checked_range(path, offset, filesz, len(data),
                      f"PT_LOAD {index} file range")
        if filesz > memsz:
            fail(path, f"PT_LOAD {index} has filesz larger than memsz")
        if align < PAGE_SIZE or align & (align - 1):
            fail(path, f"PT_LOAD {index} has invalid alignment {align}")
        if vaddr < KERNEL_DIRECT_BASE or vaddr - KERNEL_DIRECT_BASE != paddr:
            fail(path, f"PT_LOAD {index} has inconsistent VA/PA")
        if paddr < KERNEL_PHYS_LOAD or memsz > KERNEL_WINDOW_END - paddr:
            fail(path, f"PT_LOAD {index} escapes the 4 MiB window")
        if flags & PF_X and vaddr <= entry < vaddr + memsz:
            executable_entry = True
    if not loads:
        fail(path, "no PT_LOAD segments")
    if not executable_entry:
        fail(path, "entry is not in an executable PT_LOAD")
    highest = max(program[4] + program[6] for program in loads)
    if highest > KERNEL_WINDOW_END - HANDOFF_SIZE:
        fail(path, "kernel overlaps the reserved handoff page")

    sections = section_names(path, data, header)
    if ".dynamic" in sections or ".interp" in sections:
        fail(path, "dynamic-linker sections are not permitted")
    trap = sections.get(".trap_table")
    if trap is None:
        fail(path, "missing .trap_table")
    trap_address, trap_size, trap_align = trap[3], trap[5], trap[8]
    if trap_address % TRAP_ALIGN or trap_align < TRAP_ALIGN:
        fail(path, ".trap_table is not 32 KiB aligned")
    if trap_size != TRAP_SIZE:
        fail(path, f".trap_table size is {trap_size}, expected {TRAP_SIZE}")

    print(
        f"{path}: SPARC V9 kernel check: PASS "
        f"(entry={entry:#x}, load_end={highest:#x})"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    check(args.elf)


if __name__ == "__main__":
    main()

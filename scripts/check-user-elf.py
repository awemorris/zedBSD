#!/usr/bin/env python3
"""Validate a zedBSD user ELF and its non-executable initial stack."""

import argparse
import struct
from pathlib import Path

PT_GNU_STACK = 0x6474E551
PT_LOAD = 1
PF_X = 1
PF_W = 2
PF_R = 4
EXPECTED_STACK = 0x100000


def fail(path: Path, message: str) -> None:
    raise SystemExit(f"{path}: {message}")


MACHINES = {
    "i386": (1, 3, 52, 32, "<", "little"),
    "aarch64": (2, 183, 64, 56, "<", "little"),
    "sparcv9": (2, 43, 64, 56, ">", "big"),
}


def check(path: Path, machine: str) -> None:
    data = path.read_bytes()
    (elf_class, expected_machine, ehsize, phentsize, endian,
     endian_name) = MACHINES[machine]
    if len(data) < ehsize or data[:4] != b"\x7fELF":
        fail(path, "not an ELF file")
    expected_data = 1 if endian == "<" else 2
    if data[4] != elf_class or data[5] != expected_data or data[6] != 1:
        fail(path, f"expected current {endian_name}-endian "
             f"ELF{elf_class * 32}")
    header_format = (endian + "16sHHIIIIIHHHHHH" if elf_class == 1 else
                     endian + "16sHHIQQQIHHHHHH")
    header = struct.unpack_from(header_format, data, 0)
    e_type, e_machine = header[1], header[2]
    e_phoff, e_ehsize, e_phentsize, e_phnum = (
        header[5], header[8], header[9], header[10]
    )
    if e_type != 2 or e_machine != expected_machine or e_ehsize != ehsize:
        fail(path, f"expected ELF{elf_class * 32}/{machine} ET_EXEC")
    if e_phentsize != phentsize or e_phnum == 0:
        fail(path, "invalid program-header table")
    table_size = e_phentsize * e_phnum
    if e_phoff > len(data) or table_size > len(data) - e_phoff:
        fail(path, "program-header table is outside the file")
    stacks = []
    loads = []
    for index in range(e_phnum):
        program_format = (endian + "IIIIIIII" if elf_class == 1 else
                          endian + "IIQQQQQQ")
        program = struct.unpack_from(program_format, data,
                                     e_phoff + index * e_phentsize)
        if program[0] == PT_GNU_STACK:
            stacks.append(program)
        elif program[0] == PT_LOAD:
            loads.append(program)
    if len(stacks) != 1:
        fail(path, f"expected one PT_GNU_STACK, found {len(stacks)}")
    if elf_class == 1:
        _, _, _, _, p_filesz, p_memsz, p_flags, _ = stacks[0]
    else:
        _, p_flags, _, _, _, p_filesz, p_memsz, _ = stacks[0]
    if p_filesz != 0:
        fail(path, "PT_GNU_STACK has file contents")
    if p_memsz != EXPECTED_STACK:
        fail(path, f"PT_GNU_STACK size is 0x{p_memsz:x}, expected 0x100000")
    if p_flags != PF_R | PF_W or p_flags & PF_X:
        fail(path, f"PT_GNU_STACK flags are 0x{p_flags:x}, expected RW")
    if machine == "sparcv9":
        if not loads:
            fail(path, "expected at least one PT_LOAD")
        for index, program in enumerate(loads):
            alignment = program[7]
            vaddr = program[3]
            memory_size = program[6]
            if alignment < 8192 or alignment & (alignment - 1):
                fail(path, f"PT_LOAD {index} is not 8 KiB aligned")
            if vaddr < 0x2000 or memory_size > 0x80000000 - vaddr:
                fail(path, f"PT_LOAD {index} exceeds the initial user VA")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--machine", choices=MACHINES, default="i386")
    parser.add_argument("elf", nargs="+")
    args = parser.parse_args()
    for argument in args.elf:
        check(Path(argument), args.machine)


if __name__ == "__main__":
    main()

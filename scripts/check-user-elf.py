#!/usr/bin/env python3
"""Validate the zedBSD ELF32/i386 user-stack program header."""

import struct
import sys
from pathlib import Path

PT_GNU_STACK = 0x6474E551
PF_X = 1
PF_W = 2
PF_R = 4
EXPECTED_STACK = 0x100000


def fail(path: Path, message: str) -> None:
    raise SystemExit(f"{path}: {message}")


def check(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        fail(path, "not an ELF file")
    if data[4] != 1 or data[5] != 1 or data[6] != 1:
        fail(path, "expected current little-endian ELF32")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)
    e_type, e_machine = header[1], header[2]
    e_phoff, e_ehsize, e_phentsize, e_phnum = (
        header[5], header[8], header[9], header[10]
    )
    if e_type != 2 or e_machine != 3 or e_ehsize != 52:
        fail(path, "expected ELF32/i386 ET_EXEC")
    if e_phentsize != 32 or e_phnum == 0:
        fail(path, "invalid program-header table")
    table_size = e_phentsize * e_phnum
    if e_phoff > len(data) or table_size > len(data) - e_phoff:
        fail(path, "program-header table is outside the file")
    stacks = []
    for index in range(e_phnum):
        program = struct.unpack_from("<IIIIIIII", data,
                                     e_phoff + index * e_phentsize)
        if program[0] == PT_GNU_STACK:
            stacks.append(program)
    if len(stacks) != 1:
        fail(path, f"expected one PT_GNU_STACK, found {len(stacks)}")
    _, _, _, _, p_filesz, p_memsz, p_flags, _ = stacks[0]
    if p_filesz != 0:
        fail(path, "PT_GNU_STACK has file contents")
    if p_memsz != EXPECTED_STACK:
        fail(path, f"PT_GNU_STACK size is 0x{p_memsz:x}, expected 0x100000")
    if p_flags != PF_R | PF_W or p_flags & PF_X:
        fail(path, f"PT_GNU_STACK flags are 0x{p_flags:x}, expected RW")


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(f"usage: {sys.argv[0]} ELF...")
    for argument in sys.argv[1:]:
        check(Path(argument))


if __name__ == "__main__":
    main()

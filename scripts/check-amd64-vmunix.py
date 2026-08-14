#!/usr/bin/env python3
"""Validate the deliberately small ELF64 contract accepted by PC/AT ZBL2."""

import struct
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"amd64 vmunix check: {message}")


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: check-amd64-vmunix.py VMUNIX")
    data = Path(sys.argv[1]).read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        fail("not an ELF file")
    if data[4] != 2 or data[5] != 1:
        fail("expected little-endian ELF64")
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
    machine, entry, phoff = header[2], header[4], header[5]
    ehsize, phentsize, phnum = header[8], header[9], header[10]
    if machine != 62 or ehsize != 64 or phentsize != 56:
        fail("unexpected ELF64 header")
    if not (0xFFFFFFFF80000000 <= entry < 0xFFFFFFFFC0000000):
        fail("entry is outside the bootstrap high-half window")
    if phnum == 0 or phnum > 8 or phoff + phnum * phentsize > 512:
        fail("program header table is outside the first sector")
    loads = 0
    entry_is_executable = False
    for index in range(phnum):
        ph = struct.unpack_from("<IIQQQQQQ", data, phoff + index * phentsize)
        p_type, flags, offset, vaddr, paddr, filesz, memsz, _ = ph
        if p_type != 1:
            continue
        loads += 1
        if offset & 511:
            fail("PT_LOAD file offset is not sector aligned")
        if paddr >= 0x40000000 or paddr + memsz > 0x40000000:
            fail("PT_LOAD is outside the initial one-GiB physical map")
        if vaddr < 0xFFFFFFFF80000000:
            fail("PT_LOAD is not in the high half")
        if filesz > memsz:
            fail("PT_LOAD filesz exceeds memsz")
        if flags & 2 and flags & 1:
            fail("PT_LOAD is writable and executable")
        if flags & 1 and vaddr <= entry < vaddr + memsz:
            entry_is_executable = True
    if loads == 0:
        fail("no PT_LOAD segments")
    if not entry_is_executable:
        fail("entry is not covered by an executable PT_LOAD")
    print(f"amd64 vmunix check: PASS ({loads} PT_LOAD, entry={entry:#x})")


if __name__ == "__main__":
    main()

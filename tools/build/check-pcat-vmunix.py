#!/usr/bin/env python3
"""Validate the ELF/Multiboot contract shared by GRUB and the PC/AT MBR."""

import struct
import sys


ELF_HEADER = struct.Struct("<16sHHIIIIIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIIIIIII")
PT_LOAD = 1
PF_X = 1
EM_386 = 3
MULTIBOOT_MAGIC = 0x1BADB002
MULTIBOOT_ADDRESS_FIELDS = 0x00010000
KERNEL_VIRTUAL_OFFSET = 0x80000000
RAW_KERNEL_SECTORS = 65535


def fail(message):
    raise ValueError(message)


def check(path):
    with open(path, "rb") as source:
        image = source.read()
    if len(image) < ELF_HEADER.size:
        fail("truncated ELF header")
    fields = ELF_HEADER.unpack_from(image)
    ident = fields[0]
    if ident[:7] != b"\x7fELF\x01\x01\x01":
        fail("not a little-endian ELF32 image")
    machine, entry, phoff = fields[2], fields[4], fields[5]
    ehsize, phentsize, phnum = fields[8], fields[9], fields[10]
    if machine != EM_386:
        fail("ELF machine is not i386")
    if ehsize != ELF_HEADER.size or phentsize != PROGRAM_HEADER.size:
        fail("unexpected ELF/program-header size")
    if phnum == 0 or phoff + phnum * phentsize > 512:
        fail("program-header table is not wholly in the first sector")

    executable_entry = False
    physical_ranges = []
    for index in range(phnum):
        ph = PROGRAM_HEADER.unpack_from(image, phoff + index * phentsize)
        kind, offset, vaddr, paddr, filesz, memsz, flags, _align = ph
        if kind != PT_LOAD:
            continue
        if filesz > memsz or offset + filesz > len(image):
            fail(f"PT_LOAD {index} has invalid file/memory size")
        if offset & 511:
            fail(f"PT_LOAD {index} is not sector aligned")
        if paddr < 0x00100000 or paddr + memsz > 0x08000000:
            fail(f"PT_LOAD {index} is outside the supported physical window")
        if vaddr - paddr != KERNEL_VIRTUAL_OFFSET:
            fail(f"PT_LOAD {index} does not use the high-kernel mapping")
        if (offset + filesz + 511) // 512 > RAW_KERNEL_SECTORS:
            fail(f"PT_LOAD {index} exceeds the raw-kernel area")
        end = paddr + memsz
        if end < paddr:
            fail(f"PT_LOAD {index} address overflow")
        for other_start, other_end in physical_ranges:
            if paddr < other_end and other_start < end:
                fail(f"PT_LOAD {index} overlaps another load segment")
        physical_ranges.append((paddr, end))
        if flags & PF_X and paddr <= entry < paddr + filesz:
            executable_entry = True
    if not physical_ranges:
        fail("ELF contains no PT_LOAD segment")
    if not executable_entry:
        fail("physical entry is outside an executable file-backed segment")

    found = False
    search_end = min(len(image), 8192)
    for offset in range(0, search_end - 11, 4):
        magic, flags, checksum = struct.unpack_from("<III", image, offset)
        if magic != MULTIBOOT_MAGIC:
            continue
        if (magic + flags + checksum) & 0xFFFFFFFF:
            fail("Multiboot header checksum is invalid")
        if not flags & MULTIBOOT_ADDRESS_FIELDS:
            fail("Multiboot header lacks address fields required by this ELF")
        found = True
        break
    if not found:
        fail("Multiboot header is absent from the first 8 KiB")


def main(argv):
    if len(argv) != 2:
        print(f"usage: {argv[0]} VMUNIX", file=sys.stderr)
        return 2
    try:
        check(argv[1])
    except (OSError, ValueError, struct.error) as error:
        print(f"check-pcat-vmunix: {error}", file=sys.stderr)
        return 1
    print(f"PC/AT vmunix contract: {argv[1]}: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

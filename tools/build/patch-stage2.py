#!/usr/bin/env python3

"""Validate the PC-98 single-image ELF and patch its B98S v2 header."""
import pathlib
import struct
import sys

KERNEL_VADDR = 0x80100000
KERNEL_PADDR = 0x00100000
KERNEL_LIMIT = 0x00F00000
SECTOR = 512
HEADER_OFFSET = SECTOR
HEADER_SIZE = 20

path = pathlib.Path(sys.argv[1])
image = bytearray(path.read_bytes())


def fail(message):
    raise SystemExit(f"{path}: {message}")


if len(image) < SECTOR or image[:4] != b"\x7fELF":
    fail("not an ELF image")
ei_class, ei_data = image[4], image[5]
e_type, e_machine = struct.unpack_from("<HH", image, 16)
if ei_class != 1 or ei_data != 1 or e_type != 2 or e_machine != 3:
    fail("not a little-endian ELF32 i386 executable")

e_entry, e_phoff = struct.unpack_from("<II", image, 24)
e_phentsize, e_phnum = struct.unpack_from("<HH", image, 42)
if e_phoff != 52 or e_phentsize != 32:
    fail("program headers must follow the ELF header")
if e_phoff + e_phnum * e_phentsize > SECTOR:
    fail("program headers spill past the first sector")

loads = []
for index in range(e_phnum):
    values = struct.unpack_from("<IIIIII", image,
                                e_phoff + index * e_phentsize)
    if values[0] == 1:
        loads.append(values[1:])
if len(loads) != 1:
    fail(f"expected exactly 1 PT_LOAD segment, found {len(loads)}")

p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = loads[0]
if p_offset != SECTOR or p_offset + p_filesz > len(image):
    fail("PT_LOAD must start at file offset 512 and fit in the file")
if p_vaddr != KERNEL_VADDR or p_paddr != KERNEL_PADDR:
    fail(f"PT_LOAD must map {KERNEL_VADDR:#x} to {KERNEL_PADDR:#x}")
if p_memsz < p_filesz or p_paddr + p_memsz >= KERNEL_LIMIT:
    fail(f"PT_LOAD reaches the PC-98 15 MiB hole at {KERNEL_LIMIT:#x}")
if not p_vaddr <= e_entry < p_vaddr + p_filesz:
    fail(f"entry {e_entry:#x} is outside the file-backed kernel image")

if image[HEADER_OFFSET:HEADER_OFFSET + 4] != b"B98S":
    fail("B98S header not at file offset 512")
version, header_size = struct.unpack_from("<HH", image, HEADER_OFFSET + 4)
if version != 2 or header_size != HEADER_SIZE:
    fail("B98S header is not version 2")

payload_start = HEADER_OFFSET + HEADER_SIZE
physical_entry = e_entry - 0x80000000
struct.pack_into("<II", image, HEADER_OFFSET + 8, len(image), physical_entry)
checksum = sum(image[payload_start:]) & 0xffffffff
struct.pack_into("<I", image, HEADER_OFFSET + 16, checksum)
path.write_bytes(image)

margin = KERNEL_LIMIT - (p_paddr + p_memsz)
print(f"{path}: {len(image)} bytes, checksum {checksum:08x}")
print(f"  kernel: {p_filesz:#8x} file / {p_memsz:#8x} mem at "
      f"{p_paddr:#x} (margin {margin:#x})")

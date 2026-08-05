#!/usr/bin/env python3

"""Check the BOOT.SYS ELF subset contract and patch the B98S v2 header.

BOOT.SYS is a two-segment ELF that Stage 1 loads with a deliberately
tiny real-mode loader.  Stage 1 trusts almost everything about the
image, so every property it relies on is enforced here, at build time:

  C1  ELF32, little endian, EM_386, ET_EXEC
  C2  e_phoff = 52, e_phentsize = 32, all headers inside the first
      512 bytes
  C3  exactly two PT_LOAD segments: low then high
  C4  each p_offset is a multiple of 512, ascending, non-overlapping
  C5  low:  p_vaddr = p_paddr = 0x80020000, p_offset = 512; physical
      end (address with bit 31 clear) at or below 0x80000
      at or below 0x80000 (boot parameters)
  C6  high: p_vaddr = p_paddr = 0x80100000, physical end below the PC-98
      15 MiB hole at 0xF00000
  C8  e_entry inside the low segment

The B98S v2 header sits at file offset 512 (the head of the low
segment).  Its file-size, entry, and checksum fields are patched here;
the checksum is the 32-bit sum of every file byte from offset 532
(right after the header) to the end of the file.
"""
import pathlib
import struct
import sys

LOW_VADDR = 0x80020000
LOW_PADDR = 0x20000
LOW_LIMIT = 0x80000
HIGH_VADDR = 0x80100000
HIGH_PADDR = 0x100000
HIGH_LIMIT = 0xF00000
SECTOR = 512
HEADER_OFFSET = SECTOR
HEADER_SIZE = 20

path = pathlib.Path(sys.argv[1])
image = bytearray(path.read_bytes())


def fail(message):
    raise SystemExit(f"{path}: {message}")


# C1: identification.
if len(image) < SECTOR or image[:4] != b"\x7fELF":
    fail("not an ELF image")
ei_class, ei_data = image[4], image[5]
e_type, e_machine = struct.unpack_from("<HH", image, 16)
if ei_class != 1 or ei_data != 1 or e_type != 2 or e_machine != 3:
    fail("not a little-endian ELF32 i386 executable (C1)")

# C2: header geometry.
e_entry, e_phoff = struct.unpack_from("<II", image, 24)
e_phentsize, e_phnum = struct.unpack_from("<HH", image, 42)
if e_phoff != 52 or e_phentsize != 32:
    fail("program headers must follow the ELF header (C2)")
if e_phoff + e_phnum * e_phentsize > SECTOR:
    fail("program headers spill past the first sector (C2)")

# C3/C4: exactly two ascending, sector-aligned PT_LOAD segments.
loads = []
for index in range(e_phnum):
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = \
        struct.unpack_from("<IIIIII", image, e_phoff + index * e_phentsize)
    if p_type != 1:
        continue
    loads.append((p_offset, p_vaddr, p_paddr, p_filesz, p_memsz))
if len(loads) != 2:
    fail(f"expected exactly 2 PT_LOAD segments, found {len(loads)} (C3)")
for p_offset, _, _, p_filesz, _ in loads:
    if p_offset % SECTOR != 0:
        fail(f"PT_LOAD offset {p_offset:#x} not sector aligned (C4)")
    if p_offset + p_filesz > len(image):
        fail("PT_LOAD extends past the end of the file (C4)")
if not loads[0][0] < loads[1][0]:
    fail("PT_LOAD segments not ascending by file offset (C4)")
if loads[0][0] + loads[0][3] > loads[1][0]:
    fail("PT_LOAD segments overlap in the file (C4)")

low_offset, low_vaddr, low_link_paddr, low_filesz, low_memsz = loads[0]
high_offset, high_vaddr, high_link_paddr, high_filesz, high_memsz = loads[1]
low_paddr = low_vaddr & 0x7fffffff
high_paddr = high_vaddr & 0x7fffffff

# C5/C6: placement.
if (low_vaddr != LOW_VADDR or low_link_paddr != LOW_VADDR or
        low_paddr != LOW_PADDR or low_offset != SECTOR):
    fail(f"low segment must be linked at {LOW_VADDR:#x} and offset 512 (C5)")
if low_paddr + low_memsz > LOW_LIMIT:
    fail(f"low segment end {low_paddr + low_memsz:#x} overlaps boot "
         f"parameters at {LOW_LIMIT:#x} (C5)")
if (high_vaddr != HIGH_VADDR or high_link_paddr != HIGH_VADDR or
        high_paddr != HIGH_PADDR):
    fail(f"high segment must be linked at {HIGH_VADDR:#x} (C6)")
if high_paddr + high_memsz > HIGH_LIMIT:
    fail(f"high segment end {high_paddr + high_memsz:#x} reaches the "
         f"PC-98 15MB hole at {HIGH_LIMIT:#x} (C6)")

# C8: entry.
if not low_vaddr <= e_entry < low_vaddr + low_filesz:
    fail(f"entry {e_entry:#x} outside the low segment (C8)")

# B98S v2 header at the head of the low segment.
if image[HEADER_OFFSET:HEADER_OFFSET + 4] != b"B98S":
    fail("B98S header not at file offset 512 (C9)")
version, header_size = struct.unpack_from("<HH", image, HEADER_OFFSET + 4)
if version != 2 or header_size != HEADER_SIZE:
    fail("B98S header is not version 2")

payload_start = HEADER_OFFSET + HEADER_SIZE
physical_entry = e_entry & 0x7fffffff
struct.pack_into("<II", image, HEADER_OFFSET + 8, len(image), physical_entry)
checksum = sum(image[payload_start:]) & 0xffffffff
struct.pack_into("<I", image, HEADER_OFFSET + 16, checksum)

path.write_bytes(image)

low_margin = LOW_LIMIT - (low_paddr + low_memsz)
high_margin = HIGH_LIMIT - (high_paddr + high_memsz)
print(f"{path}: {len(image)} bytes, checksum {checksum:08x}")
print(f"  low:  {low_filesz:#8x} file / {low_memsz:#8x} mem at "
      f"{low_paddr:#x} (margin {low_margin:#x})")
print(f"  high: {high_filesz:#8x} file / {high_memsz:#8x} mem at "
      f"{high_paddr:#x} (margin {high_margin:#x})")

#!/usr/bin/env python3
import argparse
import pathlib
import struct
import sys

EM_AARCH64 = 183
IMAGE_MAGIC = 0x644D5241
SHT_SYMTAB = 2
PAGE_SIZE = 4096
REQUIRED_SYMBOLS = ("_start", "__bss_end", "__kernel_vma_end",
                    "__arm64_image_file_size")

def fail(message):
    raise SystemExit("arm64 vmunix check: " + message)

def elf_symbols(data):
    """Returns the ELF's symbol table as a name-to-value dictionary."""
    shoff = struct.unpack_from("<Q", data, 40)[0]
    shentsize, shnum = struct.unpack_from("<HH", data, 58)
    sections = []
    for index in range(shnum):
        base = shoff + index * shentsize
        kind = struct.unpack_from("<I", data, base + 4)[0]
        offset, size, link = struct.unpack_from("<QQI", data, base + 24)
        entsize = struct.unpack_from("<Q", data, base + 56)[0]
        sections.append((kind, offset, size, link, entsize))
    symbols = {}
    for kind, offset, size, link, entsize in sections:
        if kind != SHT_SYMTAB or entsize == 0:
            continue
        strings_offset = sections[link][1]
        for at in range(offset, offset + size, entsize):
            name_offset = struct.unpack_from("<I", data, at)[0]
            value = struct.unpack_from("<Q", data, at + 8)[0]
            name_start = strings_offset + name_offset
            name_end = data.index(b"\0", name_start)
            name = data[name_start:name_end].decode()
            if name:
                symbols[name] = value
    return symbols

def check_elf(path):
    data = pathlib.Path(path).read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        fail("not ELF")
    if data[4:6] != b"\x02\x01":
        fail("expected ELF64 little-endian")
    machine = struct.unpack_from("<H", data, 18)[0]
    entry = struct.unpack_from("<Q", data, 24)[0]
    if machine != EM_AARCH64:
        fail(f"wrong e_machine {machine}")
    if entry < 0xFFFF000000000000:
        fail(f"entry is not in the high kernel mapping: {entry:#x}")
    symbols = elf_symbols(data)
    for name in REQUIRED_SYMBOLS:
        if name not in symbols:
            fail(f"symbol {name} missing from the ELF")
    return entry, symbols

def check_image(path, symbols):
    data = pathlib.Path(path).read_bytes()
    if len(data) < 64:
        fail("Image shorter than header")
    text_offset, image_size, flags = struct.unpack_from("<QQQ", data, 8)
    magic = struct.unpack_from("<I", data, 56)[0]
    if text_offset != 0x80000:
        fail(f"bad text_offset {text_offset:#x}")
    if flags & 1:
        fail("big-endian Image flag set")
    if magic != IMAGE_MAGIC:
        fail(f"bad Image magic {magic:#x}")

    # The image size counts the BSS after the loaded bytes (the Linux arm64
    # boot protocol), so a bootloader keeps its other data out of it.
    start = symbols["_start"]
    bss_end = symbols["__bss_end"] - start
    end = symbols["__kernel_vma_end"] - start
    expected = (end + PAGE_SIZE - 1) // PAGE_SIZE * PAGE_SIZE
    if image_size < bss_end:
        fail(f"header size {image_size:#x} ends before the BSS end {bss_end:#x}")
    if image_size != expected:
        fail(f"header size {image_size:#x} != image end {expected:#x}")
    if image_size < len(data):
        fail(f"header size {image_size:#x} < file size {len(data):#x}")

    # The boards that move the image copy this many bytes: all of the file,
    # and no more than the image size the bootloader leaves free.
    file_size = symbols["__arm64_image_file_size"]
    if file_size < len(data):
        fail(f"linked file size {file_size:#x} < file size {len(data):#x}")
    if file_size > image_size:
        fail(f"linked file size {file_size:#x} > header size {image_size:#x}")
    return image_size, bss_end

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--elf", required=True)
    p.add_argument("--image")
    args = p.parse_args()
    entry, symbols = check_elf(args.elf)
    detail = ""
    if args.image:
        image_size, bss_end = check_image(args.image, symbols)
        detail = (f", image={args.image}, image_size={image_size:#x}" +
                  f", bss_end={bss_end:#x}")
    print(f"arm64 vmunix check: PASS (entry={entry:#x}{detail})")

if __name__ == "__main__":
    main()

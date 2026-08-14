#!/usr/bin/env python3
import argparse
import pathlib
import struct
import sys

EM_AARCH64 = 183
IMAGE_MAGIC = 0x644D5241

def fail(message):
    raise SystemExit("arm64 vmunix check: " + message)

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
    return entry

def check_image(path):
    data = pathlib.Path(path).read_bytes()
    if len(data) < 64:
        fail("Image shorter than header")
    text_offset, image_size, flags = struct.unpack_from("<QQQ", data, 8)
    magic = struct.unpack_from("<I", data, 56)[0]
    if text_offset != 0x80000:
        fail(f"bad text_offset {text_offset:#x}")
    if image_size != len(data):
        fail(f"header size {image_size} != file size {len(data)}")
    if flags & 1:
        fail("big-endian Image flag set")
    if magic != IMAGE_MAGIC:
        fail(f"bad Image magic {magic:#x}")

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--elf", required=True)
    p.add_argument("--image")
    p.add_argument("--fix-image", action="store_true")
    args = p.parse_args()
    entry = check_elf(args.elf)
    if args.image:
        if args.fix_image:
            image_path = pathlib.Path(args.image)
            image = bytearray(image_path.read_bytes())
            if len(image) < 64:
                fail("Image shorter than header")
            struct.pack_into("<Q", image, 16, len(image))
            image_path.write_bytes(image)
        check_image(args.image)
    print(f"arm64 vmunix check: PASS (entry={entry:#x}" +
          (f", image={args.image})" if args.image else ")"))

if __name__ == "__main__":
    main()

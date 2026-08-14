#!/usr/bin/env python3
"""Create a Sun disklabel/FAT16 zedBSD SPARC V9 disk image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path

SECTOR_SIZE = 512
IMAGE_SIZE_MIB = 128
IMAGE_SECTORS = IMAGE_SIZE_MIB * 2048
HEADS = 64
SECTORS_PER_TRACK = 64
SECTORS_PER_CYLINDER = HEADS * SECTORS_PER_TRACK
BOOT_SLICE_SECTORS = SECTORS_PER_CYLINDER
FAT_LBA = BOOT_SLICE_SECTORS
FAT_SECTORS = IMAGE_SECTORS - FAT_LBA
STAGE1_LBA = 1
STAGE1_LOAD_SIZE = 15 * SECTOR_SIZE
STAGE2_HEADER_LBA = 32
STAGE2_PAYLOAD_LBA = 33
STAGE2_MAGIC = 0x5A533932
STAGE2_VERSION = 1
STAGE2_HEADER_SIZE = 32
STAGE2_LOAD_ADDRESS = 0x00100000
SUN_LABEL_MAGIC = 0xDABE
SUN_VTOC_SANITY = 0x600DDEEE


def run(*arguments: str) -> None:
    subprocess.run(arguments, check=True)


def require(path: Path) -> None:
    if not path.is_file():
        raise SystemExit(f"missing input: {path}")


def put16(buffer: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">H", buffer, offset, value)


def put32(buffer: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">I", buffer, offset, value)


def sun_label() -> bytes:
    label = bytearray(SECTOR_SIZE)
    text = b"zedBSD SPARC V9 sun4u"
    label[:len(text)] = text

    put32(label, 128, 1)
    label[132:140] = b"ZEDSP9  "
    put16(label, 140, 8)
    for index, identifier in enumerate((2, 4, 5, 0, 0, 0, 0, 0)):
        put16(label, 142 + index * 4, identifier)
        put16(label, 144 + index * 4, 0)
    put32(label, 188, SUN_VTOC_SANITY)

    put16(label, 420, 7200)
    put16(label, 422, IMAGE_SECTORS // SECTORS_PER_CYLINDER)
    put16(label, 424, 0)
    put16(label, 430, 1)
    put16(label, 432, IMAGE_SECTORS // SECTORS_PER_CYLINDER)
    put16(label, 434, 0)
    put16(label, 436, HEADS)
    put16(label, 438, SECTORS_PER_TRACK)

    partitions = (
        (0, BOOT_SLICE_SECTORS),
        (1, FAT_SECTORS),
        (0, IMAGE_SECTORS),
        (0, 0),
        (0, 0),
        (0, 0),
        (0, 0),
        (0, 0),
    )
    for index, (start_cylinder, sectors) in enumerate(partitions):
        put32(label, 444 + index * 8, start_cylinder)
        put32(label, 448 + index * 8, sectors)
    put16(label, 508, SUN_LABEL_MAGIC)
    checksum = 0
    for offset in range(0, 510, 2):
        checksum ^= struct.unpack_from(">H", label, offset)[0]
    put16(label, 510, checksum)
    return bytes(label)


def stage2_header(payload: bytes) -> bytes:
    checksum = sum(payload) & 0xFFFFFFFF
    header = struct.pack(
        ">IHHIIQII",
        STAGE2_MAGIC,
        STAGE2_VERSION,
        STAGE2_HEADER_SIZE,
        STAGE2_PAYLOAD_LBA,
        len(payload),
        STAGE2_LOAD_ADDRESS,
        0,
        checksum,
    )
    return header.ljust(SECTOR_SIZE, b"\0")


def stage1_aout(payload: bytes) -> bytes:
    """Wrap the raw stage1 in the OMAGIC format OpenBIOS relocates."""
    if len(payload) > STAGE1_LOAD_SIZE:
        raise SystemExit("stage1 exceeds the OpenBIOS 15-sector load area")
    header = struct.pack(
        ">BBHIIIIIII",
        0, 3, 0o407, len(payload), 0, 0, 0, 0x4000, 0, 0,
    )
    return header + payload.ljust(STAGE1_LOAD_SIZE, b"\0")


def create(args: argparse.Namespace) -> None:
    for path in (args.stage1, args.stage2, args.kernel):
        require(path)
    if args.shell is not None:
        require(args.shell)
    if args.output.exists() and not args.force:
        raise SystemExit(f"output exists (use --force): {args.output}")

    stage1 = args.stage1.read_bytes()
    stage2 = args.stage2.read_bytes()
    if not stage1 or len(stage1) > STAGE1_LOAD_SIZE:
        raise SystemExit("stage1 exceeds the OpenBIOS 15-sector load area")
    stage2_sectors = (len(stage2) + SECTOR_SIZE - 1) // SECTOR_SIZE
    if not stage2 or STAGE2_PAYLOAD_LBA + stage2_sectors > FAT_LBA:
        raise SystemExit("stage2 overlaps the FAT slice")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=args.output.name + ".", dir=args.output.parent
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with temporary.open("r+b") as image:
            image.truncate(IMAGE_SECTORS * SECTOR_SIZE)
            image.seek(0)
            image.write(sun_label())
            # OpenBIOS relocates OMAGIC text from load-base+32 back to
            # load-base.  The padded body satisfies its fixed 7680-byte read.
            image.seek(STAGE1_LBA * SECTOR_SIZE)
            image.write(stage1_aout(stage1))
            image.seek(STAGE2_HEADER_LBA * SECTOR_SIZE)
            image.write(stage2_header(stage2))
            image.seek(STAGE2_PAYLOAD_LBA * SECTOR_SIZE)
            image.write(stage2)

        fat_spec = f"{temporary}@@{FAT_LBA * SECTOR_SIZE}"
        run("mformat", "-i", fat_spec, "-T", str(FAT_SECTORS),
            "-v", "ZEDSP9", "::")
        run("mcopy", "-i", fat_spec, str(args.kernel), "::/VMUNIX.S9")
        if args.shell is not None:
            run("mmd", "-i", fat_spec, "::/sparcv9")
            run("mmd", "-i", fat_spec, "::/sparcv9/bin")
            run("mcopy", "-i", fat_spec, str(args.shell),
                "::/sparcv9/bin/sh")

        checker = Path(__file__).with_name("check-sparcv9-hdd-image.py")
        command = [
            "python3", str(checker),
            "--stage1", str(args.stage1),
            "--stage2", str(args.stage2),
            "--kernel", str(args.kernel),
        ]
        if args.shell is not None:
            command += ["--shell", str(args.shell)]
        command.append(str(temporary))
        run(*command)
        os.replace(temporary, args.output)
    finally:
        if temporary.exists():
            temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage1", type=Path, required=True)
    parser.add_argument("--stage2", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--shell", type=Path)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)
    create(parser.parse_args())


if __name__ == "__main__":
    main()

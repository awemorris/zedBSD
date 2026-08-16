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
LEGACY_IMAGE_SECTORS = 128 * 2048
UFS_IMAGE_SECTORS = 190 * 2048
HEADS = 64
SECTORS_PER_TRACK = 64
SECTORS_PER_CYLINDER = HEADS * SECTORS_PER_TRACK
BOOT_SLICE_SECTORS = SECTORS_PER_CYLINDER
FAT_LBA = BOOT_SLICE_SECTORS
UFS_FAT_SECTORS = 128 * 2048
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


def sun_label(image_sectors: int, fat_sectors: int,
              root_sectors: int = 0) -> bytes:
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
    put16(label, 422, image_sectors // SECTORS_PER_CYLINDER)
    put16(label, 424, 0)
    put16(label, 430, 1)
    put16(label, 432, image_sectors // SECTORS_PER_CYLINDER)
    put16(label, 434, 0)
    put16(label, 436, HEADS)
    put16(label, 438, SECTORS_PER_TRACK)

    partitions = (
        (0, BOOT_SLICE_SECTORS),
        (1, fat_sectors),
        (0, image_sectors),
        (1 + fat_sectors // SECTORS_PER_CYLINDER, root_sectors),
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
    if args.ufs_root is not None:
        require(args.ufs_root)
        if args.ufs_root.stat().st_size % SECTOR_SIZE:
            raise SystemExit("UFS1 root image is not sector aligned")
    if args.output.exists() and not args.force:
        raise SystemExit(f"output exists (use --force): {args.output}")

    stage1 = args.stage1.read_bytes()
    stage2 = args.stage2.read_bytes()
    if not stage1 or len(stage1) > STAGE1_LOAD_SIZE:
        raise SystemExit("stage1 exceeds the OpenBIOS 15-sector load area")
    stage2_sectors = (len(stage2) + SECTOR_SIZE - 1) // SECTOR_SIZE
    if not stage2 or STAGE2_PAYLOAD_LBA + stage2_sectors > FAT_LBA:
        raise SystemExit("stage2 overlaps the FAT slice")

    root_sectors = (args.ufs_root.stat().st_size // SECTOR_SIZE
                    if args.ufs_root is not None else 0)
    image_sectors = UFS_IMAGE_SECTORS if root_sectors else LEGACY_IMAGE_SECTORS
    fat_sectors = UFS_FAT_SECTORS if root_sectors else image_sectors - FAT_LBA
    root_lba = FAT_LBA + fat_sectors
    if root_lba % SECTORS_PER_CYLINDER or root_lba + root_sectors > image_sectors:
        raise SystemExit("UFS1 root does not fit cylinder-aligned slice d")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=args.output.name + ".", dir=args.output.parent
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with temporary.open("r+b") as image:
            image.truncate(image_sectors * SECTOR_SIZE)
            image.seek(0)
            image.write(sun_label(image_sectors, fat_sectors, root_sectors))
            # OpenBIOS relocates OMAGIC text from load-base+32 back to
            # load-base.  The padded body satisfies its fixed 7680-byte read.
            image.seek(STAGE1_LBA * SECTOR_SIZE)
            image.write(stage1_aout(stage1))
            image.seek(STAGE2_HEADER_LBA * SECTOR_SIZE)
            image.write(stage2_header(stage2))
            image.seek(STAGE2_PAYLOAD_LBA * SECTOR_SIZE)
            image.write(stage2)

        fat_spec = f"{temporary}@@{FAT_LBA * SECTOR_SIZE}"
        run("mformat", "-i", fat_spec, "-T", str(fat_sectors),
            "-v", "ZEDSP9", "::")
        run("mcopy", "-i", fat_spec, str(args.kernel), "::/VMUNIX.S9")
        if args.shell is not None:
            run("mmd", "-i", fat_spec, "::/bin")
            run("mcopy", "-i", fat_spec, str(args.shell), "::/bin/sh")
        if args.sysctl is not None:
            if args.shell is None:
                run("mmd", "-i", fat_spec, "::/bin")
            run("mcopy", "-i", fat_spec, str(args.sysctl), "::/bin/sysctl")
        if args.ufs_root is not None:
            with temporary.open("r+b") as image:
                image.seek(root_lba * SECTOR_SIZE)
                image.write(args.ufs_root.read_bytes())

        checker = Path(__file__).with_name("check-sparcv9-hdd-image.py")
        command = [
            "python3", str(checker),
            "--stage1", str(args.stage1),
            "--stage2", str(args.stage2),
            "--kernel", str(args.kernel),
        ]
        if args.shell is not None:
            command += ["--shell", str(args.shell)]
        if args.sysctl is not None:
            command += ["--sysctl", str(args.sysctl)]
        if args.ufs_root is not None:
            command += ["--ufs-root", str(args.ufs_root)]
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
    parser.add_argument("--sysctl", type=Path)
    parser.add_argument("--ufs-root", type=Path)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)
    create(parser.parse_args())


if __name__ == "__main__":
    main()

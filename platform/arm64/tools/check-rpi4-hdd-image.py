#!/usr/bin/env python3
"""Validate the Raspberry Pi 4 MBR/FAT32 image and boot payloads."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import hashlib
import struct
import subprocess
import tempfile
from pathlib import Path

SECTOR = 512
PARTITION_LBA = 2048
PARTITION_BLOCKS = 262144
PARTITION_TYPE = 0x0C
IMAGE_BLOCKS = 524288
ROOT_LBA = 264192


def fail(message: str) -> None:
    raise SystemExit("Raspberry Pi 4 image check: " + message)


def extract(image: Path, name: str) -> bytes:
    with tempfile.TemporaryDirectory(prefix="zedbsd-rpi4-check-") as work:
        output = Path(work) / "payload"
        subprocess.run(["mcopy", "-n", "-i",
                        f"{image}@@{PARTITION_LBA * SECTOR}",
                        f"::{name}", str(output)], check=True,
                       stdout=subprocess.DEVNULL)
        return output.read_bytes()


def same_file(image: Path, name: str, source: Path) -> None:
    if hashlib.sha256(extract(image, name)).digest() != \
            hashlib.sha256(source.read_bytes()).digest():
        fail(f"/{name} differs from {source}")


def check(args: argparse.Namespace) -> None:
    size = args.image.stat().st_size
    expected_size = IMAGE_BLOCKS * SECTOR
    if args.ufs_root is not None:
        root_size = args.ufs_root.stat().st_size
        if root_size == 0 or root_size % SECTOR:
            fail("invalid UFS root size")
        expected_size = max(expected_size, ROOT_LBA * SECTOR + root_size)
    if size != expected_size:
        fail("unexpected image size")
    with args.image.open("rb") as stream:
        mbr = stream.read(SECTOR)
        if mbr[510:512] != b"\x55\xaa":
            fail("missing MBR signature")
        entry = struct.unpack_from("<B3sB3sII", mbr, 0x1BE)
        if (entry[0], entry[2], entry[4], entry[5]) != \
                (0x80, PARTITION_TYPE, PARTITION_LBA, PARTITION_BLOCKS):
            fail("partition 1 is not the expected active FAT32 extent")
        if args.ufs_root is None:
            if any(mbr[0x1CE:0x1FE]):
                fail("unexpected additional MBR partition")
        else:
            root = struct.unpack_from("<B3sB3sII", mbr, 0x1CE)
            blocks = args.ufs_root.stat().st_size // SECTOR
            if (root[2], root[4], root[5]) != (0xA5, ROOT_LBA, blocks):
                fail("partition 2 is not the expected UFS root extent")
            if any(mbr[0x1DE:0x1FE]):
                fail("unexpected additional MBR partition")
            stream.seek(ROOT_LBA * SECTOR)
            if stream.read(blocks * SECTOR) != args.ufs_root.read_bytes():
                fail("partition 2 differs from the UFS root image")
        stream.seek(PARTITION_LBA * SECTOR)
        bpb = stream.read(SECTOR)
        # FAT32 has no fixed root directory and no 16-bit table size; its
        # table size is the 32-bit field, and the type string says FAT32.
        if struct.unpack_from("<H", bpb, 11)[0] != SECTOR or \
                struct.unpack_from("<H", bpb, 17)[0] != 0 or \
                struct.unpack_from("<H", bpb, 22)[0] != 0 or \
                struct.unpack_from("<I", bpb, 36)[0] == 0 or \
                bpb[82:90] != b"FAT32   " or bpb[510:512] != b"\x55\xaa":
            fail("partition 1 is not FAT32")

    same_file(args.image, "vmunix", args.kernel)
    if args.arch_image is not None:
        same_file(args.image, "rootfs.img", args.arch_image)
    same_file(args.image, "data.img", args.data_image)
    same_file(args.image, "swapfile", args.swapfile)
    same_file(args.image, "config.txt", args.config)
    for name in ("start4.elf", "fixup4.dat", "bcm2711-rpi-4-b.dtb",
                 "overlays/disable-bt.dtbo", "LICENCE.broadcom"):
        if not extract(args.image, name):
            fail(f"/{name} is missing or empty")
    kernel = extract(args.image, "vmunix")
    if len(kernel) < 64 or kernel[56:60] != b"ARM\x64":
        fail("vmunix has no Linux arm64 Image header")
    if args.arch_image is None:
        print("Raspberry Pi 4 image check: PASS")
        return
    with tempfile.TemporaryDirectory(prefix="zedbsd-rpi4-root-check-") as work:
        inner = Path(work) / "aarch64.img"
        inner.write_bytes(extract(args.image, "rootfs.img"))
        checker = Path(__file__).resolve().parents[3] / "tools/build/check-arch-overlay-image.py"
        subprocess.run(["python3", str(checker), "--profile", "aarch64",
                        "--image", str(inner)], check=True)
    print("Raspberry Pi 4 image check: PASS")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--arch-image", type=Path)
    parser.add_argument("--ufs-root", type=Path)
    parser.add_argument("--data-image", type=Path, required=True)
    parser.add_argument("--swapfile", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("image", type=Path)
    check(parser.parse_args())


if __name__ == "__main__":
    main()

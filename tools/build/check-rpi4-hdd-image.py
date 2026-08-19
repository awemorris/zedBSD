#!/usr/bin/env python3
"""Validate the Raspberry Pi 4 MBR/FAT16 image and boot payloads."""
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
IMAGE_BLOCKS = 524288


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
    if size != IMAGE_BLOCKS * SECTOR:
        fail("unexpected image size")
    with args.image.open("rb") as stream:
        mbr = stream.read(SECTOR)
        if mbr[510:512] != b"\x55\xaa":
            fail("missing MBR signature")
        entry = struct.unpack_from("<B3sB3sII", mbr, 0x1BE)
        if (entry[0], entry[2], entry[4], entry[5]) != \
                (0x80, 0x06, PARTITION_LBA, PARTITION_BLOCKS):
            fail("partition 1 is not the expected active FAT16 extent")
        if any(mbr[0x1CE:0x1FE]):
            fail("unexpected additional MBR partition")
        stream.seek(PARTITION_LBA * SECTOR)
        bpb = stream.read(SECTOR)
        if struct.unpack_from("<H", bpb, 11)[0] != SECTOR or \
                struct.unpack_from("<H", bpb, 22)[0] == 0:
            fail("partition 1 is not FAT16")

    same_file(args.image, "VMUNIX.A64", args.kernel)
    same_file(args.image, "rootfs.img", args.arch_image)
    same_file(args.image, "data.img", args.data_image)
    same_file(args.image, "swapfile", args.swapfile)
    same_file(args.image, "config.txt", args.config)
    for name in ("start4.elf", "fixup4.dat", "bcm2711-rpi-4-b.dtb",
                 "overlays/disable-bt.dtbo", "LICENCE.broadcom"):
        if not extract(args.image, name):
            fail(f"/{name} is missing or empty")
    kernel = extract(args.image, "VMUNIX.A64")
    if len(kernel) < 64 or kernel[56:60] != b"ARM\x64":
        fail("VMUNIX.A64 has no Linux arm64 Image header")
    with tempfile.TemporaryDirectory(prefix="zedbsd-rpi4-root-check-") as work:
        inner = Path(work) / "aarch64.img"
        inner.write_bytes(extract(args.image, "rootfs.img"))
        checker = Path(__file__).with_name("check-arch-overlay-image.py")
        subprocess.run(["python3", str(checker), "--profile", "aarch64",
                        "--image", str(inner)], check=True)
    print("Raspberry Pi 4 image check: PASS")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--arch-image", type=Path, required=True)
    parser.add_argument("--data-image", type=Path, required=True)
    parser.add_argument("--swapfile", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("image", type=Path)
    check(parser.parse_args())


if __name__ == "__main__":
    main()

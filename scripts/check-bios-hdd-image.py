#!/usr/bin/env python3
"""Validate the native zedBSD MBR/ZBL2/FAT16 disk layout."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import hashlib
import re
import struct
import subprocess
import tempfile
from pathlib import Path

MACHINES = {"pcat": 1, "pc98": 2}
FAT16_TYPES = {0x04, 0x06, 0x0E}
PC98_SECTORS = 17


def fail(message: str) -> None:
    raise SystemExit("bios image check: " + message)


def pc98_chs(lba: int, heads: int) -> bytes:
    cylinder, remainder = divmod(lba, heads * PC98_SECTORS)
    head, sector = divmod(remainder, PC98_SECTORS)
    return bytes((sector, head)) + struct.pack("<H", cylinder)


def check(args: argparse.Namespace) -> None:
    bin_files = []
    seen = set()
    for specification in args.bin_file:
        if "=" not in specification:
            fail("--bin-file requires NAME=SOURCE")
        name, source_text = specification.split("=", 1)
        if not re.fullmatch(r"[a-z0-9_]{1,8}(?:\.[a-z0-9_]{1,3})?", name):
            fail(f"invalid FAT16 /bin name: {name}")
        if name in seen:
            fail(f"duplicate /bin name: {name}")
        seen.add(name)
        bin_files.append((f"bin/{name}", Path(source_text), f"/bin/{name}"))
    image = args.image
    if (args.arch_profile is None) != (args.arch_image is None):
        fail("--arch-profile and --arch-image must be used together")
    size = image.stat().st_size
    if size == 0 or size % 512:
        fail("image size is not a positive sector multiple")
    with image.open("rb") as stream:
        mbr = stream.read(512)
        if mbr[510:512] != b"\x55\xaa":
            fail("missing MBR signature")
        entries = []
        for index in range(4):
            raw = mbr[0x1BE + index * 16:0x1CE + index * 16]
            status, ptype, start, blocks = raw[0], raw[4], \
                struct.unpack_from("<I", raw, 8)[0], \
                struct.unpack_from("<I", raw, 12)[0]
            if status not in (0, 0x80):
                fail(f"invalid partition status in entry {index + 1}")
            entries.append((status, ptype, start, blocks))
        active = [item for item in enumerate(entries, 1) if item[1][0] == 0x80]
        if len(active) != 1:
            fail("image must have exactly one active primary partition")
        index, (_, ptype, start, blocks) = active[0]
        if ptype not in FAT16_TYPES:
            fail("active partition is not FAT16")
        if start != 2048:
            fail("active partition does not start at LBA 2048")
        if blocks == 0 or start + blocks > size // 512:
            fail("active partition lies outside the image")

        stage2_lba = 1 if args.machine == "pcat" else 2
        if args.machine == "pc98":
            heads = 4 if size <= 20 * 1024 * 1024 else 8
            stream.seek(512)
            table = stream.read(512)
            expected = bytearray(32)
            expected[0:2] = b"\xa1\x91"
            expected[4:8] = pc98_chs(start, heads)
            expected[8:12] = pc98_chs(start, heads)
            expected[12:16] = pc98_chs(size // 512 - 1, heads)
            expected[16:32] = b"BOOT".ljust(16, b" ")
            if table[:32] != expected or any(table[32:]):
                fail(f"invalid PC-98 H={heads}/S=17 partition mirror")
        stream.seek(stage2_lba * 512)
        first = stream.read(512)
        if len(first) != 512:
            fail("missing Stage 2")
        magic, version, header_size, image_size = struct.unpack_from(
            "<IHHI", first, 0)
        sectors, machine, entry = struct.unpack_from("<HHI", first, 12)
        if (magic, version, header_size) != (0x324C425A, 1, 32):
            fail("invalid ZBL2 header")
        if machine != MACHINES[args.machine]:
            fail("ZBL2 machine mismatch")
        if not 1 <= sectors <= 127 or stage2_lba + sectors > 2048:
            fail("invalid ZBL2 sector count")
        if not 32 <= entry < image_size <= sectors * 512:
            fail("invalid ZBL2 size or entry")
        stream.seek(stage2_lba * 512)
        stage2 = stream.read(sectors * 512)
        total = sum(struct.unpack_from("<I", stage2, offset)[0]
                    for offset in range(0, len(stage2), 4)) & 0xFFFFFFFF
        if total:
            fail("ZBL2 checksum mismatch")
        stream.seek((stage2_lba + sectors) * 512)
        gap = stream.read((2048 - stage2_lba - sectors) * 512)
        if any(gap):
            fail("reserved pre-partition gap is not zero")
        stream.seek(start * 512)
        bpb = stream.read(512)
        if struct.unpack_from("<H", bpb, 11)[0] != 512:
            fail("FAT bytes/sector is not 512")
        if struct.unpack_from("<H", bpb, 22)[0] == 0:
            fail("volume is not FAT12/16")

    expected_files = (("VMUNIX", args.kernel, "VMUNIX"),
                      ("bin/noct", args.noct, "/bin/noct"),
                      ("apps/holoris.nct", args.holoris,
                       "/apps/holoris.nct")) + tuple(bin_files)
    with tempfile.TemporaryDirectory() as directory:
        for image_name, source, label in expected_files:
            if source is None:
                continue
            extracted = Path(directory) / image_name.replace("/", "-")
            subprocess.run(["mcopy", "-n", "-i",
                            f"{image}@@{start * 512}",
                            f"::{image_name}", str(extracted)], check=True,
                           stdout=subprocess.DEVNULL)
            if hashlib.sha256(extracted.read_bytes()).digest() != \
                    hashlib.sha256(source.read_bytes()).digest():
                fail(f"{label} content differs from the input file")
        if args.arch_image is not None:
            extracted = Path(directory) / f"{args.arch_profile}.img"
            subprocess.run(["mcopy", "-n", "-i",
                            f"{image}@@{start * 512}",
                            f"::arch/{args.arch_profile}.img", str(extracted)],
                           check=True, stdout=subprocess.DEVNULL)
            if hashlib.sha256(extracted.read_bytes()).digest() != \
                    hashlib.sha256(args.arch_image.read_bytes()).digest():
                fail("architecture image content differs from the input")
            checker = Path(__file__).with_name("check-arch-overlay-image.py")
            subprocess.run(["python3", str(checker), "--profile",
                            args.arch_profile, "--image", str(extracted)], check=True)
            direct_shell = subprocess.run(
                ["mcopy", "-n", "-i", f"{image}@@{start * 512}",
                 "::bin/sh", str(Path(directory) / "outer-sh")],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if direct_shell.returncode == 0:
                fail("architecture-specific /bin/sh leaked into the outer FAT")
    print(f"BIOS image check: PASS ({args.machine}, partition {index}, "
          f"Stage 2 {sectors} sectors)")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--machine", choices=sorted(MACHINES), required=True)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--noct", type=Path)
    parser.add_argument("--holoris", type=Path)
    parser.add_argument("--arch-profile", choices=("i386", "amd64", "aarch64"))
    parser.add_argument("--arch-image", type=Path)
    parser.add_argument("--bin-file", action="append", default=[])
    parser.add_argument("image", type=Path)
    check(parser.parse_args())


if __name__ == "__main__":
    main()

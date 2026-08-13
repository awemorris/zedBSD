#!/usr/bin/env python3
"""Create a native zedBSD BIOS MBR/ZBL2/FAT16 disk image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path


SECTOR_SIZE = 512
PC98_SECTORS = 17


def run(*arguments: str) -> None:
    subprocess.run(arguments, check=True)


def pc98_chs(lba: int, heads: int) -> bytes:
    cylinder, remainder = divmod(lba, heads * PC98_SECTORS)
    head, sector = divmod(remainder, PC98_SECTORS)
    if cylinder > 0xFFFF:
        raise SystemExit("partition exceeds the PC-98 CHS range")
    return bytes((sector, head)) + struct.pack("<H", cylinder)


def create(args: argparse.Namespace) -> None:
    for path in (args.stage1, args.stage2, args.kernel):
        if not path.is_file():
            raise SystemExit(f"missing input: {path}")
    for path in (args.shell, args.noct, args.holoris):
        if path is not None and not path.is_file():
            raise SystemExit(f"missing input: {path}")
    if args.output.exists() and not args.force:
        raise SystemExit(f"output exists (use --force): {args.output}")
    total_sectors = args.size_mib * 2048
    start = 2048
    blocks = total_sectors - start
    stage2_lba = 1 if args.machine == "pcat" else 2
    if blocks <= 0:
        raise SystemExit("image is too small for the LBA 2048 partition")
    stage1 = bytearray(args.stage1.read_bytes())
    stage2 = args.stage2.read_bytes()
    if len(stage1) != 512 or stage1[510:512] != b"\x55\xaa":
        raise SystemExit("invalid Stage 1")
    if len(stage2) % 512 or stage2_lba + len(stage2) // 512 > start:
        raise SystemExit("invalid or oversized Stage 2")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=args.output.name + ".",
                                           dir=args.output.parent)
    os.close(fd)
    temporary = Path(temporary_name)
    try:
        with temporary.open("r+b") as stream:
            stream.truncate(total_sectors * 512)
            stage1[0x1BE:0x1CE] = struct.pack(
                "<BBBBBBBBII", 0x80, 0xFE, 0xFF, 0xFF,
                0x0E, 0xFE, 0xFF, 0xFF, start, blocks)
            stage1[510:512] = b"\x55\xaa"
            stream.seek(0)
            stream.write(stage1)
            if args.machine == "pc98":
                heads = 4 if args.size_mib <= 20 else 8
                entry = bytearray(32)
                entry[0] = 0xA1
                entry[1] = 0x91
                entry[4:8] = pc98_chs(start, heads)
                entry[8:12] = pc98_chs(start, heads)
                entry[12:16] = pc98_chs(total_sectors - 1, heads)
                entry[16:32] = b"BOOT".ljust(16, b" ")
                stream.seek(SECTOR_SIZE)
                stream.write(entry)
            stream.seek(stage2_lba * SECTOR_SIZE)
            stream.write(stage2)
        offset = start * 512
        run("mformat", "-i", f"{temporary}@@{offset}", "-v", "BOOT", "::")
        if args.fragment_kernel:
            with tempfile.TemporaryDirectory(prefix="zedbsd-fragment-") as work:
                hole = Path(work) / "hole.bin"
                blocker = Path(work) / "blocker.bin"
                hole.write_bytes(bytes(4096))
                blocker.write_bytes(bytes(128 * 1024))
                run("mcopy", "-i", f"{temporary}@@{offset}", str(hole),
                    "::HOLE.BIN")
                run("mcopy", "-i", f"{temporary}@@{offset}", str(blocker),
                    "::BLOCKER.BIN")
                run("mdel", "-i", f"{temporary}@@{offset}", "::HOLE.BIN")
                run("mcopy", "-i", f"{temporary}@@{offset}", str(args.kernel),
                    "::VMUNIX")
                run("mdel", "-i", f"{temporary}@@{offset}", "::BLOCKER.BIN")
        else:
            run("mcopy", "-i", f"{temporary}@@{offset}", str(args.kernel),
                "::VMUNIX")
        if args.shell or args.noct:
            run("mmd", "-i", f"{temporary}@@{offset}", "::/bin")
        if args.shell:
            run("mcopy", "-i", f"{temporary}@@{offset}", str(args.shell),
                "::/bin/sh")
        if args.noct:
            run("mcopy", "-i", f"{temporary}@@{offset}", str(args.noct),
                "::/bin/noct")
        if args.holoris:
            run("mmd", "-i", f"{temporary}@@{offset}", "::/apps")
            run("mcopy", "-i", f"{temporary}@@{offset}",
                str(args.holoris), "::/apps/holoris.nct")
        checker = Path(__file__).with_name("check-bios-hdd-image.py")
        run("python3", str(checker), "--machine", args.machine,
            "--kernel", str(args.kernel),
            *(["--noct", str(args.noct)] if args.noct else []),
            *(["--holoris", str(args.holoris)] if args.holoris else []),
            str(temporary))
        os.replace(temporary, args.output)
    finally:
        if temporary.exists():
            temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--machine", choices=("pcat", "pc98"), required=True)
    parser.add_argument("--stage1", type=Path, required=True)
    parser.add_argument("--stage2", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--shell", type=Path)
    parser.add_argument("--noct", type=Path)
    parser.add_argument("--holoris", type=Path)
    parser.add_argument("--size-mib", type=int, default=129)
    parser.add_argument("--fragment-kernel", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)
    create(parser.parse_args())


if __name__ == "__main__":
    main()

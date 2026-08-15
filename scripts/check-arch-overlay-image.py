#!/usr/bin/env python3
"""Validate an architecture-specific raw FAT16 userland overlay image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import subprocess
from pathlib import Path

from overlay_journal_format import JOURNAL_BYTES, validate_empty_active_slot


PROFILES = {
    "i386": ("ZEDI386", 1, 3),
    "amd64": ("ZEDAMD64", 2, 62),
    "aarch64": ("ZEDAARCH64", 2, 183),
}


def capture(*arguments: str) -> bytes:
    return subprocess.run(arguments, check=True, stdout=subprocess.PIPE).stdout


def extract(image: Path, fat_path: str) -> bytes:
    return capture("mcopy", "-i", str(image), f"::{fat_path}", "-")


def parse_files(specifications: list[str]) -> dict[str, Path]:
    result = {}
    for item in specifications:
        if "=" not in item:
            raise SystemExit("--file requires DESTINATION=SOURCE")
        destination, source = item.split("=", 1)
        if destination in result:
            raise SystemExit(f"duplicate manifest destination: {destination}")
        result[destination] = Path(source)
    return result


def fat_kind(boot: bytes) -> tuple[str, int]:
    if len(boot) < 512 or boot[510:512] != b"\x55\xaa":
        raise SystemExit("inner image has no FAT boot-sector signature")
    bps = struct.unpack_from("<H", boot, 11)[0]
    spc = boot[13]
    reserved, fats, roots = struct.unpack_from("<HBH", boot, 14)
    total16 = struct.unpack_from("<H", boot, 19)[0]
    fat16 = struct.unpack_from("<H", boot, 22)[0]
    total = total16 or struct.unpack_from("<I", boot, 32)[0]
    if bps != 512 or not spc or not fats or not fat16:
        raise SystemExit("inner image has an invalid FAT12/16 BPB")
    root_sectors = (roots * 32 + bps - 1) // bps
    data = total - reserved - fats * fat16 - root_sectors
    clusters = data // spc
    return ("FAT12" if clusters < 4085 else "FAT16" if clusters < 65525 else "FAT32",
            clusters)


def check(args: argparse.Namespace) -> None:
    files = parse_files(args.file)
    if args.image.stat().st_size != args.size_mib * 1024 * 1024:
        raise SystemExit("inner image has the wrong byte size")
    if args.image.stat().st_size % 512:
        raise SystemExit("inner image is not sector aligned")
    with args.image.open("rb") as stream:
        boot = stream.read(512)
    kind, clusters = fat_kind(boot)
    if kind != "FAT16":
        raise SystemExit(f"inner image must be FAT16, got {kind} ({clusters} clusters)")
    label, expected_class, expected_machine = PROFILES[args.profile]
    if boot[43:54].decode("ascii", "replace").rstrip() != label:
        raise SystemExit("inner image has the wrong volume label")
    if extract(args.image, "/lib/arch.id") != (args.profile + "\n").encode():
        raise SystemExit("wrong /lib/arch.id")
    shell = extract(args.image, "/bin/sh")
    if not shell.startswith(b"\x7fELF"):
        raise SystemExit("/bin/sh is missing or is not ELF")
    if shell[4] != expected_class or struct.unpack_from("<H", shell, 18)[0] != expected_machine:
        raise SystemExit("/bin/sh has the wrong profile ABI")
    for base in ("bin", "lib"):
        active = extract(args.image, f"/zedovl/{base}0.log")
        inactive = extract(args.image, f"/zedovl/{base}1.log")
        validate_empty_active_slot(active, base)
        if len(inactive) != JOURNAL_BYTES or any(inactive):
            raise SystemExit(f"{base} inactive journal is not 128 KiB of zeroes")
    for destination, source in files.items():
        actual = extract(args.image, destination)
        expected = source.read_bytes()
        if hashlib.sha256(actual).digest() != hashlib.sha256(expected).digest():
            raise SystemExit(f"manifest hash mismatch: {destination}")
        if destination.startswith("/bin/") and actual.startswith(b"\x7fELF"):
            elf_class = actual[4]
            machine = struct.unpack_from("<H", actual, 18)[0]
            if elf_class != expected_class or machine != expected_machine:
                raise SystemExit(f"wrong ELF ABI for {destination}: class={elf_class} machine={machine}")
    listing = capture("mdir", "-i", str(args.image), "::").decode(
        "utf-8", "replace")
    match = re.search(r"([0-9 ,]+) bytes free", listing)
    if not match:
        raise SystemExit("could not determine FAT free space")
    free_bytes = int(match.group(1).replace(",", "").replace(" ", ""))
    if free_bytes < args.min_free_bytes:
        raise SystemExit(f"inner image free reserve is too small: {free_bytes}")
    print(f"{args.image}: {args.profile} FAT16 profile OK, {free_bytes} bytes free")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--size-mib", type=int, default=16)
    parser.add_argument("--min-free-bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--file", action="append", default=[])
    check(parser.parse_args())


if __name__ == "__main__":
    main()

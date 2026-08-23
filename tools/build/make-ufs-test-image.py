#!/usr/bin/env python3
"""Create UFS1/UFS2 filesystem fixtures used by public host tests."""

import argparse
from pathlib import Path
import struct

import ufs1_format
import ufs2_format


def append_region(payload, magic, size_mib, fields):
    size_bytes = size_mib * 1024 * 1024
    locator = bytearray(512)
    locator[:4] = magic
    struct.pack_into(fields[0], locator, 4, *fields[1])
    digest_offset = fields[2]
    digest = 2166136261
    for value in locator[:digest_offset]:
        digest = ((digest ^ value) * 16777619) & 0xffffffff
    struct.pack_into("<I", locator, digest_offset, digest)
    payload.extend(locator)
    payload.extend(b"\0" * size_bytes)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--format", choices=("ufs1", "ufs2"), required=True)
    parser.add_argument("--size-mib", type=int, default=16)
    parser.add_argument("--root", type=Path)
    parser.add_argument("--cylinder-groups", type=int, default=1)
    parser.add_argument("--journal-mib", type=int, default=0)
    parser.add_argument("--snapshot-mib", type=int, default=0)
    args = parser.parse_args()
    if args.size_mib < 4 or args.cylinder_groups < 1:
        parser.error("invalid image size or cylinder-group count")
    if args.format == "ufs1" and (args.journal_mib or args.snapshot_mib):
        parser.error("journal/snapshot regions require UFS2")

    create = ufs1_format.create if args.format == "ufs1" else ufs2_format.create
    payload = bytearray(create(args.size_mib * 1024 * 1024, args.root,
                               args.cylinder_groups))
    if args.journal_mib:
        sectors = args.journal_mib * 1024 * 1024 // 512
        append_region(payload, b"ZUJ1", args.journal_mib,
                      ("<IIQ", (1, sectors, len(payload) // 512), 24))
    if args.snapshot_mib:
        sectors = args.snapshot_mib * 1024 * 1024 // 512
        locator_lba = len(payload) // 512
        volume_sectors = args.size_mib * 1024 * 1024 // 512
        append_region(payload, b"ZSL1", args.snapshot_mib,
                      ("<IIIQQ", (1, sectors, 0, locator_lba,
                                   volume_sectors), 32))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(payload)


if __name__ == "__main__":
    main()

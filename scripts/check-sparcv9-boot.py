#!/usr/bin/env python3
"""Validate raw zedBSD SPARC V9 OpenBoot stages."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
from pathlib import Path

SECTOR_SIZE = 512
STAGE1_LIMIT = 15 * SECTOR_SIZE
STAGE2_PAYLOAD_LBA = 33
FAT_LBA = 4096
STAGE2_LOAD_LIMIT = 0x00100000


def fail(message: str) -> None:
    raise SystemExit("SPARC V9 boot check: " + message)


def check(args: argparse.Namespace) -> None:
    stage1 = args.stage1.read_bytes()
    stage2 = args.stage2.read_bytes()
    if not stage1 or len(stage1) > STAGE1_LIMIT:
        fail("stage1 exceeds the 15-sector OpenBoot load area")
    if not stage2 or len(stage2) > STAGE2_LOAD_LIMIT:
        fail("stage2 exceeds its 1 MiB claim")
    sectors = (len(stage2) + SECTOR_SIZE - 1) // SECTOR_SIZE
    if STAGE2_PAYLOAD_LBA + sectors > FAT_LBA:
        fail("stage2 overlaps the FAT slice")
    print(
        "SPARC V9 boot check: PASS "
        f"(stage1={len(stage1)}, stage2={len(stage2)})"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage1", type=Path, required=True)
    parser.add_argument("--stage2", type=Path, required=True)
    check(parser.parse_args())


if __name__ == "__main__":
    main()

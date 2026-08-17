#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import argparse
import pathlib

from ufs2_format import create

parser = argparse.ArgumentParser(description='create a canonical zedBSD UFS2 image')
parser.add_argument('output')
parser.add_argument('--size-mib', type=int, default=16)
parser.add_argument('--root')
parser.add_argument('--cylinder-groups', type=int, default=1)
parser.add_argument('--journal-mib', type=int, default=0,
                    help='append a zedBSD redo journal outside the UFS2 extent')
parser.add_argument('--snapshot-mib', type=int, default=0,
                    help='append a persistent zedBSD COW snapshot region')
args = parser.parse_args()
payload = bytearray(create(
    args.size_mib * 1024 * 1024, args.root, args.cylinder_groups))
if args.journal_mib:
    if args.journal_mib < 1:
        parser.error('--journal-mib must be positive')
    import struct
    journal_bytes = args.journal_mib * 1024 * 1024
    locator = bytearray(512)
    locator[0:4] = b'ZUJ1'
    struct.pack_into('<IIQ', locator, 4, 1, journal_bytes // 512,
                     len(payload) // 512)
    digest = 2166136261
    for value in locator[:24]:
        digest = ((digest ^ value) * 16777619) & 0xffffffff
    struct.pack_into('<I', locator, 24, digest)
    payload.extend(locator)
    payload.extend(b'\0' * journal_bytes)
if args.snapshot_mib:
    if args.snapshot_mib < 1:
        parser.error('--snapshot-mib must be positive')
    import struct
    snapshot_bytes = args.snapshot_mib * 1024 * 1024
    locator_lba = len(payload) // 512
    volume_sectors = args.size_mib * 1024 * 1024 // 512
    locator = bytearray(512)
    locator[0:4] = b'ZSL1'
    struct.pack_into('<IIIQQ', locator, 4, 1, snapshot_bytes // 512, 0,
                     locator_lba, volume_sectors)
    digest = 2166136261
    for value in locator[:32]:
        digest = ((digest ^ value) * 16777619) & 0xffffffff
    struct.pack_into('<I', locator, 32, digest)
    payload.extend(locator)
    payload.extend(b'\0' * snapshot_bytes)
pathlib.Path(args.output).write_bytes(payload)

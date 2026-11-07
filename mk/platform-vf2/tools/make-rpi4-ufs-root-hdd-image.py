#!/usr/bin/env python3
"""Add canonical UFS root and swap partitions to a Raspberry Pi 4 or 5 boot image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path

SECTOR = 512
ROOT_LBA = 264192


def run(*args: str) -> None:
    subprocess.run(args, check=True)


def build(args: argparse.Namespace) -> None:
    if not args.ufs_root.is_file() or args.ufs_root.stat().st_size % SECTOR:
        raise SystemExit('invalid UFS root image')
    if args.swap_image is not None and (not args.swap_image.is_file() or args.swap_image.stat().st_size % SECTOR):
        raise SystemExit('invalid swap image')
    if args.output.exists() and not args.force:
        raise SystemExit(f'output exists (use --force): {args.output}')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=args.output.name + '.', dir=args.output.parent)
    os.close(descriptor)
    temporary = Path(name)
    try:
        temporary.unlink()
        base = Path(__file__).with_name('make-rpi4-hdd-image.py')
        arch = [] if args.arch_image is None else ['--arch-image', str(args.arch_image)]
        cmdline = [] if args.cmdline is None else ['--cmdline', str(args.cmdline)]
        run('python3', str(base), '--force', '--kernel', str(args.kernel),
            *arch, *cmdline, '--config', str(args.config),
            '--firmware-dir', str(args.firmware_dir), '--board', args.board,
            str(temporary))
        root_blocks = args.ufs_root.stat().st_size // SECTOR
        if args.swap_image is not None:
            swap_blocks = args.swap_image.stat().st_size // SECTOR
            swap_lba = (ROOT_LBA + root_blocks + 2047) // 2048 * 2048
            required_size = (swap_lba + swap_blocks) * SECTOR
        else:
            swap_blocks = 0
            swap_lba = 0
            required_size = (ROOT_LBA + root_blocks) * SECTOR
        with temporary.open('r+b') as image:
            image.truncate(max(temporary.stat().st_size, required_size))
            # Partition 2: UFS root
            image.seek(0x1CE)
            image.write(struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', 0xA5,
                                    b'\xfe\xff\xff', ROOT_LBA, root_blocks))
            # Partition 3: Swap partition
            if args.swap_image is not None:
                image.seek(0x1DE)
                image.write(struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', 0x82,
                                        b'\xfe\xff\xff', swap_lba, swap_blocks))
            image.seek(ROOT_LBA * SECTOR)
            with args.ufs_root.open('rb') as src:
                while chunk := src.read(1024 * 1024):
                    image.write(chunk)
            if args.swap_image is not None:
                image.seek(swap_lba * SECTOR)
                with args.swap_image.open('rb') as src:
                    while chunk := src.read(1024 * 1024):
                        image.write(chunk)
        checker = Path(__file__).resolve().parents[3] / 'tools/build/check-ufs-image.py'
        if not checker.is_file():
            checker = Path(__file__).resolve().parents[3] / 'mk/old-tools/build/check-ufs-image.py'
        run('python3', str(checker), str(args.ufs_root))
        hdd_checker = Path(__file__).with_name('check-rpi4-hdd-image.py')
        swap_check = [] if args.swap_image is None else ['--swap-image', str(args.swap_image)]
        run('python3', str(hdd_checker), *cmdline, '--kernel', str(args.kernel),
            '--ufs-root', str(args.ufs_root), *swap_check,
            '--config', str(args.config), '--board', args.board, str(temporary))
        os.replace(temporary, args.output)
    finally:
        if temporary.exists():
            temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--arch-image', type=Path)
    parser.add_argument('--data-image', type=Path)
    parser.add_argument('--swapfile', '--swap-image', dest='swap_image', type=Path)
    parser.add_argument('--ufs-root', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--cmdline', type=Path)
    parser.add_argument('--firmware-dir', type=Path, required=True)
    parser.add_argument('--board', choices=('rpi4', 'rpi5'), default='rpi4')
    parser.add_argument('--force', action='store_true')
    parser.add_argument('output', type=Path)
    build(parser.parse_args())


if __name__ == '__main__':
    main()

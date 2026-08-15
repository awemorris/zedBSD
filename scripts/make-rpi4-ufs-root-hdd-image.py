#!/usr/bin/env python3
"""Add the canonical UFS1 root partition to a Raspberry Pi 4 boot image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path

SECTOR=512
ROOT_LBA=264192


def run(*args: str) -> None:
    subprocess.run(args,check=True)


def build(args: argparse.Namespace) -> None:
    if not args.ufs_root.is_file() or args.ufs_root.stat().st_size%SECTOR:
        raise SystemExit('invalid UFS1 root image')
    if args.output.exists() and not args.force:
        raise SystemExit(f'output exists (use --force): {args.output}')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    descriptor,name=tempfile.mkstemp(prefix=args.output.name+'.',dir=args.output.parent)
    os.close(descriptor); temporary=Path(name)
    try:
        temporary.unlink()
        base=Path(__file__).with_name('make-rpi4-hdd-image.py')
        run('python3',str(base),'--force','--kernel',str(args.kernel),
            '--arch-image',str(args.arch_image),'--config',str(args.config),
            '--firmware-dir',str(args.firmware_dir),str(temporary))
        blocks=args.ufs_root.stat().st_size//SECTOR
        if ROOT_LBA+blocks>temporary.stat().st_size//SECTOR:
            raise SystemExit('UFS1 root exceeds Raspberry Pi image')
        with temporary.open('r+b') as image:
            image.seek(0x1CE)
            image.write(struct.pack('<B3sB3sII',0,b'\xfe\xff\xff',0xA5,
                                    b'\xfe\xff\xff',ROOT_LBA,blocks))
            image.seek(ROOT_LBA*SECTOR)
            image.write(args.ufs_root.read_bytes())
        checker=Path(__file__).with_name('check-ufs1-image.py')
        run('python3',str(checker),str(args.ufs_root))
        os.replace(temporary,args.output)
    finally:
        if temporary.exists(): temporary.unlink()


def main() -> None:
    parser=argparse.ArgumentParser()
    parser.add_argument('--kernel',type=Path,required=True)
    parser.add_argument('--arch-image',type=Path,required=True)
    parser.add_argument('--ufs-root',type=Path,required=True)
    parser.add_argument('--config',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--force',action='store_true')
    parser.add_argument('output',type=Path)
    build(parser.parse_args())


if __name__=='__main__': main()

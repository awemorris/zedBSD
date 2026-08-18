#!/usr/bin/env python3
"""Validate an architecture-specific zedBSD UFS1 root image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

from check_ufs1_import import load_checker

PROFILES={"i386":(1,3),"amd64":(2,62),"aarch64":(2,183)}


def parse_files(items):
    result={}
    for item in items:
        destination,source=item.split("=",1); result[destination]=Path(source)
    return result


def check(args):
    checker=load_checker(); checker.check(args.image)
    fs=checker.UFS1(args.image.read_bytes()); files=parse_files(args.file)
    if fs.read_file(fs.lookup('/lib/arch.id'))!=(args.profile+'\n').encode():
        raise SystemExit('wrong /lib/arch.id')
    if fs.read_file(fs.lookup('/etc/zedbsd-root')) != \
            b'zedBSD ufs1 root v1\n':
        raise SystemExit('wrong /etc/zedbsd-root marker')
    expected_class,expected_machine=PROFILES[args.profile]
    shell_ino=fs.lookup('/bin/sh')
    shell=fs.read_file(shell_ino)
    if not shell.startswith(b'\x7fELF') or shell[4]!=expected_class or struct.unpack_from('<H',shell,18)[0]!=expected_machine:
        raise SystemExit('/bin/sh has the wrong ELF ABI')
    if fs.u16(fs.inode(shell_ino),0)&0o111==0:
        raise SystemExit('/bin/sh is not executable')
    for destination,source in files.items():
        ino=fs.lookup(destination); actual=fs.read_file(ino); expected=source.read_bytes()
        if hashlib.sha256(actual).digest()!=hashlib.sha256(expected).digest():
            raise SystemExit(f'manifest hash mismatch: {destination}')
        if source.stat().st_mode&0o111 and fs.u16(fs.inode(ino),0)&0o111==0:
            raise SystemExit(f'executable mode lost: {destination}')
    for item in args.mode:
        destination, mode_text = item.split('=', 1)
        actual = fs.u16(fs.inode(fs.lookup(destination)), 0) & 0o7777
        if actual != int(mode_text, 8):
            raise SystemExit(f'mode mismatch: {destination}')
    print(f'{args.image}: {args.profile} UFS1 root OK')


def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--profile',choices=PROFILES,required=True)
    parser.add_argument('--image',type=Path,required=True); parser.add_argument('--file',action='append',default=[])
    parser.add_argument('--mode',action='append',default=[])
    check(parser.parse_args())


if __name__=='__main__': main()

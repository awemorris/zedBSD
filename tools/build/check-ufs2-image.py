#!/usr/bin/env python3
"""Validate the canonical zedBSD UFS2 profile, including multiple CGs."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import importlib.util
import pathlib
import sys

_base_path = pathlib.Path(__file__).with_name('check-ufs1-image.py')
_spec = importlib.util.spec_from_file_location('_ufs1_checker', _base_path)
_base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_base)


class UFS2(_base.UFS1):
    def __init__(self, data: bytes):
        self.data = data
        self.sb = memoryview(data)[65536:65536 + 8192]
        if len(self.sb) < 1376 or self.u32(self.sb, 1372) != 0x19540119:
            raise ValueError('bad UFS2 magic')
        self.bsize = self.u32(self.sb, 48)
        self.fsize = self.u32(self.sb, 52)
        self.frag = self.u32(self.sb, 56)
        self.nindir = self.u32(self.sb, 116)
        self.inopb = self.u32(self.sb, 120)
        self.ipg = self.u32(self.sb, 184)
        self.fpg = self.u32(self.sb, 188)
        self.iblk = self.u32(self.sb, 16)
        self.dblk = self.u32(self.sb, 20)
        self.cblk = self.u32(self.sb, 12)
        self.sblk = self.u32(self.sb, 8)
        self.cgoffset = self.u32(self.sb, 24)
        self.cgmask = self.u32(self.sb, 28)
        self.fragments = self.u64(self.sb, 1080)
        self.ncg = self.u32(self.sb, 44)
        self.cgsize = self.u32(self.sb, 160)
        if self.u64(self.sb, 1000) != 65536:
            raise ValueError('bad UFS2 superblock location')
        if (self.bsize, self.fsize, self.frag, self.nindir,
                self.inopb) != (8192, 1024, 8, 1024, 32):
            raise ValueError('unsupported UFS2 profile')
        if self.ncg < 1 or self.fpg < self.dblk:
            raise ValueError('bad cylinder group geometry')
        if self.fragments * self.fsize > len(data):
            raise ValueError('truncated image')
        self.cgs = [self._read_cg(index) for index in range(self.ncg)]

    def inode(self, number):
        if number < 0 or number >= self.ncg * self.ipg:
            raise ValueError('inode out of range')
        cg, index = divmod(number, self.ipg)
        offset = ((self.cgstart(cg) + self.iblk) * self.fsize +
                  index * 256)
        raw = memoryview(self.data)[offset:offset + 256]
        if len(raw) != 256:
            raise ValueError('truncated inode table')
        return raw

    def _indirect_values(self, fragment, depth, owned):
        if fragment == 0:
            return []
        if owned is not None:
            self._own(fragment, owned, 'indirect')
        raw = memoryview(self.data)[
            fragment * self.fsize:fragment * self.fsize + self.bsize]
        if len(raw) != self.bsize:
            raise ValueError('truncated indirect block')
        values = []
        for index in range(self.nindir):
            pointer = self.u64(raw, index * 8)
            if not pointer:
                continue
            if depth == 1:
                values.append(pointer)
            else:
                values.extend(self._indirect_values(
                    pointer, depth - 1, owned))
        return values

    def blocks(self, number, owned=None):
        raw = self.inode(number)
        result = []
        for index in range(_base.NDADDR):
            pointer = self.u64(raw, 112 + index * 8)
            if pointer:
                result.append(pointer)
        for depth in range(1, _base.NIADDR + 1):
            pointer = self.u64(raw, 208 + (depth - 1) * 8)
            result.extend(self._indirect_values(pointer, depth, owned))
        return result

    def read_file(self, number):
        raw = self.inode(number)
        size = self.u64(raw, 16)
        result = bytearray()
        for fragment in self.blocks(number):
            start = fragment * self.fsize
            result.extend(self.data[start:start + self.bsize])
            if len(result) >= size:
                break
        if len(result) < size:
            raise ValueError(
                f'inode {number}: sparse/truncated file unsupported by checker')
        return bytes(result[:size])

    def _check_global_summaries(self, totals):
        actual = tuple(self.u64(self.sb, offset)
                       for offset in (1008, 1016, 1024, 1032))
        if actual != totals:
            raise ValueError('superblock summary counters mismatch')


def check(path):
    fs = UFS2(pathlib.Path(path).read_bytes())
    fs.validate()
    marker = fs.read_file(fs.lookup('/etc/zedbsd-root'))
    if marker != b'zedBSD ufs2 root v1\n':
        raise ValueError('bad root marker')
    print(f'{path}: canonical zedBSD UFS2 OK')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('image')
    args = parser.parse_args()
    try:
        check(args.image)
    except Exception as error:
        print(f'{args.image}: {error}', file=sys.stderr)
        sys.exit(1)

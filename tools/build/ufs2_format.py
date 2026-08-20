#!/usr/bin/env python3
"""Deterministic canonical FreeBSD-style UFS2 image writer for zedBSD."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import pathlib
import secrets
import struct

from ufs1_format import Image as TreeImage, IFDIR, p32, p64

UFS2_MAGIC = 0x19540119


class Image(TreeImage):
    """UFS2 disk codec over the format-neutral tree and CG builder."""
    super_offset = 65536
    inode_size = 256
    nindir = 1024
    ipg = 256
    sblk = 64
    cblk = 72
    iblk = 80
    dblk = 144
    pointer_size = 8
    format_name = 'UFS2'

    def _super(self):
        sb = memoryview(self.data)[
            self.super_offset:self.super_offset + self.super_size]
        dsize = sum(max(0, self.cg_ndblk(cg) - self.dblk)
                    for cg in range(self.ncg))
        values = [
            (8, self.sblk), (12, self.cblk), (16, self.iblk),
            (20, self.dblk), (24, 0), (28, 0),
            (44, self.ncg), (48, self.block_size),
            (52, self.frag_size), (56, self.frags_per_block),
            (80, 13), (84, 10), (96, 3), (100, 1),
            (104, 1376), (116, self.nindir),
            (120, self.block_size // self.inode_size),
            (152, 0), (156, 0), (160, self.cgsize),
            (184, self.ipg), (188, self.fpg),
            (1312, 0), (1320, 120), (1372, UFS2_MAGIC),
        ]
        for offset, value in values:
            p32(sb, offset, value)
        sb[144:152] = secrets.token_bytes(8)
        p64(sb, 1000, self.super_offset)
        p64(sb, 1080, self.fragments)
        p64(sb, 1088, dsize)
        p64(sb, 1096, self.dblk)
        p64(sb, 1328, 0x7fffffffffffffff)
        sb[209] = 1

    def _write_global_summaries(self, totals):
        sb = memoryview(self.data)[
            self.super_offset:self.super_offset + self.super_size]
        for offset, value in zip((1008, 1016, 1024, 1032), totals):
            p64(sb, offset, value)

    def _indirect(self, blocks, depth):
        capacity = self.nindir ** depth
        if len(blocks) > capacity:
            raise ValueError('too many indirect blocks')
        pointers = []
        metadata = 0
        if depth == 1:
            pointers = list(blocks)
        else:
            span = self.nindir ** (depth - 1)
            for pos in range(0, len(blocks), span):
                pointer, count = self._indirect(
                    blocks[pos:pos + span], depth - 1)
                pointers.append(pointer)
                metadata += count
        raw = bytearray(self.block_size)
        for index, pointer in enumerate(pointers):
            p64(raw, index * 8, pointer)
        return self._alloc_block(raw), metadata + 1

    def _inode(self, ino, mode, nlink, size, blocks, uid=0, gid=0):
        offset = self.inode_offset(ino)
        raw = memoryview(self.data)[offset:offset + self.inode_size]
        struct.pack_into('<HH', raw, 0, mode, nlink)
        p32(raw, 4, uid)
        p32(raw, 8, gid)
        p32(raw, 12, self.block_size)
        p64(raw, 16, size)
        direct = blocks[:12]
        remaining = blocks[12:]
        metadata = 0
        for index, block in enumerate(direct):
            p64(raw, 112 + index * 8, block)
        for depth in range(1, 4):
            capacity = self.nindir ** depth
            group = remaining[:capacity]
            if group:
                pointer, count = self._indirect(group, depth)
                p64(raw, 208 + (depth - 1) * 8, pointer)
                metadata += count
                remaining = remaining[len(group):]
        if remaining:
            raise ValueError('file exceeds UFS2 triple-indirect range')
        p64(raw, 24, (len(blocks) + metadata) *
            (self.block_size // 512))
        p32(raw, 80, ino)

    def _write_inline_symlink(self, ino, target):
        if len(target) > 120:
            raise ValueError('UFS2 inline symlink target too long')
        offset = self.inode_offset(ino)
        self.data[offset + 112:offset + 112 + len(target)] = target


def create(size, root=None, cylinder_groups=1):
    image = Image(size, cylinder_groups)
    if root:
        image.add_tree(pathlib.Path(root), 2)
    etc = image._child(2, 'etc', 4)
    if etc is None:
        etc = image.add_dir(2, 'etc')
    marker = image._child(etc, 'zedbsd-root')
    if marker is None:
        image.add_file(etc, 'zedbsd-root', b'zedBSD ufs2 root v1\n')
    return image.finish()

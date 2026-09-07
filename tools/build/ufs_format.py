#!/usr/bin/env python3
"""Canonical zedBSD UFS image writer with a single 64-bit disk codec."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
from __future__ import annotations
import os
import pathlib
import secrets
import struct

SECTOR = 512
FRAG = 1024
BLOCK = 8192
FRAGS = 8
SUPER = 65536
NINDIR = 1024
MAGIC = 0x19540119
CG_MAGIC = 0x090255
INODE_SIZE = 256
IPG = 256
SBLK = 64
CBLK = 72
IBLK = 80
DBLK = 144
IFDIR = 0o040000
IFREG = 0o100000
IFLNK = 0o120000
MAX_BYTES = 2147482624
JOURNAL_SECTORS = 256
SNAPSHOT_SECTORS = 2049


def p32(buffer, offset, value):
    struct.pack_into('<I', buffer, offset, value)


def p64(buffer, offset, value):
    struct.pack_into('<Q', buffer, offset, value)


def digest(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


class Image:
    """Build the tree, allocation maps and 256-byte inodes in one format."""
    frag_size = FRAG
    block_size = BLOCK
    frags_per_block = FRAGS
    super_offset = SUPER
    super_size = 8192
    nindir = NINDIR
    inode_size = INODE_SIZE
    ipg = IPG
    sblk = SBLK
    cblk = CBLK
    iblk = IBLK
    dblk = DBLK
    pointer_size = 8
    format_name = 'UFS'

    def __init__(self, size: int, cylinder_groups: int = 2, profile: str = "ordinary"):
        if size < 4 * 1024 * 1024 or size > MAX_BYTES or size % self.frag_size:
            raise ValueError(f'bad {self.format_name} size')
        if cylinder_groups < 2:
            raise ValueError('at least two cylinder groups are required')
        if profile not in ('ordinary', 'journal-snapshot'):
            raise ValueError('unsupported UFS profile')
        self.profile = profile
        filesystem_size = size
        if profile == 'journal-snapshot':
            filesystem_size = (size - (2 + JOURNAL_SECTORS +
                               SNAPSHOT_SECTORS) * SECTOR) // FRAG * FRAG
        self.data = bytearray(size)
        self.fragments = filesystem_size // self.frag_size
        self.ncg = cylinder_groups
        while True:
            self.fpg = ((self.fragments + self.ncg - 1) // self.ncg +
                        FRAGS - 1) & ~(FRAGS - 1)
            self.cgsize = 200 + (self.fpg + 7) // 8
            if self.cgsize <= BLOCK:
                break
            self.ncg += 1
        if self.fragments - (self.ncg - 1) * self.fpg <= self.dblk:
            raise ValueError('last cylinder group has no data space')
        self.cg_next = [self.dblk for _ in range(self.ncg)]
        self.used_fragments = [set(range(min(self.dblk, self.cg_ndblk(cg))))
                               for cg in range(self.ncg)]
        self.used_inodes = [set() for _ in range(self.ncg)]
        self.next_ino = 3
        self.nodes = {}
        self.node_modes = {2: 0o755}
        self.used_inodes[0].update(range(3))
        self._super()
        self._inode(2, IFDIR | 0o755, 2, 0, [])
        self.nodes[2] = []


    def cg_base(self, cg: int) -> int:
        return cg * self.fpg


    def cg_ndblk(self, cg: int) -> int:
        return min(self.fpg, self.fragments - self.cg_base(cg))


    def inode_offset(self, ino: int) -> int:
        cg, index = divmod(ino, self.ipg)
        if cg >= self.ncg:
            raise OSError(28, f'{self.format_name} inode table full')
        return ((self.cg_base(cg) + self.iblk) * self.frag_size +
                index * self.inode_size)


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
            (1312, 0), (1320, 120), (1372, MAGIC),
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


    def _allocate_inode(self) -> int:
        while self.next_ino < self.ncg * self.ipg:
            ino = self.next_ino
            self.next_ino += 1
            cg, index = divmod(ino, self.ipg)
            if index not in self.used_inodes[cg]:
                self.used_inodes[cg].add(index)
                return ino
        raise OSError(28, f'{self.format_name} inode table full')


    def _alloc_block(self, payload: bytes) -> int:
        for cg in range(self.ncg):
            start = ((self.cg_next[cg] + self.frags_per_block - 1) &
                     ~(self.frags_per_block - 1))
            ndblk = self.cg_ndblk(cg)
            if start + self.frags_per_block > ndblk:
                continue
            self.cg_next[cg] = start + self.frags_per_block
            self.used_fragments[cg].update(
                range(start, start + self.frags_per_block))
            fragment = self.cg_base(cg) + start
            offset = fragment * self.frag_size
            self.data[offset:offset + len(payload)] = payload
            return fragment
        raise OSError(28, 'UFS image full')


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
            raise ValueError('file exceeds UFS triple-indirect range')
        p64(raw, 24, (len(blocks) + metadata) *
            (self.block_size // 512))
        p32(raw, 80, ino)


    def _write_inline_symlink(self, ino, target):
        if len(target) > 120:
            raise ValueError('UFS inline symlink target too long')
        offset = self.inode_offset(ino)
        self.data[offset + 112:offset + 112 + len(target)] = target


    def add_file(self, parent, name, payload, mode=0o644):
        ino = self._allocate_inode()
        blocks = []
        for pos in range(0, len(payload), self.block_size):
            blocks.append(self._alloc_block(payload[pos:pos + self.block_size]))
        self._inode(ino, IFREG | mode, 1, len(payload), blocks)
        self.nodes[parent].append((name, ino, 8))
        return ino


    def add_dir(self, parent, name, mode=0o755):
        ino = self._allocate_inode()
        self.nodes[ino] = []
        self.node_modes[ino] = mode
        self._inode(ino, IFDIR | mode, 2, 0, [])
        self.nodes[parent].append((name, ino, 4))
        return ino


    def _child(self, parent, name, typ=None):
        for child_name, ino, child_type in self.nodes[parent]:
            if child_name == name and (typ is None or child_type == typ):
                return ino
        return None


    def add_tree(self, root: pathlib.Path, parent=2):
        for path in sorted(root.iterdir(), key=lambda item: item.name.encode()):
            if path.is_symlink():
                target = os.readlink(path).encode()
                ino = self._allocate_inode()
                self._inode(ino, IFLNK | 0o777, 1, len(target), [])
                try:
                    self._write_inline_symlink(ino, target)
                except ValueError as error:
                    raise ValueError(f'symlink target too long: {path}') from error
                self.nodes[parent].append((path.name, ino, 10))
            elif path.is_dir():
                child = self._child(parent, path.name, 4)
                if child is None:
                    child = self.add_dir(parent, path.name,
                                         path.stat().st_mode & 0o7777)
                self.add_tree(path, child)
            elif path.is_file():
                if self._child(parent, path.name) is not None:
                    raise ValueError(f'duplicate path: {path}')
                self.add_file(parent, path.name, path.read_bytes(),
                              path.stat().st_mode & 0o7777)


    def finish(self):
        parents = {2: 2}
        for parent, items in self.nodes.items():
            for _, ino, typ in items:
                if typ == 4:
                    parents[ino] = parent
        for ino, items in self.nodes.items():
            records = [('.', ino, 4), ('..', parents[ino], 4)] + items
            rawdir = bytearray()
            pos = 0
            previous = None
            for name, target, typ in records:
                raw = name.encode()
                minimum = ((8 + len(raw) + 3) // 4) * 4
                if minimum > 512:
                    raise ValueError(f'directory name too large: {name}')
                within = pos % 512
                if within + minimum > 512:
                    if previous is None:
                        raise ValueError('directory packing failure')
                    struct.pack_into('<H', rawdir, previous + 4,
                                     512 - (previous % 512))
                    rawdir.extend(b'\0' * (512 - within))
                    pos = len(rawdir)
                    previous = None
                rawdir.extend(b'\0' * minimum)
                struct.pack_into('<IHBB', rawdir, pos, target, minimum,
                                 typ, len(raw))
                rawdir[pos + 8:pos + 8 + len(raw)] = raw
                previous = pos
                pos += minimum
            if previous is not None:
                tail = 512 - (previous % 512)
                struct.pack_into('<H', rawdir, previous + 4, tail)
                if len(rawdir) % 512:
                    rawdir.extend(b'\0' * (512 - len(rawdir) % 512))
            blocks = []
            for offset in range(0, len(rawdir), self.block_size):
                blocks.append(self._alloc_block(
                    rawdir[offset:offset + self.block_size]))
            subdirs = sum(1 for _, _, typ in items if typ == 4)
            self._inode(ino, IFDIR | self.node_modes[ino], 2 + subdirs,
                        len(rawdir), blocks)
        self._cylinder_groups()
        self._persistence_tail()
        return bytes(self.data)


    def _cylinder_groups(self):
        totals = [0, 0, 0, 0]
        for cg_index in range(self.ncg):
            base = self.cg_base(cg_index)
            ndblk = self.cg_ndblk(cg_index)
            cg = memoryview(self.data)[
                (base + self.cblk) * self.frag_size:
                (base + self.cblk + self.frags_per_block) * self.frag_size]
            used_inodes = self.used_inodes[cg_index]
            ndir = sum(1 for ino in used_inodes
                       if (cg_index * self.ipg + ino) in self.nodes)
            nifree = self.ipg - len(used_inodes)
            free_fragments = set(range(ndblk)) - self.used_fragments[cg_index]
            nbfree = 0
            for fragment in range(0, ndblk - self.frags_per_block + 1,
                                  self.frags_per_block):
                if all(fragment + part in free_fragments
                       for part in range(self.frags_per_block)):
                    nbfree += 1
            nffree = len(free_fragments) - nbfree * self.frags_per_block
            values = [(4, CG_MAGIC), (12, cg_index), (20, ndblk),
                      (24, ndir), (28, nbfree), (32, nifree),
                      (36, nffree), (92, 168), (96, 200),
                      (100, self.cgsize), (116, self.ipg), (120, self.ipg)]
            for offset, value in values:
                p32(cg, offset, value)
            for ino in used_inodes:
                cg[168 + ino // 8] |= 1 << (ino & 7)
            for fragment in free_fragments:
                cg[200 + fragment // 8] |= 1 << (fragment & 7)
            totals[0] += ndir
            totals[1] += nbfree
            totals[2] += nifree
            totals[3] += nffree
            if cg_index != 0:
                start = (base + self.sblk) * self.frag_size
                self.data[start:start + self.super_size] = self.data[
                    self.super_offset:self.super_offset + self.super_size]
        self._write_global_summaries(totals)
        # Backups must include the final global summary totals.
        for cg_index in range(1, self.ncg):
            start = (self.cg_base(cg_index) + self.sblk) * self.frag_size
            self.data[start:start + self.super_size] = self.data[
                self.super_offset:self.super_offset + self.super_size]


    def _write_global_summaries(self, totals):
        sb = memoryview(self.data)[
            self.super_offset:self.super_offset + self.super_size]
        for offset, value in zip((1008, 1016, 1024, 1032), totals):
            p64(sb, offset, value)


    def _persistence_tail(self):
        if self.profile == 'ordinary':
            return
        end = self.fragments * (FRAG // SECTOR)
        cursor = end + 1 + JOURNAL_SECTORS
        journal = memoryview(self.data)[end * SECTOR:(end + 1) * SECTOR]
        journal[:4] = b'ZUJ2'
        p32(journal, 4, 2)
        p32(journal, 8, JOURNAL_SECTORS)
        p64(journal, 12, end)
        p32(journal, 24, digest(journal[:24]))
        snapshot = memoryview(self.data)[cursor * SECTOR:(cursor + 1) * SECTOR]
        snapshot[:4] = b'ZSL1'
        p32(snapshot, 4, 1)
        p32(snapshot, 8, SNAPSHOT_SECTORS)
        p64(snapshot, 16, cursor)
        p64(snapshot, 24, end)
        p32(snapshot, 32, digest(snapshot[:32]))
        control = memoryview(self.data)[(cursor + 1) * SECTOR:(cursor + 2) * SECTOR]
        control[:4] = b'ZSN1'
        p32(control, 4, 1)
        p32(control, 16, (SNAPSHOT_SECTORS - 1) // 2)
        p64(control, 24, end)
        p32(control, 32, digest(control[:32]))


def create(size, root=None, cylinder_groups=2, profile="ordinary"):
    image = Image(size, cylinder_groups, profile)
    if root:
        image.add_tree(pathlib.Path(root), 2)
    etc = image._child(2, 'etc', 4)
    if etc is None:
        etc = image.add_dir(2, 'etc')
    marker = image._child(etc, 'zedbsd-root')
    if marker is None:
        image.add_file(etc, 'zedbsd-root', b'zedBSD ufs root v1\n')
    return image.finish()

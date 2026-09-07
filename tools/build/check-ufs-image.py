#!/usr/bin/env python3
"""Validate the canonical zedBSD UFS image and its allocation ownership."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
from __future__ import annotations
import argparse
import pathlib
import struct
import sys

ROOT = 2
NDADDR = 12
NIADDR = 3
IFMT = 0o170000
IFDIR = 0o040000
IFLNK = 0o120000


class UFS:
    def __init__(self, data: bytes, *, runtime: bool = False):
        self.data = data
        self.runtime = runtime
        self.sb = memoryview(data)[65536:65536 + 8192]
        if len(self.sb) < 1376 or self.u32(self.sb, 1372) != 0x19540119:
            raise ValueError('bad UFS magic')
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
            raise ValueError('bad UFS superblock location')
        if (self.bsize, self.fsize, self.frag, self.nindir,
                self.inopb) != (8192, 1024, 8, 1024, 32):
            raise ValueError('unsupported UFS profile')
        if self.ncg < 1 or self.fpg < self.dblk:
            raise ValueError('bad cylinder group geometry')
        if self.fragments * self.fsize > len(data):
            raise ValueError('truncated image')
        self.maxsymlinklen = self.u32(self.sb, 1320)
        if (self.sblk, self.cblk, self.iblk, self.dblk, self.ipg,
                self.maxsymlinklen) != (64, 72, 80, 144, 256, 120):
            raise ValueError('unsupported UFS metadata layout')
        if self.cgoffset or self.cgmask or self.ncg < 2:
            raise ValueError('unsupported UFS group layout')
        if self.fragments - (self.ncg - 1) * self.fpg <= self.dblk:
            raise ValueError('invalid final cylinder group')
        self.cgs = [self._read_cg(index) for index in range(self.ncg)]


    @staticmethod
    def u16(data,offset): return struct.unpack_from('<H',data,offset)[0]


    @staticmethod
    def u32(data,offset): return struct.unpack_from('<I',data,offset)[0]


    @staticmethod
    def u64(data,offset): return struct.unpack_from('<Q',data,offset)[0]


    def cgstart(self,index):
        return index*self.fpg+self.cgoffset*(index & ~self.cgmask)


    def cg_ndblk(self,index):
        return min(self.fpg,self.fragments-self.cgstart(index))


    def _read_cg(self,index):
        base=self.cgstart(index)+self.cblk
        cg=memoryview(self.data)[base*self.fsize:(base+self.frag)*self.fsize]
        if len(cg)!=self.bsize or self.u32(cg,4)!=0x090255:
            raise ValueError(f'cylinder group {index}: bad magic')
        if self.u32(cg,12)!=index or self.u32(cg,20)!=self.cg_ndblk(index):
            raise ValueError(f'cylinder group {index}: bad identity/size')
        iusedoff=self.u32(cg,92); freeoff=self.u32(cg,96)
        nextfreeoff=self.u32(cg,100)
        if not (0<iusedoff<freeoff<nextfreeoff<=len(cg)):
            raise ValueError(f'cylinder group {index}: bad bitmap offsets')
        if self.cgsize!=nextfreeoff:
            raise ValueError(f'cylinder group {index}: size mismatch')
        if index:
            backup=memoryview(self.data)[(self.cgstart(index)+self.sblk)*self.fsize:
                                         (self.cgstart(index)+self.sblk)*self.fsize+8192]
            # Mounted UFS updates the primary clean byte and summary counters.
            # Backup geometry remains authoritative; only those live fields may differ.
            mutable = {209, *range(1008, 1040)} if self.runtime else set()
            if len(backup) != 8192 or any(backup[offset] != self.sb[offset]
                    for offset in range(1376) if offset not in mutable):
                raise ValueError(f'cylinder group {index}: stale backup superblock')
        return (cg,iusedoff,freeoff,nextfreeoff)


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
        if (self.u16(raw, 0) & IFMT) == IFLNK and self.u64(raw, 16) <= 120:
            return result
        for index in range(NDADDR):
            pointer = self.u64(raw, 112 + index * 8)
            if pointer:
                result.append(pointer)
        for depth in range(1, NIADDR + 1):
            pointer = self.u64(raw, 208 + (depth - 1) * 8)
            result.extend(self._indirect_values(pointer, depth, owned))
        return result


    def read_file(self, number):
        raw = self.inode(number)
        size = self.u64(raw, 16)
        if (self.u16(raw, 0) & IFMT) == IFLNK and size <= 120:
            return bytes(raw[112:112 + size])
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


    def entries(self,number):
        raw=self.read_file(number); result=[]; offset=0
        while offset<len(raw):
            if offset%512>504: raise ValueError(f'inode {number}: bad directory alignment')
            ino,reclen,typ,nlen=struct.unpack_from('<IHBB',raw,offset)
            if reclen<8 or reclen%4 or offset%512+reclen>512 or offset+reclen>len(raw) or 8+nlen>reclen:
                raise ValueError(f'inode {number}: bad directory record')
            if ino:
                name=raw[offset+8:offset+8+nlen].decode('utf-8')
                result.append((name,ino,typ))
            offset+=reclen
        return result


    def lookup(self,path):
        inode=ROOT
        for component in pathlib.PurePosixPath(path).parts:
            if component=='/': continue
            found=[entry for entry in self.entries(inode) if entry[0]==component]
            if len(found)!=1: raise ValueError(f'missing or duplicate path: {path}')
            inode=found[0][1]
        return inode


    def _own(self,fragment,owned,kind):
        group=None
        for index in range(self.ncg):
            start=self.cgstart(index); ndblk=self.cg_ndblk(index)
            if fragment>=start+self.dblk and fragment+self.frag<=start+ndblk:
                group=index; local=fragment-start; break
        if group is None or local%self.frag:
            raise ValueError(f'{kind}: invalid block {fragment}')
        for part in range(fragment,fragment+self.frag):
            if part in owned: raise ValueError(f'duplicate block ownership: fragment {part}')
            owned.add(part)


    def validate(self):
        allocated=set()
        for index,(cg,iusedoff,freeoff,_) in enumerate(self.cgs):
            iused=cg[iusedoff:freeoff]
            allocated.update(index*self.ipg+ino for ino in range(self.ipg)
                             if iused[ino//8]&(1<<(ino&7)))
        reachable=set(); references={}; owned=set(); pending=[ROOT]
        while pending:
            ino=pending.pop()
            if ino in reachable: continue
            if ino not in allocated: raise ValueError(f'reachable inode {ino} is free')
            reachable.add(ino); raw=self.inode(ino); mode=self.u16(raw,0)
            data_blocks=self.blocks(ino,owned)
            for fragment in data_blocks: self._own(fragment,owned,'data')
            if mode&IFMT==IFDIR:
                entries=self.entries(ino)
                names=[entry[0] for entry in entries]
                if names.count('.')!=1 or names.count('..')!=1:
                    raise ValueError(f'inode {ino}: missing dot entries')
                for name,child,_ in entries:
                    references[child]=references.get(child,0)+1
                    if name not in ('.','..'): pending.append(child)
        unexpected={ino for ino in allocated if ino>=ROOT and ino not in reachable}
        if unexpected: raise ValueError(f'orphan allocated inodes: {sorted(unexpected)}')
        for ino in reachable:
            nlink=self.u16(self.inode(ino),2)
            if references.get(ino,0)!=nlink:
                raise ValueError(f'inode {ino}: nlink {nlink}, references {references.get(ino,0)}')
        total_nbfree=0; total_nifree=0; total_nffree=0; total_ndir=0
        for index,(cg,iusedoff,freeoff,nextfreeoff) in enumerate(self.cgs):
            start=self.cgstart(index); ndblk=self.cg_ndblk(index)
            free=cg[freeoff:nextfreeoff]
            for local in range(ndblk):
                marked_free=bool(free[local//8]&(1<<(local&7)))
                absolute=start+local
                if local<self.dblk and marked_free:
                    raise ValueError(f'cylinder group {index}: metadata marked free')
                if local>=self.dblk and marked_free==(absolute in owned):
                    raise ValueError(f'fragment bitmap mismatch: {absolute}')
            for local in range(ndblk,self.fpg):
                if free[local//8]&(1<<(local&7)):
                    raise ValueError(f'cylinder group {index}: tail marked free')
            nbfree=sum(1 for local in range((self.dblk+self.frag-1)//self.frag*self.frag,
                                           ndblk-self.frag+1,self.frag)
                       if all(free[part//8]&(1<<(part&7))
                              for part in range(local,local+self.frag)))
            free_count=sum(1 for local in range(self.dblk,ndblk)
                           if free[local//8]&(1<<(local&7)))
            nffree=free_count-nbfree*self.frag
            local_allocated=sum(1 for ino in range(self.ipg)
                                if index*self.ipg+ino in allocated)
            nifree=self.ipg-local_allocated
            ndir=sum(1 for ino in reachable
                     if ino//self.ipg==index and
                     self.u16(self.inode(ino),0)&IFMT==IFDIR)
            if (self.u32(cg,24),self.u32(cg,28),self.u32(cg,32),self.u32(cg,36))!=(ndir,nbfree,nifree,nffree):
                raise ValueError(f'cylinder group {index}: summary mismatch')
            total_ndir+=ndir; total_nbfree+=nbfree
            total_nifree+=nifree; total_nffree+=nffree
        self._check_global_summaries(
            (total_ndir,total_nbfree,total_nifree,total_nffree))


    def _check_global_summaries(self, totals):
        actual = tuple(self.u64(self.sb, offset)
                       for offset in (1008, 1016, 1024, 1032))
        if actual != totals:
            raise ValueError('superblock summary counters mismatch')



def check(path):
    fs = UFS(pathlib.Path(path).read_bytes())
    fs.validate()
    marker = fs.read_file(fs.lookup('/etc/zedbsd-root'))
    if marker != b'zedBSD ufs root v1\n':
        raise ValueError('bad root marker')
    print(f'{path}: canonical zedBSD UFS OK')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('image')
    args = parser.parse_args()
    try:
        check(args.image)
    except Exception as error:
        print(f'{args.image}: {error}', file=sys.stderr)
        sys.exit(1)

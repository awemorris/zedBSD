#!/usr/bin/env python3
"""Validate the canonical, single-cylinder-group zedBSD UFS1 profile."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

MAGIC=0x011954; ROOT=2; NDADDR=12; NIADDR=3; INODE_SIZE=128
IFMT=0o170000; IFDIR=0o040000


class UFS1:
    def __init__(self,data: bytes):
        self.data=data; self.sb=memoryview(data)[8192:16384]
        if len(self.sb)<1376 or self.u32(self.sb,1372)!=MAGIC:
            raise ValueError('bad UFS1 magic')
        self.bsize=self.u32(self.sb,48); self.fsize=self.u32(self.sb,52)
        self.frag=self.u32(self.sb,56); self.nindir=self.u32(self.sb,116)
        self.inopb=self.u32(self.sb,120); self.ipg=self.u32(self.sb,184)
        self.fpg=self.u32(self.sb,188); self.iblk=self.u32(self.sb,16)
        self.dblk=self.u32(self.sb,20); self.cblk=self.u32(self.sb,12)
        self.fragments=self.u32(self.sb,36); self.ncg=self.u32(self.sb,44)
        self.cgsize=self.u32(self.sb,160)
        if (self.bsize,self.fsize,self.frag,self.nindir,self.inopb,self.ncg)!=(8192,1024,8,2048,64,1):
            raise ValueError('unsupported UFS1 profile')
        if self.fragments*self.fsize>len(data): raise ValueError('truncated image')
        self.cg=memoryview(data)[self.cblk*self.fsize:(self.cblk+self.frag)*self.fsize]
        if self.u32(self.cg,4)!=0x090255: raise ValueError('bad cylinder group magic')
        self.iusedoff=self.u32(self.cg,92); self.freeoff=self.u32(self.cg,96)
        self.nextfreeoff=self.u32(self.cg,100)
        if not (0<self.iusedoff<self.freeoff<self.nextfreeoff<=len(self.cg)):
            raise ValueError('bad cylinder group bitmap offsets')
        if self.cgsize!=self.nextfreeoff:
            raise ValueError('superblock/cylinder-group size mismatch')

    @staticmethod
    def u16(data,offset): return struct.unpack_from('<H',data,offset)[0]
    @staticmethod
    def u32(data,offset): return struct.unpack_from('<I',data,offset)[0]
    @staticmethod
    def u64(data,offset): return struct.unpack_from('<Q',data,offset)[0]

    def inode(self,number):
        if number<0 or number>=self.ipg: raise ValueError('inode out of range')
        offset=self.iblk*self.fsize+number*INODE_SIZE
        raw=memoryview(self.data)[offset:offset+INODE_SIZE]
        if len(raw)!=INODE_SIZE: raise ValueError('truncated inode table')
        return raw

    def _indirect_values(self,fragment,depth,owned):
        if fragment==0: return []
        if owned is not None: self._own(fragment,owned,'indirect')
        raw=memoryview(self.data)[fragment*self.fsize:fragment*self.fsize+self.bsize]
        if len(raw)!=self.bsize: raise ValueError('truncated indirect block')
        values=[]
        for index in range(self.nindir):
            pointer=self.u32(raw,index*4)
            if not pointer: continue
            if depth==1: values.append(pointer)
            else: values.extend(self._indirect_values(pointer,depth-1,owned))
        return values

    def blocks(self,number,owned=None):
        raw=self.inode(number); result=[]
        for index in range(NDADDR):
            pointer=self.u32(raw,40+index*4)
            if pointer: result.append(pointer)
        for depth in range(1,NIADDR+1):
            result.extend(self._indirect_values(self.u32(raw,88+(depth-1)*4),depth,owned))
        return result

    def read_file(self,number):
        raw=self.inode(number); size=self.u64(raw,8); result=bytearray()
        blocks=self.blocks(number)
        for fragment in blocks:
            start=fragment*self.fsize
            result.extend(self.data[start:start+self.bsize])
            if len(result)>=size: break
        if len(result)<size: raise ValueError(f'inode {number}: sparse/truncated file unsupported by checker')
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
        if fragment<self.dblk or fragment+self.frag>self.fragments or fragment%self.frag:
            raise ValueError(f'{kind}: invalid block {fragment}')
        for part in range(fragment,fragment+self.frag):
            if part in owned: raise ValueError(f'duplicate block ownership: fragment {part}')
            owned.add(part)

    def validate(self):
        iused=self.cg[self.iusedoff:self.freeoff]
        allocated={ino for ino in range(self.ipg) if iused[ino//8]&(1<<(ino&7))}
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
        free=self.cg[self.freeoff:self.nextfreeoff]
        for fragment in range(self.dblk,self.fragments):
            marked_free=bool(free[fragment//8]&(1<<(fragment&7)))
            if marked_free==(fragment in owned):
                raise ValueError(f'fragment bitmap mismatch: {fragment}')
        nbfree=sum(1 for fragment in range((self.dblk+self.frag-1)//self.frag*self.frag,
                                           self.fragments-self.frag+1,self.frag)
                   if all(free[part//8]&(1<<(part&7)) for part in range(fragment,fragment+self.frag)))
        nifree=self.ipg-len(allocated)
        ndir=sum(1 for ino in reachable if self.u16(self.inode(ino),0)&IFMT==IFDIR)
        if self.u32(self.cg,28)!=nbfree or self.u32(self.cg,32)!=nifree:
            raise ValueError('cylinder group free counters mismatch')
        if (self.u32(self.sb,192),self.u32(self.sb,196),self.u32(self.sb,200))!=(ndir,nbfree,nifree):
            raise ValueError('superblock summary counters mismatch')


def check(path):
    fs=UFS1(pathlib.Path(path).read_bytes()); fs.validate()
    marker=fs.read_file(fs.lookup('/etc/zedbsd-root'))
    if marker!=b'zedBSD ufs1 root v1\n': raise ValueError('bad root marker')
    print(f'{path}: canonical zedBSD UFS1 OK')


if __name__=='__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('image'); args=parser.parse_args()
    try: check(args.image)
    except Exception as error:
        print(f'{args.image}: {error}',file=sys.stderr); sys.exit(1)

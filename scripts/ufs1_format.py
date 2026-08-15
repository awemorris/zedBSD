#!/usr/bin/env python3
"""Deterministic canonical 4.4BSD-style UFS1 image writer for zedBSD."""
from __future__ import annotations
import os, pathlib, struct

SECTOR=512; FRAG=1024; BLOCK=8192; FRAGS=8; SUPER=8192; NINDIR=BLOCK//4
MAGIC=0x011954; INODE_SIZE=128; IPG=256; IBLK=24; DBLK=56
IFDIR=0o040000; IFREG=0o100000; IFLNK=0o120000

def p32(b,o,v): struct.pack_into('<I',b,o,v & 0xffffffff)
def p64(b,o,v): struct.pack_into('<Q',b,o,v & 0xffffffffffffffff)

class Image:
    def __init__(self, size: int):
        if size < 4*1024*1024 or size % FRAG: raise ValueError('bad UFS1 size')
        self.data=bytearray(size); self.fragments=size//FRAG; self.next_frag=DBLK
        self.cgsize=200+(self.fragments+7)//8
        if self.cgsize>BLOCK:
            raise ValueError('single-cylinder-group UFS1 image is too large')
        self.next_ino=3; self.nodes={}; self.node_modes={2:0o755}
        self._super(); self._inode(2, IFDIR|0o755, 2, 0, [])
        self.nodes[2]=[]

    def _super(self):
        s=memoryview(self.data)[SUPER:SUPER+8192]
        for off,val in [(8,8),(12,16),(16,IBLK),(20,DBLK),(36,self.fragments),
            (40,self.fragments-DBLK),(44,1),(48,BLOCK),(52,FRAG),(56,FRAGS),
            (80,13),(84,10),(96,3),(100,1),(104,1376),(116,NINDIR),
            (120,64),(152,0),(156,0),(160,self.cgsize),(184,IPG),(188,self.fragments),
            (1320,60),(1324,2),(1352,0),(1372,MAGIC)]: p32(s,off,val)
        p64(s,1328,0x7fffffffffffffff); s[209]=1

    def _alloc_block(self, payload: bytes) -> int:
        start=(self.next_frag+FRAGS-1)&~(FRAGS-1)
        if start+FRAGS>self.fragments: raise OSError(28,'UFS1 image full')
        self.next_frag=start+FRAGS
        off=start*FRAG; self.data[off:off+len(payload)]=payload
        return start

    def _indirect(self, blocks, depth):
        capacity=NINDIR**depth
        if len(blocks)>capacity: raise ValueError('too many indirect blocks')
        pointers=[]; metadata=0
        if depth==1:
            pointers=list(blocks)
        else:
            span=NINDIR**(depth-1)
            for pos in range(0,len(blocks),span):
                pointer,count=self._indirect(blocks[pos:pos+span],depth-1)
                pointers.append(pointer); metadata+=count
        raw=bytearray(BLOCK)
        for index,pointer in enumerate(pointers): p32(raw,index*4,pointer)
        return self._alloc_block(raw),metadata+1

    def _inode(self, ino, mode, nlink, size, blocks, uid=0, gid=0):
        off=IBLK*FRAG+ino*INODE_SIZE; d=memoryview(self.data)[off:off+INODE_SIZE]
        struct.pack_into('<HH',d,0,mode,nlink); p64(d,8,size)
        direct=blocks[:12]; remaining=blocks[12:]; metadata=0
        for i,block in enumerate(direct): p32(d,40+i*4,block)
        for depth in range(1,4):
            capacity=NINDIR**depth; group=remaining[:capacity]
            if group:
                pointer,count=self._indirect(group,depth)
                p32(d,88+(depth-1)*4,pointer); metadata+=count
                remaining=remaining[len(group):]
        if remaining: raise ValueError('file exceeds UFS1 triple-indirect range')
        p32(d,104,(len(blocks)+metadata)*16); p32(d,108,ino); p32(d,112,uid); p32(d,116,gid)

    def add_file(self, parent, name, payload, mode=0o644):
        ino=self.next_ino; self.next_ino+=1; blocks=[]
        for pos in range(0,len(payload),BLOCK): blocks.append(self._alloc_block(payload[pos:pos+BLOCK]))
        self._inode(ino,IFREG|mode,1,len(payload),blocks); self.nodes[parent].append((name,ino,8)); return ino

    def add_dir(self,parent,name,mode=0o755):
        ino=self.next_ino; self.next_ino+=1; self.nodes[ino]=[]; self.node_modes[ino]=mode
        self._inode(ino,IFDIR|mode,2,0,[]); self.nodes[parent].append((name,ino,4)); return ino

    def _child(self,parent,name,typ=None):
        for child_name,ino,child_type in self.nodes[parent]:
            if child_name==name and (typ is None or child_type==typ): return ino
        return None

    def add_tree(self, root: pathlib.Path, parent=2):
        for p in sorted(root.iterdir(), key=lambda x:x.name.encode()):
            if p.is_symlink():
                target=os.readlink(p).encode(); ino=self.next_ino; self.next_ino+=1
                if len(target)>60: raise ValueError(f'symlink target too long: {p}')
                self._inode(ino,IFLNK|0o777,1,len(target),[])
                off=IBLK*FRAG+ino*INODE_SIZE; self.data[off+40:off+40+len(target)]=target
                self.nodes[parent].append((p.name,ino,10))
            elif p.is_dir():
                child=self._child(parent,p.name,4)
                if child is None: child=self.add_dir(parent,p.name,p.stat().st_mode & 0o777)
                self.add_tree(p,child)
            elif p.is_file():
                if self._child(parent,p.name) is not None: raise ValueError(f'duplicate path: {p}')
                self.add_file(parent,p.name,p.read_bytes(),p.stat().st_mode & 0o777)

    def finish(self):
        parents={2:2}
        for p,items in self.nodes.items():
            for _,ino,typ in items:
                if typ==4: parents[ino]=p
        for ino,items in self.nodes.items():
            records=[('.',ino,4),('..',parents[ino],4)]+items; rawdir=bytearray(); pos=0; previous=None
            for name,target,typ in records:
                raw=name.encode(); minimum=((8+len(raw)+3)//4)*4
                if minimum>512: raise ValueError(f'directory name too large: {name}')
                within=pos%512
                if within+minimum>512:
                    if previous is None: raise ValueError('directory packing failure')
                    struct.pack_into('<H',rawdir,previous+4,512-(previous%512))
                    rawdir.extend(b'\0'*(512-within)); pos=len(rawdir); previous=None
                rawdir.extend(b'\0'*minimum)
                struct.pack_into('<IHBB',rawdir,pos,target,minimum,typ,len(raw))
                rawdir[pos+8:pos+8+len(raw)]=raw; previous=pos; pos+=minimum
            if previous is not None:
                tail=512-(previous%512)
                struct.pack_into('<H',rawdir,previous+4,tail)
                rawdir.extend(b'\0'*(512-(len(rawdir)%512)) if len(rawdir)%512 else b'')
            blocks=[]
            for offset in range(0,len(rawdir),BLOCK): blocks.append(self._alloc_block(rawdir[offset:offset+BLOCK]))
            subdirs=sum(1 for _,_,typ in items if typ==4)
            self._inode(ino,IFDIR|self.node_modes[ino],2+subdirs,len(rawdir),blocks)
        self._cylinder_group()
        return bytes(self.data)

    def _cylinder_group(self):
        cg=memoryview(self.data)[16*FRAG:24*FRAG]
        ndir=len(self.nodes); nbfree=(self.fragments-self.next_frag)//FRAGS
        nifree=IPG-self.next_ino; nffree=(self.fragments-self.next_frag)%FRAGS
        for off,val in [(4,0x090255),(12,0),(20,self.fragments),(24,ndir),
            (28,nbfree),(32,nifree),(36,nffree),(92,168),(96,200),
            (100,self.cgsize),(116,IPG),(120,IPG)]: p32(cg,off,val)
        for ino in range(self.next_ino): cg[168+ino//8] |= 1 << (ino&7)
        for frag in range(self.next_frag,self.fragments): cg[200+frag//8] |= 1 << (frag&7)
        sb=memoryview(self.data)[SUPER:SUPER+8192]
        for off,val in [(192,ndir),(196,nbfree),(200,nifree),(204,nffree)]: p32(sb,off,val)

def create(size, root=None):
    image=Image(size)
    if root: image.add_tree(pathlib.Path(root),2)
    etc=image._child(2,'etc',4)
    if etc is None: etc=image.add_dir(2,'etc')
    marker=image._child(etc,'zedbsd-root')
    if marker is None: image.add_file(etc,'zedbsd-root',b'zedBSD ufs1 root v1\n')
    return image.finish()

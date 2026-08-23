#!/usr/bin/env python3
import pathlib, sys, tempfile
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'/'build'))
from ufs1_format import create
from importlib.machinery import SourceFileLoader
checker=SourceFileLoader('check',str(pathlib.Path(__file__).resolve().parents[1]/'tools'/'build'/'check-ufs1-image.py')).load_module()
check=checker.check
with tempfile.TemporaryDirectory() as d:
 root=pathlib.Path(d)/'root'; root.mkdir(); (root/'bin').mkdir()
 payload=bytes((index*37)&0xff for index in range(200000))
 (root/'bin'/'large').write_bytes(payload)
 for index in range(100): (root/'bin'/f'f{index:03d}').write_text(str(index))
 p=pathlib.Path(d)/'test.ufs'; p.write_bytes(create(8*1024*1024,root)); check(p)
 fs=checker.UFS1(p.read_bytes())
 assert fs.read_file(fs.lookup('/bin/large'))==payload
 assert fs.read_file(fs.lookup('/bin/f099'))==b'99'

 # Exercise power-of-two and odd CG counts, a shortened last CG, and a file
 # whose blocks cross a CG boundary.  The default remains the byte-compatible
 # single-CG profile above.
 crossing=bytes((index*19)&0xff for index in range(3*1024*1024))
 (root/'bin'/'crossing').write_bytes(crossing)
 for groups,size_mib in ((2,9),(3,17),(16,32)):
  multi=pathlib.Path(d)/f'test-{groups}cg.ufs'
  multi.write_bytes(create(size_mib*1024*1024,root,groups))
  check(multi)
  fs=checker.UFS1(multi.read_bytes())
  assert fs.ncg==groups
  assert fs.read_file(fs.lookup('/bin/crossing'))==crossing
 b=bytearray(p.read_bytes()); b[8192+1372]=0; p.write_bytes(b)
 try: check(p); raise AssertionError('corrupt magic accepted')
 except ValueError: pass
print('zedBSD UFS1 Python formatter tests: PASS')

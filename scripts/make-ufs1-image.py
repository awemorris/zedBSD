#!/usr/bin/env python3
import argparse, pathlib
from ufs1_format import create
p=argparse.ArgumentParser(description='create a canonical zedBSD UFS1 image')
p.add_argument('output'); p.add_argument('--size-mib',type=int,default=16); p.add_argument('--root')
p.add_argument('--cylinder-groups',type=int,default=1)
a=p.parse_args(); pathlib.Path(a.output).write_bytes(create(a.size_mib*1024*1024,a.root,a.cylinder_groups))

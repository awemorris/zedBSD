#!/usr/bin/env python3
"""Actual vmap/scratch owners; deterministic physical and table boundaries."""
from pathlib import Path
import hashlib,json,os,subprocess,sys
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws025-io-memory-cache/temp');out.mkdir(exist_ok=False)
paths=['src/hal/amd64/space-vmap.inc','src/kern/io-scratch.c','include/kern/io-scratch.h','include/hal/hal.h']
(out/'source.json').write_text(json.dumps({f:hashlib.sha256((repo/f).read_bytes()).hexdigest() for f in paths},indent=2)+'\n')
for variant in ('ordinary','sanitize'):
 flags=[] if variant=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
 binary=out/variant
 args=['cc','-std=c11','-g','-Wall','-Wextra','-Werror','-Iinclude','-Iinclude/uapi','-I.',*flags,'plan/ws025-io-memory-cache/tests/vmap-host.c','src/kern/io-scratch.c','-pthread','-o',str(binary)]
 subprocess.run(args,cwd=repo,check=True)
 subprocess.run(['timeout','60',str(binary)],cwd=repo,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})

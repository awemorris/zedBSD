#!/usr/bin/env python3
"""Actual vmap/scratch owners; deterministic physical and table boundaries."""
from pathlib import Path
import hashlib,json,os,subprocess,sys
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws025-io-memory-cache/temp');out.mkdir(exist_ok=False)
paths=['src/kern/vm.c','src/kern/io.c','include/kern/io-scratch.h','include/hal/hal.h']
(out/'source.json').write_text(json.dumps({f:hashlib.sha256((repo/f).read_bytes()).hexdigest() for f in paths},indent=2)+'\n')
for variant in ('ordinary','sanitize'):
 flags=[] if variant=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
 binary=out/variant
 source=(repo/'src/kern/vm.c').read_text()
 (out/'vm-kernel-map-extracted.c').write_text('#include <kern/vm-kernel-map.h>\n#include <kern/lock.h>\n#include <string.h>\n#include <limits.h>\n#define PAGE_SIZE 4096U\n'+source[source.index('/* Common ownership of bounded, contiguous kernel virtual mappings. */'):])
 args=['cc','-std=c11','-g','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Wall','-Wextra','-Werror','-Iinclude','-Iinclude/uapi','-I.','-I'+str(out),*flags,'plan/ws025-io-memory-cache/tests/vmap-host.c','src/kern/io.c',str(out/'vm-kernel-map-extracted.c'),'-pthread','-o',str(binary)]
 subprocess.run(args,cwd=repo,check=True)
 subprocess.run(['timeout','60',str(binary)],cwd=repo,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})

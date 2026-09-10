#!/usr/bin/env python3
from pathlib import Path
import os,subprocess,sys
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws025-io-memory-cache/temp');out.mkdir(exist_ok=False)
for variant in ('ordinary','sanitize'):
 flags=[] if variant=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
 binary=out/variant
 args=['cc','-std=c11','-g','-Dtid_t=int32_t','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Wall','-Wextra','-Werror','-Iinclude','-Iinclude/uapi','-I.',*flags,'plan/ws025-io-memory-cache/tests/dma-vector-host.c','src/drivers/generic/dma.c','src/hal/amd64/pmem-range.c','src/kern/cache.c','src/kern/io.c','-pthread','-o',str(binary)]
 subprocess.run(args,cwd=repo,check=True)
 subprocess.run(['timeout','60',str(binary)],cwd=repo,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})

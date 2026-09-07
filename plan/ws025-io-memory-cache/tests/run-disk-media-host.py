#!/usr/bin/env python3
from pathlib import Path
import os,subprocess,sys
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws025-io-memory-cache/temp');out.mkdir(exist_ok=False)
for mode in ('ordinary','sanitize'):
 flags=[] if mode=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','--param=asan-globals=0']
 binary=out/mode
 args=['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-DZEDBSD_USER_ABI_LP64','-DZEDBSD_STORAGE_HOST_TEST','-I.','-Iinclude','-Iinclude/uapi','-Isrc','-Ilibc/include',*flags,'plan/ws025-io-memory-cache/tests/disk-media-host.c','src/drivers/disklabel/mbr.c','src/kern/io-stats.c','src/kern/io-error.c','-Wl,--gc-sections','-o',str(binary)]
 subprocess.run(args,cwd=repo,check=True)
 subprocess.run(['timeout','60',str(binary)],cwd=repo,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})

#!/usr/bin/env python3
"""Real VM/file/reclaim owners with deterministic physical mappings."""
from pathlib import Path
import os,subprocess,sys
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws025-io-memory-cache/temp');out.mkdir(exist_ok=False)
sources=['file','filedesc','readahead','backing-claim','vm-object','cache-memory','io-error','writeback','vmspace','vm-reclaim','io-stats','elf']
for variant in ('ordinary','sanitize'):
 flags=[] if variant=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','--param=asan-globals=0']
 binary=out/variant
 command=['cc','-std=c11','-O0','-g','-DZEDBSD_USER_ABI_LP64','-DHAL_ARCH_AMD64','-Iinclude','-Iinclude/uapi','-I.','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',*flags,*['src/kern/'+s+'.c' for s in sources],'plan/ws025-io-memory-cache/tests/exec-snapshot-vm-host.c','-Wl,--gc-sections','-pthread','-o',str(binary)]
 subprocess.run(command,cwd=repo,check=True)
 subprocess.run(['timeout','60',str(binary)],cwd=repo,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})

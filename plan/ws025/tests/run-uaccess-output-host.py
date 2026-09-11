#!/usr/bin/env python3
from pathlib import Path
import hashlib,json,os,subprocess,sys
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws025/temp');out.mkdir(exist_ok=False)
paths=['src/kern/uaccess.c','include/kern/uaccess.h','plan/ws025/tests/uaccess-output-host.c']
(out/'source.json').write_text(json.dumps({p:hashlib.sha256((repo/p).read_bytes()).hexdigest() for p in paths},indent=2)+'\n')
for mode in ('ordinary','sanitize'):
 flags=[] if mode!='sanitize' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
 if mode=='unsupported':flags+=['-DOMIT_BORROW']
 binary=out/mode
 with (out/(mode+'.log')).open('w') as log:
  subprocess.run(['cc','-std=c11','-g','-pthread','-Dtid_t=int32_t','-DZEDBSD_USER_ABI_LP64','-Iinclude','-Iinclude/uapi','-I.','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',*flags,paths[0],paths[2],'-Wl,--gc-sections','-o',str(binary)],cwd=repo,stdout=log,stderr=subprocess.STDOUT,check=True)
  subprocess.run(['timeout','60',str(binary)],cwd=repo,stdout=log,stderr=subprocess.STDOUT,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
 print(mode+': PASS')

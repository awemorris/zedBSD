#!/usr/bin/env python3
"""Unified production driver audit, run/view ownership and consistency gates."""
__import__('runpy').run_path(str(__import__('pathlib').Path(__file__).resolve().parents[3] / 'plan/ws025/tests/prepare-driver-fragments.py'), run_name='__main__')
from pathlib import Path
import json,os,subprocess,sys
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws024/temp');out.mkdir(parents=True,exist_ok=False)
commands=[]
def run(name,args):
 commands.append(dict(name=name,argv=args));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
 r=subprocess.run(args,cwd=repo,capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
 (out/(name+'.log')).write_text(r.stdout+r.stderr);print(name,r.returncode,r.stdout[-600:],r.stderr[-1500:],flush=True)
 if r.returncode:sys.exit(1)
for arch in ('amd64',):
 for variant in ('ordinary','sanitize'):
  prefix=arch+'-'+variant
  abi=['-DKERN_USER_ABI_LP64'] if arch=='amd64' else ['-m32']
  san=[] if variant=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','--param','asan-globals=0']
  bridge=str(out/(prefix+'-thread.o'))
  run(prefix+'-thread',['cc',*abi,*san,'-O1','-g','-pthread','-c','plan/ws018/tests/mount-thread-host.c','-o',bridge])
  flags=['cc',*abi,*san,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-I.','-Iinclude','-Iinclude/uapi','-Isrc','-Ilibc/include','-Iplan/ws018/tests','-Iplan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs']
  for kind in ('metadata','run','view','consistency'):
   source='plan/ws024/tests/ufs-'+kind+'-host.c'
   extra=['plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-endian.c','src/kern/quota.c','src/kern/io-stats.c',bridge,'-pthread']
   if kind=='consistency':extra=['plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-journal.c','plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-snapshot.c']
   binary=str(out/(prefix+'-'+kind))
   run(prefix+'-'+kind+'-build',[*flags,source,*extra,'-Wl,--gc-sections','-o',binary])
   run(prefix+'-'+kind,['timeout','90s',binary])

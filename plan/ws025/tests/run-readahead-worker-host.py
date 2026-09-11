#!/usr/bin/env python3
"""Production readahead queue lifetime with controlled VM and device completion."""
from pathlib import Path
import subprocess,sys,json,hashlib,os
root=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(root/'plan/ws025/temp');out.mkdir(parents=True,exist_ok=False)
sources=['src/kern/readahead.c','src/kern/readahead-worker.c','src/kern/io-scratch.c','src/kern/shutdown.c','plan/ws025/tests/readahead-worker-host.c']
(out/'source.json').write_text(json.dumps({p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources},indent=2)+'\n')
commands=[]
for variant in ('ordinary','sanitize'):
 extra=[] if variant=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','--param','asan-globals=0']
 bridges=[]
 for name,path in [('thread','plan/ws018/tests/mount-thread-host.c'),('exit','plan/ws025/tests/async-thread-host.c')]:
  obj=out/(variant+'-'+name+'.o');bridges.append(str(obj))
  command=['cc','-O1','-g','-pthread',*extra,'-c',path,'-o',str(obj)]
  subprocess.run(command,cwd=root,check=True)
 binary=str(out/variant)
 build=['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-DKERN_USER_ABI_LP64','-Dsched_yield=shutdown_test_yield','-ffunction-sections','-fdata-sections','-I.','-Iinclude','-Iinclude/uapi','-Isrc','-Ilibc/include','-I'+str(out),*extra,*sources,*bridges,'-pthread','-Wl,--gc-sections','-o',binary]
 for name,args in [(variant+'-build',build),(variant,['timeout','60s',binary])]:
  commands.append(dict(name=name,argv=args));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
  result=subprocess.run(args,cwd=root,capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
  (out/(name+'.log')).write_text(result.stdout+result.stderr)
  print(name,result.returncode,result.stdout[-1000:],result.stderr[-2000:],flush=True)
  if result.returncode:sys.exit(1)

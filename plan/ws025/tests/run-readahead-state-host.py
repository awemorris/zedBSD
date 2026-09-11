#!/usr/bin/env python3
"""Test the production stream predictor and retain commands/source hashes."""
from pathlib import Path
import subprocess,sys,json,hashlib,os
root=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(root/'plan/ws025/temp');out.mkdir(parents=True,exist_ok=False)
sources=['src/kern/readahead.c','plan/ws025/tests/readahead-state-host.c']
(out/'source.json').write_text(json.dumps({p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources+['include/kern/readahead.h']},indent=2)+'\n')
commands=[]
for mode in ('ordinary','sanitize'):
 extra=[] if mode=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
 binary=str(out/mode)
 build=['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-DZEDBSD_USER_ABI_LP64','-Iinclude','-Iinclude/uapi',*extra,*sources,'-o',binary]
 for name,args in [(mode+'-build',build),(mode,['timeout','30s',binary])]:
  commands.append(dict(name=name,argv=args));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
  r=subprocess.run(args,cwd=root,capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
  (out/(name+'.log')).write_text(r.stdout+r.stderr);print(name,r.returncode,r.stdout,r.stderr,flush=True)
  if r.returncode:sys.exit(1)

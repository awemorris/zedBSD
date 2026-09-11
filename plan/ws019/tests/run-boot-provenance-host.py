#!/usr/bin/env python3
"""Production FAT plus maintained deterministic disk/VFS fixture."""
from pathlib import Path
import subprocess,sys,json,hashlib,os
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws019/temp');out.mkdir(parents=True,exist_ok=False)
sources=['src/kern/boot.c', 'bootloader/uefi/volume-discovery.c', 'src/hal/amd64/bsp-pcat/handoff-validation.c', 'plan/ws019/tests/boot-provenance-host.c']
(out/'source.json').write_text(json.dumps({s:hashlib.sha256((repo/s).read_bytes()).hexdigest() for s in sources},indent=2)+'\n')
commands=[]
for variant in ('ordinary','sanitize'):
 extra=[] if variant=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','--param','asan-globals=0']
 binary=str(out/variant)
 build=['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-DKERN_USER_ABI_LP64','-ffunction-sections','-fdata-sections','-I.','-Iinclude','-Iinclude/uapi','-Isrc','-Ilibc/include',*extra,*sources,'-Wl,--gc-sections','-o',binary]
 for name,args in ((variant+'-build',build),(variant,['timeout','60s',binary])):
  commands.append(dict(name=name,argv=args));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
  r=subprocess.run(args,cwd=repo,capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
  (out/(name+'.log')).write_text(r.stdout+r.stderr);print(name,r.returncode,r.stdout[-800:],r.stderr[-1000:],flush=True)
  if r.returncode:sys.exit(1)

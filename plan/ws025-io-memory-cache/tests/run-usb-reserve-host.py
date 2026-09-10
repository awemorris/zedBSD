#!/usr/bin/env python3
"""Production core/HCD reservation tests with retained build commands and hashes."""
from pathlib import Path
import subprocess,json,sys,os,hashlib
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws025-io-memory-cache/temp');out.mkdir(parents=True,exist_ok=False)
commands=[]
def run(name,args):
 commands.append(dict(name=name,argv=args));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
 r=subprocess.run(args,cwd=repo,capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
 (out/(name+'.log')).write_text(r.stdout+r.stderr);print(name,r.returncode,(r.stdout+r.stderr)[-2500:],flush=True)
 if r.returncode:sys.exit(1)
for mode in ('ordinary','sanitize'):
 extra=[] if mode=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','--param','asan-globals=0']
 for name in (sys.argv[2:] or ['usb','xhci','storage']):
  if name not in ('usb','xhci','storage'): raise ValueError(name)
  binary=str(out/(name+'-'+mode))
  run(name+'-'+mode+'-build',['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-I.','-Iinclude','-Iinclude/uapi','-Isrc',*extra,'plan/ws025-io-memory-cache/tests/'+name+'-reserve-host.c','src/kern/io.c','-pthread','-Wl,--gc-sections','-o',binary])
  run(name+'-'+mode,['timeout','60s',binary])
(out/'source.json').write_text(json.dumps({p:hashlib.sha256((repo/p).read_bytes()).hexdigest() for p in ('src/drivers/usb/usb.c','src/drivers/pci/pci-xhci.c','src/drivers/usb/usb-storage.c','include/drivers/usb.h')},indent=2)+'\n')
print('WS025 reservation gates PASS: ' + ', '.join(sys.argv[2:] or ['usb','xhci','storage']))

#!/usr/bin/env python3
"""Production buf.c plus verbatim disk transfer implementation and injected HAL/BIO."""
from pathlib import Path
import hashlib,json,subprocess,sys,os
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws025/temp');out.mkdir(parents=True,exist_ok=False)
s=(repo/'src/kern/disk.c').read_text()
def function(name):
    token='\n'+name+'(\n';at=s.index(token);start=s.rfind('\n',0,at-1)+1
    if s[start:at]=='int' and s[start-7:start]=='static ':start-=7
    brace=s.index('{',at);depth=1;end=brace+1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]+'\n'
# Helper first, then public wrappers, exactly as production defines them.
(out/'direct-transfer.inc').write_text('\n'.join(function(n) for n in ('disk_transfer_direct','disk_read_direct','disk_write_direct','disk_write_direct_context','disk_transfer_progress','disk_transfer_progress_context')))
(out/'source.json').write_text(json.dumps({p:hashlib.sha256((repo/p).read_bytes()).hexdigest() for p in ('src/kern/disk.c','src/kern/buf.c')},indent=2)+'\n')
commands=[]
def run(name,args):
    commands.append(dict(name=name,argv=args));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    r=subprocess.run(args,cwd=repo,capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
    (out/(name+'.log')).write_text(r.stdout+r.stderr)
    print(name,r.returncode,r.stdout[-500:],r.stderr[-1500:],flush=True)
    if r.returncode:sys.exit(1)
for mode in ('ordinary','sanitize'):
    extra=[] if mode=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','--param','asan-globals=0']
    bridge=str(out/(mode+'-thread.o'))
    run(mode+'-thread',['cc','-O1','-g','-pthread',*extra,'-c','plan/ws018/tests/mount-thread-host.c','-o',bridge])
    binary=str(out/mode)
    run(mode+'-build',['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-DKERN_USER_ABI_LP64','-ffunction-sections','-fdata-sections','-I.','-Iinclude','-Iinclude/uapi','-Isrc','-Ilibc/include','-I'+str(out),*extra,'plan/ws025/tests/cache-buffer-host.c', 'src/kern/cache-memory.c', 'src/kern/io-stats.c',bridge,'-pthread','-Wl,--gc-sections','-o',binary])
    run(mode,['timeout','60s',binary])
print('WS025 shared buffer accounting PASS')

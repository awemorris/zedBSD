#!/usr/bin/env python3
"""Exercise verbatim ARM64 C dispatch; assemble actual entry stubs separately."""
from pathlib import Path
import subprocess,sys,json,hashlib
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws025/temp');out.mkdir(exist_ok=False)
paths=['src/hal/arm64/int.c','src/hal/arm64/int.h','src/hal/arm64/trap.S','include/hal/hal.h','plan/ws025/tests/arm64-fixed-entry-host.c']
(out/'source.json').write_text(json.dumps({p:hashlib.sha256((repo/p).read_bytes()).hexdigest() for p in paths},indent=2)+'\n')
s=(repo/paths[0]).read_text();a=s.index('void arm64_sync_handler(');b=s.index('\nvoid arm64_irq_handler',a)
(out/'arm64-dispatch-extracted.h').write_text(s[a:b])
for mode,flags in [('ordinary',[]),('sanitize',['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie'])]:
 with (out/(mode+'.log')).open('w') as log:
  subprocess.run(['cc','-std=c11','-Dtid_t=int32_t','-Iinclude','-Iinclude/uapi','-I.','-I'+str(out),*flags,paths[-1],'-o',str(out/mode)],cwd=repo,stdout=log,stderr=subprocess.STDOUT,check=True)
  subprocess.run([str(out/mode)],stdout=log,stderr=subprocess.STDOUT,check=True)
with (out/'target.log').open('w') as log:
 subprocess.run(['/usr/bin/clang','--target=aarch64-unknown-none','-c',paths[2],'-o',str(out/'trap.o')],cwd=repo,stdout=log,stderr=subprocess.STDOUT,check=True)
 subprocess.run(['/usr/bin/clang','--target=aarch64-unknown-none','-ffreestanding','-nostdinc','-Ilibc/include','-Iinclude','-Iinclude/uapi','-DHAL_ARCH_ARM64','-fsyntax-only',paths[0]],cwd=repo,stdout=log,stderr=subprocess.STDOUT,check=True)
print('ARM64 dispatch and target assembly/syntax: PASS')

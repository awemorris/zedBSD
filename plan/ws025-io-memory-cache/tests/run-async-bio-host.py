#!/usr/bin/env python3
"""Production FAT plus maintained deterministic disk/VFS fixture."""
from pathlib import Path
import subprocess,sys,json,hashlib,os
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws025-io-memory-cache/temp');out.mkdir(parents=True,exist_ok=False)
loop=(repo/'src/drivers/generic/loop.c').read_text()
a=loop.index('struct loop_device {');b=loop.index('\n};',a)+3
c=loop.index('static int\nloop_submit(');d=loop.index('\nstatic const struct disk_ops loop_disk_ops',c)
(out/'loop-submit.inc').write_text('#define LOOP_SECTOR_SIZE 512U\n#define LOOP_MAX_TRANSFER_BLOCKS 128U\n'+loop[a:b]+'\n'+loop[c:d])
e=loop.index('int\nloop_backing_disk_ref(');f=loop.index('\nint\nloop_get_index(',e)
with (out/'loop-submit.inc').open('a') as include:
 include.write('\nstatic struct loop_device loops[LOOP_MAX_DEVICES];\nstatic struct spinlock loop_lock;\nstatic const struct disk_ops loop_disk_ops={.submit=loop_submit};\n'+loop[e:f])
sources=['src/kern/writeback-domain.c','src/drivers/disklabel/mbr.c','src/kern/io-stats.c','src/kern/io-error.c','plan/ws025-io-memory-cache/tests/async-bio-host.c']
(out/'source.json').write_text(json.dumps({s:hashlib.sha256((repo/s).read_bytes()).hexdigest() for s in [*sources,"src/kern/disk.c","src/kern/mount.c","include/kern/disk.h","include/kern/mount.h","include/kern/io-epoch.h","plan/ws019-installation/tests/storage-foundation-test.c"]},indent=2)+'\n')
commands=[]
for variant in ('ordinary','sanitize'):
 extra=[] if variant=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','--param','asan-globals=0']
 bridge=str(out/(variant+'-thread.o'))
 bridge_command=['cc','-O1','-g','-pthread',*extra,'-c','plan/ws018-kernel-architecture/tests/mount-thread-host.c','-o',bridge]
 commands.append(dict(name=variant+'-thread',argv=bridge_command))
 (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
 subprocess.run(bridge_command,cwd=repo,check=True)
 exit_bridge=str(out/(variant+'-exit.o'))
 subprocess.run(['cc','-O1','-g','-pthread',*extra,'-c','plan/ws025-io-memory-cache/tests/async-thread-host.c','-o',exit_bridge],cwd=repo,check=True)
 binary=str(out/variant)
 build=['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-DZEDBSD_USER_ABI_LP64','-DZEDBSD_STORAGE_HOST_TEST','-ffunction-sections','-fdata-sections','-I.','-Iinclude','-Iinclude/uapi','-Isrc','-Ilibc/include','-I'+str(out),*extra,*sources,bridge,exit_bridge,'-pthread','-Wl,--gc-sections','-o',binary]
 for name,args in ((variant+'-build',build),(variant,['timeout','60s',binary])):
  commands.append(dict(name=name,argv=args));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
  r=subprocess.run(args,cwd=repo,capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
  (out/(name+'.log')).write_text(r.stdout+r.stderr);print(name,r.returncode,r.stdout[-800:],r.stderr[-1000:],flush=True)
  if r.returncode:sys.exit(1)

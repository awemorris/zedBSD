#!/usr/bin/env python3
"""Real VM/file/reclaim owners with deterministic physical mappings."""
from pathlib import Path
import hashlib,json,os,subprocess,sys
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws025/temp');out.mkdir(exist_ok=False)
sources=['file','filedesc','readahead','backing-claim','vm','cache','io','writeback','vmspace','elf']
paths=['src/kern/'+s+'.c' for s in sources]+['plan/ws025/tests/exec-snapshot-vm-host.c','plan/ws025/tests/file-cache-host.c','plan/ws019/tests/format-reservation-test.c','include/kern/io-destination.h','include/kern/file.h','include/kern/vm-object.h','include/kern/vmspace.h','include/kern/vm-reclaim.h','plan/ws025/tests/run-exec-snapshot-vm-host.py']
(out/'source.json').write_text(json.dumps({p:hashlib.sha256((repo/p).read_bytes()).hexdigest() for p in paths},indent=2)+'\n')
for variant in ('ordinary','sanitize'):
 flags=[] if variant=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','--param=asan-globals=0','-no-pie']
 common=['cc','-std=c11','-O0','-g','-pthread','-Dtid_t=int32_t','-DZEDBSD_USER_ABI_LP64','-DHAL_ARCH_AMD64','-Iinclude','-Iinclude/uapi','-I.','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',*flags]
 with (out/(variant+'.log')).open('w') as log:
  objects=[]
  for source in sources:
   obj=out/(variant+'-'+source+'.o');objects.append(str(obj))
   subprocess.run([*common,'-c','src/kern/'+source+'.c','-o',str(obj)],cwd=repo,stdout=log,stderr=subprocess.STDOUT,check=True)
   symbols={'vm':['vm_commit_release','vm_commit_reserve','vm_metadata_enter','vm_metadata_leave','vm_metadata_init','vm_metadata_owned'], 'readahead':['readahead_consumed','readahead_demand_begin','readahead_demand_end','readahead_cancel','readahead_submit'], 'writeback':['writeback_mount_admit','writeback_pressure'], 'io':['io_pool_borrow','io_pool_release']}.get(source,[])
   if symbols:subprocess.run(['objcopy',*['--weaken-symbol='+s for s in symbols],str(obj)],check=True)
  binary=out/variant
  subprocess.run([*common,*objects,paths[len(sources)],'-Wl,--gc-sections','-pthread','-o',str(binary)],cwd=repo,stdout=log,stderr=subprocess.STDOUT,check=True)
  subprocess.run(['timeout','60',str(binary)],cwd=repo,stdout=log,stderr=subprocess.STDOUT,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
 print(variant+': PASS')

#!/usr/bin/env python3
"""Production UFS allocation and journal failure/recovery boundaries."""
__import__('runpy').run_path(str(__import__('pathlib').Path(__file__).resolve().parents[3] / 'plan/ws025-io-memory-cache/tests/prepare-driver-fragments.py'), run_name='__main__')
from pathlib import Path
import os,subprocess,sys
repo=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(repo/'plan/ws025-io-memory-cache/temp');out.mkdir(exist_ok=False)
fixture=sys.argv[2] if len(sys.argv)>2 else 'allocation-journal-host.c'
deferred = len(sys.argv)>3 and sys.argv[3]=='--deferred'
if len(sys.argv)>3 and not deferred:raise ValueError('unknown mode')
if fixture not in ('allocation-journal-host.c','release-journal-host.c','unlink-journal-host.c','link-journal-host.c','rmdir-journal-host.c','metadata-images-host.c','rename-journal-host.c','retire-journal-host.c','xattr-release-journal-host.c','xattr-replace-journal-host.c','xattr-allocation-journal-host.c','inode-reservation-journal-host.c','creation-cleanup-journal-host.c','directory-backing-journal-host.c','creation-journal-host.c','creation-vfs-journal-host.c','orphan-journal-host.c','journal-view-vfs-host.c'):raise ValueError('unknown fixture')
for variant in ('sanitize','ordinary'):
 extra=[] if variant=='ordinary' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','--param','asan-globals=0']
 if deferred:extra+=['-DUFS_AUDIT_DEFERRED_DEFAULT=1']
 bridge=out/(variant+'-thread.o')
 subprocess.run(['cc','-DZEDBSD_USER_ABI_LP64',*extra,'-O1','-g','-pthread','-c','plan/ws018-kernel-architecture/tests/mount-thread-host.c','-o',str(bridge)],cwd=repo,check=True)
 jump=out/(variant+'-crash.o')
 subprocess.run(['cc',*extra,'-O1','-g','-c','plan/ws025-io-memory-cache/tests/allocation-crash-bridge.c','-o',str(jump)],cwd=repo,check=True)
 extra_sources=['src/kern/namecache.c'] if fixture in ('unlink-journal-host.c','link-journal-host.c','rmdir-journal-host.c','rename-journal-host.c','creation-journal-host.c','creation-vfs-journal-host.c') else []
 binary=out/variant
 subprocess.run(['cc','-DZEDBSD_USER_ABI_LP64',*extra,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-I.','-Iinclude','-Iinclude/uapi','-Isrc','-Ilibc/include','-Iplan/ws018-kernel-architecture/tests','-Iplan/ws025-io-memory-cache/temp/p031-driver-fragments/src/drivers/fs/ufs','plan/ws025-io-memory-cache/tests/'+fixture,'plan/ws025-io-memory-cache/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-endian.c','plan/ws025-io-memory-cache/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-journal.c','src/kern/quota.c','src/kern/io-stats.c',*extra_sources,str(bridge),str(jump),'-pthread','-Wl,--gc-sections','-o',str(binary)],cwd=repo,check=True)
 subprocess.run(['timeout','90',str(binary)],cwd=repo,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})

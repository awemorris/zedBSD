#!/usr/bin/env python3
"""Run existing lifecycle assertions against the complete current NVMe source."""
from pathlib import Path
import os,subprocess,sys
r=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.relative_to(r/'plan/ws004/temp');out.mkdir(parents=True)
for name in ['nvme-lifecycle-test','nvme-io-lifecycle-test','nvme-shutdown-lifecycle-test']:
 source=(r/'plan/ws004/tests'/f'{name}.c').read_text()
 lines=source.splitlines(True)
 lines=['#include "src/drivers/pci/pci-nvme.c"\n' if line.startswith('#include ') and 'p031-driver-fragments' in line else line for line in lines]
 fixture=out/(name+'.c');fixture.write_text(''.join(lines))
 for mode in ['normal','sanitize']:
  binary=out/(name+'-'+mode)
  flags=[] if mode=='normal' else ['-fsanitize=address,undefined','-fno-omit-frame-pointer','--param=asan-globals=0']
  subprocess.run(['cc','-std=c11','-pthread','-O1','-g','-Dtid_t=int32_t','-DHAL_ARCH_AMD64','-DKERN_USER_ABI_LP64','-I'+str(r),'-I'+str(r/'include'),'-I'+str(r/'include/uapi'),'-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',*flags,str(fixture),'-Wl,--gc-sections','-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
  print(name,mode,'PASS',flush=True)

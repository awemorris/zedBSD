#!/usr/bin/env python3
"""Compile actual private backing owners with controlled IRQ/wakeup services."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys

repo = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(repo / 'plan/ws025/temp')
out.mkdir(exist_ok=False)
paths = ['src/kern/vm.c', 'include/kern/vm-reclaim.h', 'include/kern/vmspace.h',
         'plan/ws025/tests/private-upgrade-host.c',
         'plan/ws025/tests/run-private-upgrade-host.py']
(out / 'source.json').write_text(json.dumps({p: hashlib.sha256((repo / p).read_bytes()).hexdigest() for p in paths}, indent=2) + '\n')
for mode in ('ordinary', 'sanitize'):
    flags = [] if mode == 'ordinary' else ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
    binary = out / mode
    args = ['cc', '-std=c11', '-g', '-O0', '-pthread', '-Dtid_t=int32_t', '-DZEDBSD_USER_ABI_LP64',
            '-Iinclude', '-Iinclude/uapi', '-I.', '-Wall', '-Wextra', '-Werror',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', *flags,
            paths[0], paths[3], '-o', str(binary)]
    with (out / (mode + '.log')).open('w') as log:
        subprocess.run(args, cwd=repo, stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(['timeout', '60', str(binary)], cwd=repo, stdout=log,
                       stderr=subprocess.STDOUT, check=True,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})
    print(mode + ': PASS')

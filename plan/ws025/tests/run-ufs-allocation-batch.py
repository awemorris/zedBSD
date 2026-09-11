#!/usr/bin/env python3
"""Run the operation-local UFS allocation/publication fault matrix."""
__import__('runpy').run_path(str(__import__('pathlib').Path(__file__).resolve().parents[3] / 'plan/ws025/tests/prepare-driver-fragments.py'), run_name='__main__')
from pathlib import Path
import os
import subprocess
import sys
root = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(root / 'plan/ws025/temp')
out.mkdir(parents=True, exist_ok=False)
for variant in ('ordinary', 'sanitize'):
    extra = [] if variant == 'ordinary' else ['-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', '--param', 'asan-globals=0']
    bridge = out / (variant + '-thread.o')
    subprocess.run(['cc', '-DZEDBSD_USER_ABI_LP64', '-O1', '-g', '-pthread',
        *extra, '-c', 'plan/ws018/tests/mount-thread-host.c',
        '-o', str(bridge)], cwd=root, check=True)
    binary = out / variant
    subprocess.run(['cc', '-DZEDBSD_USER_ABI_LP64', '-std=c11', '-O1', '-g',
        '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
        *extra, '-I.', '-Iinclude', '-Iinclude/uapi', '-Isrc', '-Ilibc/include',
        '-Iplan/ws018/tests', '-Iplan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs',
        'plan/ws025/tests/ufs-allocation-batch-host.c',
        'plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-endian.c', 'src/kern/quota.c', 'src/kern/io-stats.c',
        str(bridge), '-pthread', '-Wl,--gc-sections', '-o', str(binary)], cwd=root, check=True)
    with (out / (variant + '.log')).open('w') as log:
        subprocess.run(['timeout', '90s', str(binary)], stdout=log,
            stderr=subprocess.STDOUT, check=True, cwd=root,
            env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1',
                 'UBSAN_OPTIONS': 'halt_on_error=1'})
    print(variant, (out / (variant + '.log')).read_text(), flush=True)

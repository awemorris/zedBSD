#!/usr/bin/env python3
"""Compile the actual journal against volatile/torn-write crash media."""
__import__('runpy').run_path(str(__import__('pathlib').Path(__file__).resolve().parents[3] / 'plan/ws025/tests/prepare-driver-fragments.py'), run_name='__main__')
from pathlib import Path
import os
import subprocess
import sys
repo = Path(__file__).resolve().parents[3]
output = Path(sys.argv[1]).resolve()
output.relative_to(repo / 'plan/ws025/temp')
output.mkdir(parents=True, exist_ok=False)
for variant in ('ordinary', 'sanitize'):
    binary = output / variant
    extra = [] if variant == 'ordinary' else ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    command = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-I.', '-Isrc',
               *extra, 'plan/ws025/tests/journal-crash-host.c',
               'plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-journal.c', '-o', str(binary)]
    subprocess.run(command, cwd=repo, check=True)
    subprocess.run(['timeout', '60', str(binary)], cwd=repo, check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})

    snapshot = output / ('snapshot-' + variant)
    snapshot_extra = [] if variant == 'ordinary' else ['--param', 'asan-globals=0']
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-ffunction-sections', '-fdata-sections', '-I.', '-Iinclude',
                    '-Iinclude/uapi', '-Isrc', '-DKERN_USER_ABI_LP64', '-Ilibc/include',
                    *extra, *snapshot_extra,
                    'plan/ws025/tests/snapshot-flush-host.c',
                    '-Wl,--gc-sections', '-o', str(snapshot)], cwd=repo, check=True)
    subprocess.run(['timeout', '60', str(snapshot)], cwd=repo, check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})

    owner = output / ('image-owner-' + variant)
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-ffunction-sections', '-fdata-sections', '-I.', '-Iinclude',
                    '-Iinclude/uapi', '-Isrc', '-DKERN_USER_ABI_LP64', '-Ilibc/include',
                    *extra, *snapshot_extra,
                    'plan/ws025/tests/journal-image-owner-host.c',
                    'plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-journal.c',
                    '-Wl,--gc-sections', '-o', str(owner)], cwd=repo, check=True)
    subprocess.run(['timeout', '60', str(owner)], cwd=repo, check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})

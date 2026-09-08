#!/usr/bin/env python3
"""Run FAT transaction fault and public create/append/unlink cost gates."""
__import__('runpy').run_path(str(__import__('pathlib').Path(__file__).resolve().parents[3] / 'plan/ws025-io-memory-cache/tests/prepare-driver-fragments.py'), run_name='__main__')
from pathlib import Path
import subprocess
import sys
import os
import json
import hashlib
root = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(root / 'plan/ws025-io-memory-cache/temp')
out.mkdir(parents=True, exist_ok=False)
tracked = ['plan/ws025-io-memory-cache/temp/p031-driver-fragments/src/drivers/fs/fat.c', 'plan/ws025-io-memory-cache/temp/p031-driver-fragments/src/drivers/fs/fat-batch.inc',
    'plan/ws025-io-memory-cache/tests/fat-batch-private-host.c',
    'plan/ws025-io-memory-cache/tests/fat-batch-cost-host.c',
    'plan/ws018-kernel-architecture/tests/fat-native-vfs-host-test.c']
(out / 'source.json').write_text(json.dumps({p: hashlib.sha256((root / p).read_bytes()).hexdigest()
    for p in tracked}, indent=2) + '\n')
for variant in ('ordinary', 'sanitize'):
    extra = [] if variant == 'ordinary' else ['-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', '--param', 'asan-globals=0']
    for kind in ('private', 'cost'):
        binary = out / (variant + '-' + kind)
        sources = [f'plan/ws025-io-memory-cache/tests/fat-batch-{kind}-host.c', 'src/kern/io-stats.c']
        if kind == 'cost':
            sources.append('plan/ws025-io-memory-cache/temp/p031-driver-fragments/src/drivers/fs/fat.c')
        subprocess.run(['cc', '-std=c11', '-DZEDBSD_USER_ABI_LP64', '-O1', '-g',
            '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
            '-Iinclude', '-Iinclude/uapi', '-Isrc', '-Ilibc/include', '-I.',
            *extra, *sources, '-Wl,--gc-sections', '-o', str(binary)], cwd=root, check=True)
        with (out / (binary.name + '.log')).open('w') as log:
            subprocess.run(['timeout', '90s', str(binary)], cwd=root, check=True,
                stdout=log, stderr=subprocess.STDOUT,
                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1',
                    'UBSAN_OPTIONS': 'halt_on_error=1'})
        print(binary.name, (out / (binary.name + '.log')).read_text(), flush=True)

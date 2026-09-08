#!/usr/bin/env python3
"""Verify mandatory pool/DMA backing against the real shared accounting owner."""
__import__('runpy').run_path(str(__import__('pathlib').Path(__file__).resolve().parents[3] / 'plan/ws025-io-memory-cache/tests/prepare-driver-fragments.py'), run_name='__main__')
import json
import os
from pathlib import Path
import subprocess
import sys
repo = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(repo / 'plan/ws025-io-memory-cache/temp')
out.mkdir(parents=True, exist_ok=False)
commands = []
for variant in ('ordinary', 'sanitize'):
    flags = [] if variant == 'ordinary' else ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
    for name, sources in {
        'dma': ['plan/ws025-io-memory-cache/temp/p031-driver-fragments/src/drivers/dma.c', 'src/hal/amd64/pmem-range.c'],
        'pool': ['src/kern/io-pool.c','src/kern/io-scratch.c'],
        'worker': ['src/kern/cache-worker.c','src/kern/io-scratch.c'],
    }.items():
        binary = out / (name + '-' + variant)
        build = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread', *flags,
                 '-Iinclude', '-Iinclude/uapi', '-I.',
                 f'plan/ws025-io-memory-cache/tests/cache-{name}-host.c',
                 'src/kern/cache-memory.c', 'src/kern/io-stats.c', *sources, '-o', str(binary)]
        steps = [('build', build), ('run', [str(binary)])]
        if name == 'pool':
            steps += [(f'failure-{n}', [str(binary), str(n)]) for n in (1, 2, 3, 17)]
        for label, argv in steps:
            commands.append(argv)
            (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
            with (out / (binary.name + '-' + label + '.log')).open('w') as log:
                result = subprocess.run(argv, cwd=repo, stdout=log, stderr=subprocess.STDOUT,
                    env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})
            if result.returncode:
                raise SystemExit(f'FAIL: {binary.name} {label}: {result.returncode}')
        print('PASS:', binary.name, flush=True)

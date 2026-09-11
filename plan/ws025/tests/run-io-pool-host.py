#!/usr/bin/env python3
"""Verify the production pool and verbatim production exec-copy function."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

repo = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(repo / 'plan/ws025/temp')
out.mkdir(parents=True, exist_ok=False)
source = (repo / 'src/kern/elf.c').read_text()
start = source.index('\ncopy_segment_snapshot(') + 1
body = source.index('{', start)
end, depth = body + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
(out / 'exec-copy-extracted.h').write_text('static int\n' + source[start:end] + '\n')
(out / 'source.sha256').write_text(hashlib.sha256(source.encode()).hexdigest() + '  src/kern/elf.c\n')
commands = []
for variant in ('ordinary', 'sanitize'):
    flags = [] if variant == 'ordinary' else ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
    for name in ('io-pool', 'exec-pool'):
        binary = out / (name + '-' + variant)
        build = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread', *flags,
                 '-Iinclude', '-Iinclude/uapi', '-I.', '-I' + str(out),
                 f'plan/ws025/tests/{name}-host.c', 'src/kern/io-pool.c','src/kern/io-scratch.c', 'src/kern/io-stats.c', '-o', str(binary)]
        for suffix, argv in [('build', build), ('run', [str(binary)])]:
            commands.append(argv)
            (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
            with (out / (binary.name + '-' + suffix + '.log')).open('w') as log:
                result = subprocess.run(argv, cwd=repo, stdout=log, stderr=subprocess.STDOUT,
                    env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})
            if result.returncode:
                raise SystemExit(f'FAIL: {binary.name} {suffix}: {result.returncode}')
        print('PASS:', binary.name, flush=True)

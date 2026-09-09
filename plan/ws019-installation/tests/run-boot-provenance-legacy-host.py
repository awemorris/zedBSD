#!/usr/bin/env python3
"""Focused existing memory/parameter regressions, without driver extraction."""
from pathlib import Path
import json
import os
import subprocess
import sys
repo = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(repo / 'plan/ws019-installation/temp')
out.mkdir(parents=True, exist_ok=False)
groups = {
    'memory': ['plan/ws025-io-memory-cache/tests/memory-handoff-host.c',
        'bootloader/common/memory-map.c', 'bootloader/bios/memory-map.c',
        'bootloader/uefi/memory-map-v6.c', 'src/hal/amd64/bsp-pcat/handoff-validation.c'],
    'parameters': ['plan/ws003-bringup/tests/x86-parameter-handoff-test.c',
        'src/hal/x86/boot-parameters.c', 'src/hal/amd64/bsp-pcat/handoff-validation.c']}
commands = []
for variant in ('ordinary', 'sanitize'):
    extra = [] if variant == 'ordinary' else ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
    for name, sources in groups.items():
        label = name + '-' + variant
        binary = out / label
        for suffix, args in [('build', ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-I.', '-Iinclude', '-Iinclude/uapi', *extra, *sources, '-o', str(binary)]), ('run', [str(binary)])]:
            commands.append(args)
            (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
            result = subprocess.run(args, cwd=repo, capture_output=True, text=True,
                env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=1', 'UBSAN_OPTIONS':'halt_on_error=1'})
            (out / (label + '-' + suffix + '.log')).write_text(result.stdout + result.stderr)
            if result.returncode: raise RuntimeError(label + '-' + suffix + ': ' + result.stderr)
        print(label, 'PASS', flush=True)

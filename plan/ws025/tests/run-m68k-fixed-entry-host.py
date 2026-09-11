#!/usr/bin/env python3
"""Check actual m68k dispatch units with saved frames and mocked callbacks."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

repo = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(repo / 'plan/ws025/temp')
out.mkdir(exist_ok=False)
sources = [
    'src/hal/m68k/trap.c',
    'src/hal/m68k/exception.c',
    'plan/ws025/tests/m68k-fixed-entry-host.c',
]
inputs = sources + [
    'include/hal/hal.h', 'src/hal/m68k/exception.h',
    'src/hal/m68k/frame-offsets.h',
]
(out / 'source.json').write_text(json.dumps({
    p: hashlib.sha256((repo / p).read_bytes()).hexdigest() for p in inputs
}, indent=2) + '\n')
for mode, flags in [
    ('ordinary', []),
    ('sanitize', ['-fsanitize=address,undefined',
                  '-fno-omit-frame-pointer', '-no-pie']),
]:
    with (out / (mode + '.log')).open('w') as log:
        subprocess.run([
            'cc', '-std=c11', '-Dtid_t=int32_t', '-Iinclude',
            '-Iinclude/uapi', '-I.', '-ffunction-sections',
            '-fdata-sections', *flags, *sources, '-Wl,--gc-sections',
            '-o', str(out / mode),
        ], cwd=repo, stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run([str(out / mode)], stdout=log,
                       stderr=subprocess.STDOUT, check=True)
with (out / 'target.log').open('w') as log:
    subprocess.run([
        '/usr/bin/clang', '--target=m68k-unknown-none', '-ffreestanding',
        '-nostdinc', '-Ilibc/include', '-Iinclude', '-Iinclude/uapi',
        '-DHAL_ARCH_M68K', '-fsyntax-only', *sources[:2],
    ], cwd=repo, stdout=log, stderr=subprocess.STDOUT, check=True)
print('m68k dispatch ordinary/sanitizer and target syntax: PASS')

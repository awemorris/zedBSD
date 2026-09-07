#!/usr/bin/env python3
"""Exercise the production core with deterministic checkpoint/read concurrency."""
from pathlib import Path
import os
import subprocess
import sys

repo = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(repo / 'plan/ws025-io-memory-cache/temp')
out.mkdir(exist_ok=False)
for mode in ('sanitize', 'ordinary'):
    extra = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if mode == 'sanitize' else []
    binary = out / mode
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-I.', '-pthread', *extra,
        'plan/ws025-io-memory-cache/tests/journal-view-host.c',
        'src/drivers/fs/ufs/ufs-journal.c',
        'plan/ws018-kernel-architecture/tests/mount-thread-host.c',
        '-o', str(binary)], cwd=repo, check=True)
    subprocess.run(['timeout', '60', str(binary)], cwd=repo, check=True,
        env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})

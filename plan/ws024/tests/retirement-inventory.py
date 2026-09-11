#!/usr/bin/env python3
"""Audit production source ownership and the supported kernels after retirement."""
from pathlib import Path
import subprocess
import json
import re
import hashlib
import sys
root = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(root / 'plan/ws024/temp')
areas = ['src', 'include', 'tools/build', 'userland/base/mkfs', 'platform']
names = subprocess.check_output(['git', 'ls-files', '--cached', '--others',
    '--exclude-standard', *areas], cwd=root, text=True).splitlines()
hits = []
for name in sorted(set(names)):
    p = root / name
    if not p.is_file():
        continue
    try:
        source = p.read_text()
    except UnicodeError:
        continue
    for number, line in enumerate(source.splitlines(), 1):
        if re.search('ufs[12]', line, re.I):
            hits.append([name, number, line])
assert all(name == 'include/uapi/zedbsd/io-stats.h' and 'IO_UFS' in line
           for name, number, line in hits), hits
symbols = {}
for arch in ('amd64', 'pcat', 'pc98'):
    p = root / 'build' / arch / 'vmunix'
    result = subprocess.check_output(['nm', str(p)], text=True)
    assert not re.search(r'\bufs[12]_', result), arch
    selected = [line for line in result.splitlines()
                if re.search(r'\bufs_(filesystem_type|identify)$', line)]
    assert len(selected) == 2, (arch, selected)
    symbols[arch] = {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(),
                     'symbols': selected}
active = subprocess.run(['rg', '-n',
    r'(src/drivers/fs/ufs[12]|include/kern/ufs[12]|tools/build/(ufs[12]_format|make-.*ufs[12]|check-.*ufs[12]))',
    'plan', '--glob', '*.py', '--glob', '*.sh', '--glob', '*.mk', '--glob', '*.c',
    '--glob', '*.noct', '--glob', '!**/temp/**'], cwd=root, capture_output=True, text=True)
# This audit script contains the deliberately matched patterns as regex syntax.
assert active.returncode == 1, active.stdout + active.stderr
out.write_text(json.dumps({'status': 'PASS', 'allowed_historical_stat_ids': hits,
    'kernel_symbols': symbols, 'retired_active_path_references': [],
    'source_selection': 'existing git tracked and nonignored untracked production files'},
    indent=2) + '\n')
print('Unified source, active source paths and three kernel symbol tables PASS')

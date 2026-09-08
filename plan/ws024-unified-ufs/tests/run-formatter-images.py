#!/usr/bin/env python3
"""Compare target, Noct backend and Python UFS producers in disposable files."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import subprocess
import sys

repo = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(repo / 'plan/ws024-unified-ufs/temp')
out.mkdir(exist_ok=False)
sys.path.insert(0, str(repo / 'tools/build'))
from ufs_format import create
from overlay_journal_format import empty_active_slot, JOURNAL_BYTES
spec = importlib.util.spec_from_file_location('ufs_checker', repo / 'tools/build/check-ufs-image.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
commands = []
results = []

def run(name, argv):
    commands.append(dict(name=name, argv=argv))
    (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    result = subprocess.run(argv, cwd=repo, capture_output=True, text=True,
        env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=1', 'UBSAN_OPTIONS':'halt_on_error=1'})
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(name + ': ' + result.stdout + result.stderr)
    print(name, 'PASS', flush=True)

root = out / 'root'
root.mkdir()
(root / '.zovl0').write_bytes(empty_active_slot('overlay'))
(root / '.zovl1').write_bytes(bytes(JOURNAL_BYTES))
for item in root.iterdir():
    item.chmod(0o644)
run('backend-build', ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
    'tools/build/zedimage-host.c', '-o', str(out / 'backend')])
for variant in ['ordinary', 'sanitize']:
    extra = [] if variant == 'ordinary' else ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    binary = str(out / ('target-' + variant))
    run(variant + '-build', ['cc', '-std=c89', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g',
        '-Wall', '-Wextra', '-Werror', '-I.', *extra,
        'plan/ws024-unified-ufs/tests/formatter-image-host.c',
        'userland/base/mkfs/ufs-format.c', 'userland/base/mkfs/ufs-super.c',
        'userland/base/mkfs/ufs-endian.c', '-o', binary])
    for size in [4 * 1024**2, 32 * 1024**2, 192 * 1024**2]:
        for feature in [0, 1]:
            label = f'{variant}-{size}-{feature}'
            target = out / (label + '.img')
            run(label, [binary, str(target), str(size), str(feature)])
            data = target.read_bytes()
            fs = checker.UFS(data)
            fs.validate()
            assert fs.read_file(fs.lookup('/.zovl0')) == empty_active_slot('overlay')
            assert fs.read_file(fs.lookup('/.zovl1')) == bytes(JOURNAL_BYTES)
            assert fs.read_file(fs.lookup('/etc/zedbsd-root')) == b'zedBSD ufs root v1\n'
            if variant == 'ordinary':
                host = out / (label + '-host.img')
                profile_args = ['--profile=journal-snapshot'] if feature else []
                run(label + '-backend', [str(out / 'backend'), 'ufs', str(size), str(root), str(host), *profile_args])
                assert host.read_bytes() == data, 'target/backend mismatch'
                profile = 'journal-snapshot' if feature else 'ordinary'
                py = bytearray(create(size, root, profile=profile))
                # Python assigns a fresh fs_id; compare all other initialized bytes.
                for cg in range(fs.ncg):
                    offset = (cg * fs.fpg + 64) * 1024 + 144
                    py[offset:offset+8] = data[offset:offset+8]
                assert py == data, 'target/Python mismatch outside fs_id'
                host.unlink()
            results.append(dict(cell=label, status='PASS', groups=fs.ncg,
                sha256=hashlib.sha256(data).hexdigest()))
            target.unlink()
    # Exercise the actual producer maximum without reading a 2 GiB file into RAM.
    maximum = out / (variant + '-maximum.img')
    run(variant + '-maximum', [binary, str(maximum), '2147482624', '1'])
    maximum.unlink()
(out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
print('12 target cells, six three-producer comparisons, two sparse maximum gates PASS')

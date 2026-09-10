#!/usr/bin/env python3
"""Verify independent formatter compilation and debug-command installation."""
from pathlib import Path
import json
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
output = Path(sys.argv[1]).resolve()
output.relative_to(REPO / 'plan/ws025-io-memory-cache/temp')
output.mkdir(parents=True, exist_ok=False)
isolated = output / 'isolated'
commands = []


def run(command):
    commands.append(command)
    (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    result = subprocess.run(command, cwd=isolated, text=True, capture_output=True)
    with (output / 'commands.log').open('a') as log:
        log.write(repr(command) + '\n' + result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(str(command) + '\n' + result.stderr)
    return result.stdout


# Copy only userland sources and the published reservation/block ABI headers.
for name in ('mkfs', 'cflow', 'cxref', 'common'):
    directory = isolated / 'userland/base' / name
    directory.mkdir(parents=True)
    for source in (REPO / 'userland/base' / name).iterdir():
        if source.suffix in ('.c', '.h'):
            shutil.copy2(source, directory / source.name)
header = isolated / 'include/zedbsd/fcntl.h'
header.parent.mkdir(parents=True)
shutil.copy2(REPO / 'include/uapi/zedbsd/fcntl.h', header)
shutil.copy2(REPO / 'include/uapi/zedbsd/block.h', header.with_name('block.h'))
assert not (isolated / 'src').exists()
formatter = ['userland/base/mkfs/' + name for name in
             ('main.c', 'ufs-format.c', 'ufs-super.c', 'ufs-endian.c',
              'block-command.c', 'fat32-format.c')]
run(['cc', '-std=c89', '-D_GNU_SOURCE', '-O2', '-Wall', '-Wextra', '-Werror',
     '-I.', '-Iinclude', *formatter, 'userland/base/common/format-file.c',
     '-o', 'mkfs'])
(isolated / 'sample.c').write_text('int leaf(void) { return 7; }\nint main(void) { return leaf(); }\n')
for name in ('cflow', 'cxref'):
    run(['cc', '-std=c89', '-D_GNU_SOURCE', '-O2', '-Wall', '-Wextra', '-Werror',
         '-I.', 'userland/base/' + name + '/main.c',
         'userland/base/common/c_parser.c', '-o', name])
    result = run(['./' + name, 'sample.c'])
    assert 'main' in result and 'leaf' in result, result
    run(['make', '-j16', '-C', str(REPO / 'userland/base' / name),
         'install', 'PREFIX=/', 'DESTDIR=' + str(output / 'install')])
    installed = output / 'install/lib/debug' / name
    assert installed.is_file()
    assert not (output / 'install/bin' / name).exists()

for platform in ('amd64', 'pcat', 'pc98'):
    root = REPO / 'build' / platform / 'rootfs'
    for name in ('cflow', 'cxref'):
        assert (root / 'lib/debug' / name).is_file()
        assert not (root / 'bin' / name).exists()
    assert not (root / 'bin/nettest').exists()
    for name in ('init', 'mkfs', 'mount', 'net', 'networkd', 'sysctl'):
        assert (root / 'sbin' / name).is_file(), (platform, name)
(output / 'results.json').write_text(json.dumps({
    'kernel-free mkfs host compilation': 'PASS',
    'cflow/cxref host commands': 'PASS',
    'standalone debug installation': 'PASS',
    'three-platform rootfs placement': 'PASS',
    'host reservation runtime': 'not provided; foreign OS ports must adapt the frontend'
}, indent=2) + '\n')
print('p031 userland independence and packaging: PASS')

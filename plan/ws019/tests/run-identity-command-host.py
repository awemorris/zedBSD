#!/usr/bin/env python3
"""Real stat identity and deterministic blkid error/escaping acceptance."""
from pathlib import Path
import json
import os
import subprocess
import sys
repo = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(repo / 'plan/ws019/temp')
out.mkdir(parents=True, exist_ok=False)
source = repo / 'plan/ws019/tests/identity-command-host.c'
checks = 0
for variant in ('ordinary', 'sanitize'):
    flags = [] if variant == 'ordinary' else ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1', 'UBSAN_OPTIONS': 'halt_on_error=1'}
    for command in ('stat', 'blkid'):
        binary = out / (variant + '-' + command)
        args = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *flags,
                '-I' + str(repo), '-idirafter', str(repo / 'include/uapi')]
        if command == 'stat': args += ['-DTEST_STAT']
        subprocess.run([*args, str(source), '-o', str(binary)], check=True)
    def run(command, args, expected, extra=None):
        global checks
        result = subprocess.run([str(out / (variant + '-' + command)), *args], capture_output=True,
                                text=True, env={**env, **(extra or {})}, timeout=10)
        assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
        checks += 1
        return result.stdout
    path = out / (variant + ' file;literal')
    path.write_bytes(b'abc')
    path.chmod(0o640)
    link = out / (variant + '-link'); link.symlink_to(path.name)
    hard = out / (variant + '-hard'); os.link(path, hard)
    fmt = '%d:%i:%f:%s:%a:%u:%g'
    for target in (path, link, hard):
        st = target.lstat()
        expected = f'{st.st_dev}:{st.st_ino}:{st.st_mode:x}:{st.st_size}:{st.st_mode & 0o7777:o}:{st.st_uid}:{st.st_gid}\n'
        assert run('stat', ['-c', fmt, '--', str(target)], 0) == expected
    assert run('stat', ['-c', '%n %%', '--', str(path)], 0) == str(path) + ' %\n'
    assert run('stat', ['-c', '%Q', str(path)], 2) == ''
    assert run('stat', ['-c', '%', str(path)], 2) == ''
    run('stat', ['-c', '%s', str(out / 'missing')], 1)
    export = ['-o', 'export', '-s', 'TYPE', '-s', 'PARTUUID', '/dev/mock']
    assert run('blkid', export, 0) == 'DEVNAME=/dev/mock\nTYPE=vfat\nPARTUUID=12345678-1234-4321-8765-123456789abc\n\n'
    assert run('blkid', ['-o', 'export', '-s', 'LABEL', '/dev/mock'], 0) == 'DEVNAME=/dev/mock\nLABEL=a\\x20b\\x0ac\\x3dd\\x5ce\n\n'
    for extra in ({'QUERY_ERROR':'1'}, {'CLOSE_ERROR':'1'}):
        assert run('blkid', export, 1, extra) == ''
    assert run('blkid', export, 2, {'UNKNOWN':'1'}) == ''
    assert run('blkid', ['/dev/mock'], 0, {'UNKNOWN':'1'}) == ''
    assert run('blkid', ['-s', 'UNKNOWN', '/dev/mock'], 2) == ''
    assert run('blkid', ['-o', 'bogus', '/dev/mock'], 2) == ''
    assert run('blkid', ['-o', 'export', '-s', 'UUID', '/dev/mock'], 2) == 'DEVNAME=/dev/mock\n\n'
    # Buffered output failures must not be reported as a complete record stream.
    with open('/dev/full', 'wb') as full:
        for command, args in [('stat', ['-c', '%s', str(path)]), ('blkid', export)]:
            result = subprocess.run([str(out / (variant + '-' + command)), *args],
                                    stdout=full, stderr=subprocess.PIPE, env=env, timeout=10)
            assert result.returncode == 1, (command, result.returncode, result.stderr)
            checks += 1
    print(variant, 'PASS', flush=True)
(out / 'result.json').write_text(json.dumps({'result':'PASS', 'checks':checks}, indent=2) + '\n')

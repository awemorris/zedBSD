#!/usr/bin/env python3
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
repo = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(repo / 'plan/ws019-installation/temp')
out.mkdir(parents=True, exist_ok=False)
checks = 0
for variant in ('ordinary', 'sanitize'):
    extra = [] if variant == 'ordinary' else ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    binary = out / variant
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *extra,
        '-I' + str(repo), str(repo / 'plan/ws019-installation/tests/cksum-sha256-host.c'),
        str(repo / 'userland/base/common/sha256.c'), '-o', str(binary)], check=True)
    env = {**os.environ, 'ASAN_OPTIONS':'detect_leaks=1', 'UBSAN_OPTIONS':'halt_on_error=1'}
    vectors = [b'', b'abc', b'a' * 1000000]
    vectors += [bytes((i * 37 + 11) % 256 for i in range(n)) for n in (55,56,63,64,65,4095,16383,16384,16385,100000)]
    for data in vectors:
        r = subprocess.run([str(binary), '-a', 'sha256'], input=data, capture_output=True, env=env, timeout=15)
        assert r.returncode == 0 and r.stdout == (hashlib.sha256(data).hexdigest() + '  -\n').encode(), r.stderr
        checks += 1
    path = out / (variant + '-file with\nback\\slash')
    path.write_bytes(vectors[-1])
    r = subprocess.run([str(binary), '-a', 'sha256', '--', str(path)], capture_output=True, env={**env, 'SHORT_READ':'1'}, timeout=15)
    escaped = str(path).replace('\\', '\\\\').replace('\n', '\\n')
    assert r.returncode == 0 and r.stdout.decode() == '\\' + hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + escaped + '\n', r.stderr
    checks += 1
    for mode in ('READ_ERROR', 'CLOSE_ERROR'):
        r = subprocess.run([str(binary), '-a', 'sha256', str(path)], capture_output=True, env={**env, mode:'1'})
        assert r.returncode == 1, (mode, r.returncode, r.stderr)
        if mode == 'READ_ERROR': assert r.stdout == b''
        checks += 1
    for args in (['-a','unknown'], ['-a'], ['-a','sha256',str(out/'missing')], ['-a','sha256',str(out)]):
        r = subprocess.run([str(binary), *args], capture_output=True, env=env)
        assert r.returncode != 0 and r.stdout == b'', (args, r.stderr)
        checks += 1
    subprocess.run([str(binary)], env={**env, 'LENGTH_OVERFLOW':'1'}, check=True)
    checks += 1
    data = b'CRC behavior remains available\n'
    expected = subprocess.run(['/usr/bin/cksum'], input=data, capture_output=True, check=True).stdout
    actual = subprocess.run([str(binary)], input=data, capture_output=True, env=env, check=True).stdout
    assert actual == expected
    checks += 1
    with open('/dev/full','wb') as full:
        r = subprocess.run([str(binary), '-a','sha256'], input=b'abc', stdout=full, stderr=subprocess.PIPE, env=env)
        assert r.returncode == 1
        checks += 1
    print(variant, 'PASS', flush=True)
(out/'result.json').write_text(json.dumps({'result':'PASS','checks':checks},indent=2)+'\n')

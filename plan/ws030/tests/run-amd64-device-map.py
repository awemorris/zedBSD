"""Exercise actual amd64 mapping functions with allocation/TLB fault injection."""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(root / 'build')
out.mkdir(parents=True, exist_ok=False)
source = (root / 'src/hal/amd64/space.c').read_text()
names = {
    'amd64_device_vaddr': 'void *',
    'user_page_allowed': 'static int',
    'device_fixed_range': 'static int',
    'device_window_populate': 'static int',
    'device_window_clear': 'static void',
    'device_window_retire': 'static void',
    'device_window_map': 'static int',
    'device_window_unmap': 'static int',
    'hal_space_map_device': 'int',
    'hal_space_unmap_device': 'int',
    'valid_user_range': 'static int',
    'leaf_flags': 'static uint64_t',
    'hal_space_map': 'int',
    'hal_space_prot_query': 'int',
    'hal_space_prot': 'int',
    'hal_space_unmap': 'int',
}
parts = []
for name, result in names.items():
    start = source.index('\n' + name + '(') + 1
    cursor = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    parts.append(result + '\n' + source[start:cursor])
(out / 'device-map-functions.h').write_text('\n\n'.join(parts) + '\n')
constants = re.findall(r'^#define AMD64_(?:DEVICE_[A-Z_]+|USER_LIMIT) .+$', source, re.M)
start = source.index('struct amd64_device_mapping {')
end = source.index('\n};', start) + 3
(out / 'device-map-definitions.h').write_text('\n'.join(constants) + '\n' + source[start:end] + '\n')
paths = ['src/hal/amd64/space.c', 'include/hal/hal.h',
         'plan/ws030/tests/amd64-device-map.c', 'plan/ws030/tests/run-amd64-device-map.py']
(out / 'source.json').write_text(json.dumps({p: hashlib.sha256((root / p).read_bytes()).hexdigest()
                                           for p in paths}, indent=2) + '\n')
for mode in ('ordinary', 'sanitize'):
    extra = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'] if mode == 'sanitize' else []
    with (out / (mode + '.log')).open('w') as log:
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-Iinclude', '-Iinclude/uapi', '-I.', '-I' + str(out), *extra,
                        paths[2], '-o', str(out / mode)], cwd=root, stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=60)
        subprocess.run([str(out / mode)], cwd=root, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=60, env={**os.environ,
                       'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                       'UBSAN_OPTIONS': 'halt_on_error=1'})
    print(mode + ': PASS')

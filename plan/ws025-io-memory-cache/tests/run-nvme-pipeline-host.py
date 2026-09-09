#!/usr/bin/env python3
"""Whole NVMe source; inject only MMIO, sleeping and hardware recovery boundaries."""
from pathlib import Path
import hashlib, json, os, subprocess, sys
repo = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(repo / 'plan/ws025-io-memory-cache/temp')
out.mkdir(parents=True, exist_ok=False)
source = (repo / 'src/drivers/pci/pci-nvme.c').read_text()
for name in ('nvme_write32', 'nvme_io_wait_locked', 'nvme_io_recover'):
    token = '\n' + name + '(\n'
    assert source.count(token) == 1, name
    source = source.replace(token, '\n' + name + '_hardware(\n')
(out / 'nvme-production.c').write_text(source)
(out / 'source.json').write_text(json.dumps({'sha256': hashlib.sha256((repo / 'src/drivers/pci/pci-nvme.c').read_bytes()).hexdigest(), 'injection': ['MMIO write', 'wait', 'hardware recovery']}, indent=2)+'\n')
for depth in (1, 2, 4, 8):
    for mode in ('ordinary', 'sanitize'):
        name = f'depth-{depth}-{mode}'
        extra = [] if mode == 'ordinary' else ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '--param', 'asan-globals=0']
        cmd = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-DZEDBSD_USER_ABI_LP64', f'-DNVME_IO_PIPELINE_DEPTH={depth}', '-ffunction-sections', '-fdata-sections', '-I.', '-Iinclude', '-Iinclude/uapi', '-Isrc', '-Ilibc/include', '-I'+str(out), *extra, 'plan/ws025-io-memory-cache/tests/nvme-pipeline-host.c', '-Wl,--gc-sections', '-o', str(out/name)]
        r = subprocess.run(cmd, cwd=repo, capture_output=True, text=True)
        (out/(name+'-build.log')).write_text(r.stdout+r.stderr)
        if r.returncode:
            print(r.stderr[-6000:]); sys.exit(r.returncode)
        r = subprocess.run(['timeout', '30s', str(out/name)], cwd=repo, capture_output=True, text=True, env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1', 'UBSAN_OPTIONS':'halt_on_error=1'})
        (out/(name+'.log')).write_text(r.stdout+r.stderr)
        print(name, r.returncode, r.stdout, r.stderr, flush=True)
        if r.returncode: sys.exit(r.returncode)
        if depth == 1 and mode == 'ordinary':
            (out / 'layout.json').write_bytes(subprocess.check_output([str(out/name), '--layout']))

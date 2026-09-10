#!/usr/bin/env python3
"""Bounded native ownership probe; uses only a disposable USB image."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import time

repo = Path(__file__).resolve().parents[3]
QMP = runpy.run_path(str(Path(__file__).with_name('capture-uas-descriptors.py')))['QMP']
parser = argparse.ArgumentParser()
parser.add_argument('output', type=Path)
parser.add_argument('--image', required=True, type=Path)
options = parser.parse_args()
out = options.output.resolve()
out.relative_to(repo / 'plan/ws025-io-memory-cache/temp')
out.mkdir(exist_ok=False)
source = options.image.resolve()
def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()
record = {'source_sha256': digest(source), 'passed': False}
paths = ['src/kern/vm.c', 'src/kern/io.c', 'src/hal/amd64/space.c',
         'include/hal/hal.h', 'tests/vmap-kernel.c']
record['sources'] = {name: digest(repo / name if not name.startswith('tests/')
    else Path(__file__).parent / Path(name).name) for name in paths}
subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(source), str(out / 'boot.img')], check=True)
shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', out / 'vars.fd')
args = ['qemu-system-x86_64', '-machine', 'q35,usb=off', '-m', '8192', '-smp', '4',
    '-display', 'none', '-serial', 'none', '-nic', 'none',
    '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
    '-drive', f'if=pflash,format=raw,file={out}/vars.fd',
    '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0,port=3',
    '-drive', f'if=none,format=raw,id=boot,file={out}/boot.img',
    '-device', 'usb-storage,bus=xhci.0,port=1,drive=boot,bootindex=1',
    '-debugcon', f'file:{out}/guest.log', '-qmp', f'unix:{out}/qmp.sock,server=on,wait=off']
(out / 'argv.json').write_text(json.dumps(args, indent=2))
with (out / 'qemu.log').open('w') as log:
    process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
    def wait(pattern):
        deadline = time.monotonic() + 150
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError('QEMU exited')
            text = (out / 'guest.log').read_text(errors='replace') if (out / 'guest.log').exists() else ''
            if re.search(r'panic:|fatal:', text, re.I):
                raise RuntimeError('guest fault')
            if re.search(pattern, text):
                return text
            time.sleep(.2)
        raise TimeoutError(pattern)
    try:
        text = wait('login:')
        assert len(re.findall(r'VMAP TABLE ROLLBACK PASS step=[12]', text)) == 2
        assert len(re.findall(r'VMAP PASS round=[012] pages=16 high=1 fragmented=1 cpus=4', text)) == 3
        assert 'VMAP SCRATCH PASS size=65536' in text
        qmp = QMP(out / 'qmp.sock')
        for key in ['r', 'o', 'o', 't', 'ret']:
            qmp.call('human-monitor-command', {'command-line': 'sendkey ' + key})
            time.sleep(.1)
        wait('Password:')
        qmp.call('human-monitor-command', {'command-line': 'sendkey ret'})
        wait(r'root[^\n]*[$#]')
        record['passed'] = True
    except Exception as error:
        record['error'] = repr(error)
        raise
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        record['qemu_exit'] = process.returncode
        record['source_unchanged'] = digest(source) == record['source_sha256']
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
        assert record['source_unchanged']
print('Native common kernel map PASS')

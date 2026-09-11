#!/usr/bin/env python3
"""Fresh FAT allocations through real Noct, cp, truncate and formatters."""
from pathlib import Path
import json
import re
import runpy
import subprocess
import sys
import time
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
f = runpy.run_path(str(HERE / 'run-formatter-qemu.py'))
class StagingGuest(f['FormatterGuest']):
    def prompt(self, start):
        return self.wait(r'root@[^\s]*:[^\n]*\$ ?$', start, 480)[0]
out = Path(sys.argv[1]).resolve()
out.relative_to(REPO / 'plan/ws019/temp')
out.mkdir(parents=True, exist_ok=False)
metadata = f['prepare_boot'](out, False, False,
    REPO / 'build/arch-images/amd64-ws019-staging.ufs')
f['create_nvme'](out / 'gpt.img')
image = f"{out / 'gpt.img'}@@1048576"
# Remove the old formatter fixture's blanks: this acceptance must allocate fresh.
for name in ('data.img', 'swapfile', 'empty', 'short'):
    subprocess.run(['mdel', '-i', image, '::/' + name], check=True)
before = f['protected_bytes'](out / 'gpt.img')
guest = StagingGuest(out, usb_boot=True)
try:
    guest.login()
    guest.run('noct --path=/usr/lib/zedinst /usr/lib/zedinst/staging-command.noct', 'Noct command PASS')
    guest.run('mount -t fat nvme0n1p1 /staging')
    start = time.monotonic()
    output = guest.run('noct --path=/usr/lib/zedinst /usr/lib/zedinst/staging-files.noct /staging', 'Noct staging PASS')
    metadata['staging_elapsed_seconds'] = time.monotonic() - start
    metadata['timing_output'] = output
    guest.run('swapon /staging/swapfile')
    guest.run('formatter-probe swap /staging/swapfile 1', 'slots=16383')
    guest.run('swapoff /staging/swapfile')
    guest.run('umount /staging')
    assert not re.search(r'panic:|fatal trap|assertion failed', guest.text(), re.I)
    metadata['result'] = 'guest functional PASS'
except BaseException as error:
    metadata['result'] = 'FAIL staging'
    metadata['failure'] = str(error)
    raise
finally:
    guest.stop()
    metadata['nvme_sha256_after'] = f['digest'](out / 'gpt.img')
    metadata['production_sha256_after'] = f['digest'](REPO / 'build/amd64/hdd-image.img')
    metadata['protected_before'] = before
    metadata['protected_after'] = f['protected_bytes'](out / 'gpt.img')
    (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
for name in ('data.img', 'swapfile'):
    subprocess.run(['mcopy', '-i', image, '::/' + name, str(out / name)], check=True)
    metadata[name + '_sha256'] = f['digest'](out / name)
metadata['result'] = 'PASS staging' if before == metadata['protected_after'] and metadata['production_sha256'] == metadata['production_sha256_after'] else 'FAIL protected source'
(out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
assert metadata['result'] == 'PASS staging'
print(metadata['result'])

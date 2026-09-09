#!/usr/bin/env python3
"""Actual command and Noct identity contract on disposable USB/NVMe QEMU media."""
from pathlib import Path
import json
import re
import runpy
import sys
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
f = runpy.run_path(str(HERE / 'run-formatter-qemu.py'))
out = Path(sys.argv[1]).resolve()
out.relative_to(REPO / 'plan/ws019-installation/temp')
out.mkdir(parents=True, exist_ok=False)
metadata = f['prepare_boot'](out, False, False, REPO / 'build/arch-images/amd64-ws019-installer.ufs')
f['create_nvme'](out / 'gpt.img')
before = f['digest'](out / 'gpt.img')
guest = f['FormatterGuest'](out, usb_boot=True)
try:
    guest.login()
    guest.run('noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-identity.noct', 'installer identity PASS')
    guest.run('noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-command.noct', 'installer command PASS')
    guest.run('noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-selection.noct', 'installer selection PASS')
    scratch = guest.run('noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-scratch.noct', 'installer scratch PASS')
    canonical = json.loads((REPO / 'plan/ws019-installation/temp/q133-staging/result.json').read_text())
    assert canonical['result'] == 'PASS staging'
    assert canonical['data.img_sha256'] == f['digest'](REPO / 'plan/ws019-installation/temp/q133-staging/data.img')
    assert canonical['swapfile_sha256'] == f['digest'](REPO / 'plan/ws019-installation/temp/q133-staging/swapfile')
    assert canonical['data.img_sha256'] + '  /run/installer-data' in scratch
    assert canonical['swapfile_sha256'] + '  /run/installer-swap' in scratch
    metadata['canonical_data_sha256'] = canonical['data.img_sha256']
    metadata['canonical_swap_sha256'] = canonical['swapfile_sha256']
    assert not re.search(r'panic:|fatal trap|assertion failed', guest.text(), re.I)
    metadata['result'] = 'guest functional PASS'
except BaseException as error:
    metadata['result'] = 'FAIL guest'
    metadata['failure'] = str(error)
    raise
finally:
    guest.stop()
    metadata['nvme_sha256_before'] = before
    metadata['nvme_sha256_after'] = f['digest'](out / 'gpt.img')
    metadata['production_sha256_after'] = f['digest'](REPO / 'build/amd64/hdd-image.img')
    (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
metadata['result'] = 'PASS command identity' if before == metadata['nvme_sha256_after'] and metadata['production_sha256'] == metadata['production_sha256_after'] else 'FAIL source changed'
(out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
assert metadata['result'] == 'PASS command identity'
print(metadata['result'])

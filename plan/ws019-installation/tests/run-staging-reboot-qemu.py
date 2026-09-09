#!/usr/bin/env python3
"""Boot and persist through data/swap images created by actual Noct staging."""
from pathlib import Path
import json
import re
import runpy
import sys
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
f = runpy.run_path(str(HERE / 'run-formatter-qemu.py'))
source = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
for path in (source, out):
    path.relative_to(REPO / 'plan/ws019-installation/temp')
previous = json.loads((source / 'result.json').read_text())
assert previous['result'] == 'PASS staging'
assert previous['nvme_sha256_after'] == f['digest'](source / 'gpt.img')
out.mkdir(parents=True, exist_ok=False)
metadata = f['prepare_boot'](out, True, False,
    REPO / 'build/arch-images/amd64-ws019-staging.ufs')
f['copy_image'](source / 'gpt.img', out / 'gpt.img')
metadata['staging_source'] = str(source)
metadata['staging_source_sha256'] = previous['nvme_sha256_after']
before = f['protected_bytes'](out / 'gpt.img')
guest = f['FormatterGuest'](out, usb_boot=True)
try:
    f['overlay_cell'](guest)
    assert not re.search(r'panic:|fatal trap|assertion failed', guest.text(), re.I)
    metadata['guest_functional_result'] = 'PASS'
    metadata['result'] = 'awaiting protected-byte verification'
except BaseException as error:
    metadata['result'] = 'FAIL staging reboot'
    metadata['failure'] = str(error)
    raise
finally:
    guest.stop()
    metadata['protected_before'] = before
    metadata['protected_after'] = f['protected_bytes'](out / 'gpt.img')
    metadata['production_sha256_after'] = f['digest'](REPO / 'build/amd64/hdd-image.img')
    metadata['staging_source_sha256_after'] = f['digest'](source / 'gpt.img')
    (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
try:
    assert before == metadata['protected_after']
    assert metadata['production_sha256'] == metadata['production_sha256_after']
    assert metadata['staging_source_sha256'] == metadata['staging_source_sha256_after']
    metadata['result'] = 'PASS staging reboot'
except BaseException as error:
    metadata['result'] = 'FAIL protected-byte verification'
    metadata['failure'] = str(error)
    raise
finally:
    (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(metadata['result'])

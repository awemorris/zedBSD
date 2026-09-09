#!/usr/bin/env python3
"""Real command and syscall publication on disposable USB-root and NVMe FAT."""
from pathlib import Path
import json
import re
import runpy
import sys
import subprocess

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
f = runpy.run_path(str(HERE / 'run-formatter-qemu.py'))
out = Path(sys.argv[1]).resolve()
out.relative_to(REPO / 'plan/ws019-installation/temp')
out.mkdir(parents=True, exist_ok=False)
metadata = f['prepare_boot'](out, False, False,
    REPO / 'build/arch-images/amd64-ws019-publication.ufs')
f['create_nvme'](out / 'gpt.img')
before = f['protected_bytes'](out / 'gpt.img')
(out / 'native-root').mkdir()
subprocess.run([str(REPO / 'build/zedimage-host'), 'ufs', '33554432',
    str(out / 'native-root'), str(out / 'native.img')], check=True)
guest = f['FormatterGuest'](out, usb_boot=True, extra_args=[
    '-drive', f"file={out / 'native.img'},format=raw,if=none,id=native",
    '-device', 'usb-storage,drive=native,bus=xhci.0,port=2'])
try:
    guest.login()
    guest.run('mount -t fat nvme0n1p1 /publication')
    guest.run('publication-probe /publication fat', 'publication target PASS')
    guest.run('mkdir /root/publication')
    guest.run('publication-probe /root/publication overlay', 'publication target PASS')
    guest.run('mount -t ufs sdb /native')
    guest.run('publication-probe /native ufs', 'publication target PASS')
    guest.run('cp /etc/passwd /root/stage')
    guest.run('mv -T --update=none-fail -- /root/stage /root/published')
    guest.run('cp /etc/group /root/stage')
    guest.run('mv -T --update=none-fail -- /root/stage /root/published', 'File exists', 1)
    guest.run('cmp /etc/passwd /root/published')
    guest.run('cmp /etc/group /root/stage')
    guest.run('mv -n -T /root/stage /root/published')
    guest.run('cmp /etc/group /root/stage')
    guest.run('mv -T /root/stage /root/published')
    guest.run('cmp /etc/group /root/published')
    guest.run('sync /publication /native /root/published /root')
    guest.run('sync /missing-publication-file', status=1)
    guest.run('sync')
    guest.run('umount /publication')
    guest.run('umount /native')
    at = len(guest.text())
    guest.send('reboot')
    guest.login(at)
    guest.run('mount -t fat nvme0n1p1 /publication')
    guest.run('publication-probe /publication verify', 'publication persisted PASS')
    guest.run('publication-probe /root/publication verify', 'publication persisted PASS')
    guest.run('mount -t ufs sdb /native')
    guest.run('publication-probe /native verify', 'publication persisted PASS')
    guest.run('umount /native')
    guest.run('cmp /etc/group /root/published')
    guest.run('umount /publication')
    assert not re.search(r'panic:|fatal trap|assertion failed', guest.text(), re.I)
    metadata['result'] = 'guest functional PASS'
except BaseException as error:
    metadata['result'] = 'FAIL publication'
    metadata['failure'] = str(error)
    raise
finally:
    guest.stop()
    metadata['protected_before'] = before
    metadata['protected_after'] = f['protected_bytes'](out / 'gpt.img')
    metadata['production_sha256_after'] = f['digest'](REPO / 'build/amd64/hdd-image.img')
    (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
if before != metadata['protected_after'] or metadata['production_sha256'] != metadata['production_sha256_after']:
    metadata['result'] = 'FAIL protected media'
else:
    metadata['result'] = 'PASS publication'
(out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
assert metadata['result'] == 'PASS publication'
print(metadata['result'])

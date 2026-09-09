#!/usr/bin/env python3
"""Retained boot identities on fresh USB images, independent of boot0."""
from pathlib import Path
import json
import re
import runpy
import struct
import subprocess
import sys
import uuid

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
f = runpy.run_path(str(HERE / 'run-formatter-qemu.py'))
mode = sys.argv[1]
assert mode in ('ordinary', 'override', 'ambiguous', 'legacy')
out = Path(sys.argv[2]).resolve()
out.relative_to(REPO / 'plan/ws019-installation/temp')
out.mkdir(parents=True, exist_ok=False)
metadata = f['prepare_boot'](out, False, False,
    REPO / 'build/arch-images/amd64-ws019-storage.ufs')
f['create_nvme'](out / 'gpt.img')
boot = out / 'boot.img'
with boot.open('rb') as disk:
    disk.seek(512)
    header = disk.read(512)
    table, slots, stride = struct.unpack_from('<QII', header, 72)
    disk.seek(table * 512)
    entries = disk.read(slots * stride)
partitions = []
for offset in range(0, len(entries), stride):
    entry = entries[offset:offset + stride]
    if entry[:16] == bytes(16):
        continue
    partitions.append((str(uuid.UUID(bytes_le=entry[16:32])), struct.unpack_from('<Q', entry, 32)[0], str(uuid.UUID(bytes_le=entry[:16]))))
esps = [p for p in partitions if p[2] == 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b']
payloads = [p for p in partitions if p[1] == f['boot_payload'](boot)]
assert len(esps) == 1 and len(payloads) == 1
esp, payload = esps[0], payloads[0]
firmware = 'PARTUUID=' + esp[0]
configuration = 'PARTUUID=' + payload[0]
matches = 1
esp_image = f'{boot}@@{esp[1] * 512}'
payload_image = f'{boot}@@{payload[1] * 512}'
original_config = subprocess.run(['mtype', '-i', payload_image, '::/zedbsd.cfg'], check=True, capture_output=True).stdout.decode()
with boot.open('rb') as disk:
    disk.seek(payload[1] * 512 + 67)
    serial = struct.unpack('<I', disk.read(4))[0]
payload_uuid = f'{serial >> 16:04X}-{serial & 65535:04X}'

def put_config(image, content):
    file = out / 'selected.cfg'
    file.write_text(content)
    subprocess.run(['mcopy', '-o', '-i', image, str(file), '::/zedbsd.cfg'], check=True)

if mode == 'ordinary':
    # An off-disk configuration marker must not enter this boot disk's count.
    put_config(f"{out / 'gpt.img'}@@1048576", original_config)
elif mode == 'override':
    # Config/kernel still come from USB; boot0's data/root come from another disk.
    for name in ('rootfs.img', 'data.img', 'swapfile'):
        temporary = out / ('source-' + name)
        subprocess.run(['mcopy', '-i', payload_image, '::/' + name, str(temporary)], check=True)
        subprocess.run(['mcopy', '-o', '-i', f"{out / 'gpt.img'}@@1048576", str(temporary), '::/' + name], check=True)
    put_config(payload_image, original_config + 'boot0=UUID=7819-0000\n')
elif mode == 'ambiguous':
    # Firmware ESP becomes the first config source while runtime boot0 stays payload.
    kernel = out / 'esp-vmunix'
    subprocess.run(['mcopy', '-i', payload_image, '::/vmunix', str(kernel)], check=True)
    subprocess.run(['mcopy', '-i', esp_image, str(kernel), '::/vmunix'], check=True)
    put_config(esp_image, original_config + f'boot0=UUID={payload_uuid}\n')
    configuration, matches = firmware, 2
elif mode == 'legacy':
    loader = REPO / 'plan/ws019-installation/temp/q131-inputs/BOOTX64-v6.EFI'
    metadata['legacy_loader_sha256'] = f['digest'](loader)
    subprocess.run(['mcopy', '-o', '-i', esp_image, str(loader), '::/EFI/BOOT/BOOTX64.EFI'], check=True)
    firmware = configuration = 'unavailable'
    matches = 0

metadata.update(mode=mode, expected_firmware=firmware,
    expected_configuration=configuration, expected_matches=matches)
metadata['boot_sha256_before'] = f['digest'](boot)
metadata['auxiliary_sha256_before'] = f['digest'](out / 'gpt.img')
guest = f['FormatterGuest'](out, usb_boot=True)
try:
    guest.login()
    guest.run('sysctl kern.boot.firmware_partition', re.escape('kern.boot.firmware_partition: ' + firmware))
    guest.run('sysctl kern.boot.config_partition', re.escape('kern.boot.config_partition: ' + configuration))
    guest.run('sysctl kern.boot.config_matches', rf'^kern.boot.config_matches: {matches}$')
    guest.run('sysctl kern.boot.config_matches=0', status=1)
    guest.run('sysctl kern.boot.config_matches', rf'^kern.boot.config_matches: {matches}$')
    if mode == 'override':
        assert 'boot0 UUID=7819-0000 -> /dev/nvme0n1p1' in guest.text()
    if mode == 'ambiguous':
        assert 'multiple zedbsd.cfg files' in guest.text()
    assert not re.search(r'panic:|fatal trap|assertion failed', guest.text(), re.I)
    metadata['result'] = 'guest functional PASS'
except BaseException as error:
    metadata['result'] = 'FAIL ' + mode
    metadata['failure'] = str(error)
    raise
finally:
    guest.stop()
    metadata['production_sha256_after'] = f['digest'](REPO / 'build/amd64/hdd-image.img')
    (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
if metadata['production_sha256'] != metadata['production_sha256_after']:
    metadata['result'] = 'FAIL protected source'
else:
    metadata['result'] = 'PASS ' + mode
(out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
assert metadata['result'] == 'PASS ' + mode
print(metadata['result'])

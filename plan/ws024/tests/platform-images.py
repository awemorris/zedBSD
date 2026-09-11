#!/usr/bin/env python3
"""Check platform packaging with synthetic ABI headers, never executable boot evidence."""
from pathlib import Path
import json
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(sys.argv[1]).resolve()
OUT.relative_to(ROOT / 'plan/ws024/temp')
OUT.mkdir(parents=True, exist_ok=False)

def run(script, *args):
    subprocess.run(['python3', str(ROOT / script), *map(str, args)], check=True)

def header(name, machine, endian):
    value = bytearray(64)
    value[:7] = b'\x7fELF\x02' + bytes([1 if endian == '<' else 2, 1])
    struct.pack_into(endian + 'H', value, 18, machine)
    path = OUT / name
    path.write_bytes(value)
    path.chmod(0o755)
    return path

arm = header('synthetic-arm-shell', 183, '<')
sparc = header('synthetic-sparc-shell', 43, '>')
kernel = bytearray(64)
kernel[56:60] = b'ARM\x64'
(OUT / 'arm-kernel').write_bytes(kernel)
(OUT / 'config.txt').write_text('arm_64bit=1\n')
(OUT / 'swapfile').write_bytes(bytes(4096))
firmware = OUT / 'synthetic-firmware'
for name in ('start4.elf', 'fixup4.dat', 'bcm2711-rpi-4-b.dtb',
             'LICENCE.broadcom', 'overlays/disable-bt.dtbo'):
    path = firmware / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'synthetic packaging input\n')
run('tools/build/make-arch-overlay-image.py', '--profile', 'aarch64',
    '--output', OUT / 'arm.fat', '--file', f'/bin/sh={arm}')
run('tools/build/make-arch-overlay-ufs.py', '--profile', 'aarch64',
    '--output', OUT / 'arm.ufs', '--file', f'/bin/sh={arm}')
run('tools/build/check-arch-overlay-ufs.py', '--profile', 'aarch64',
    '--image', OUT / 'arm.ufs')
run('tools/build/make-ufs-root-image.py', '--arch-profile', 'aarch64',
    '--arch-image', OUT / 'arm.ufs', OUT / 'arm-root.ufs')
assert (OUT / 'arm.ufs').read_bytes() == (OUT / 'arm-root.ufs').read_bytes()
run('platform/arm64/tools/make-rpi4-ufs-root-hdd-image.py',
    '--kernel', OUT / 'arm-kernel', '--arch-image', OUT / 'arm.fat',
    '--data-image', OUT / 'arm.ufs', '--swapfile', OUT / 'swapfile',
    '--ufs-root', OUT / 'arm-root.ufs', '--config', OUT / 'config.txt',
    '--firmware-dir', firmware, OUT / 'rpi4.img')
with (OUT / 'rpi4.img').open('rb') as image:
    image.seek(0x1ce)
    entry = struct.unpack('<B3sB3sII', image.read(16))
    assert entry[2] == 0xa5 and entry[4] == 264192
    image.seek(entry[4] * 512)
    assert image.read(entry[5] * 512) == (OUT / 'arm-root.ufs').read_bytes()
run('tools/build/make-ufs-root-image.py', '--arch-profile', 'sparcv9',
    '--native-shell', sparc, '--native-sysctl', sparc, '--size-mib', '16',
    OUT / 'sparc.ufs')
(OUT / 'stage1').write_bytes(b'synthetic stage1')
(OUT / 'stage2').write_bytes(b'synthetic stage2')
run('platform/sparcv9/tools/make-sparcv9-hdd-image.py',
    '--stage1', OUT / 'stage1', '--stage2', OUT / 'stage2',
    '--kernel', sparc, '--shell', sparc, '--sysctl', sparc,
    '--ufs-root', OUT / 'sparc.ufs', OUT / 'sparc.img')
(OUT / 'result.json').write_text(json.dumps({
    'status': 'PASS', 'scope': 'ARM64/RPi4/SPARC producer/check paths',
    'inputs': 'synthetic ABI headers and firmware placeholders',
    'boot_executed': False}, indent=2) + '\n')
print('Platform packaging and unified UFS checks PASS (no boot execution)')

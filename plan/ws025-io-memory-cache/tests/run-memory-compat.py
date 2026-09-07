#!/usr/bin/env python3
"""Exercise retained old artifacts against the current memory handoff consumer."""
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('memory_boot', Path(__file__).with_name('run-memory-boot.py'))
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)

def partition(image, name):
    with image.open('rb') as stream:
        stream.seek(512)
        header = stream.read(512)
        assert header[:8] == b'EFI PART'
        lba, count, width = struct.unpack_from('<QII', header, 72)
        assert count <= 4096 and 128 <= width <= 4096
        stream.seek(lba * 512)
        entries = stream.read(count * width)
    found = []
    for i in range(count):
        entry = entries[i * width:(i + 1) * width]
        if entry[:16] == bytes(16):
            continue
        first, last = struct.unpack_from('<QQ', entry, 32)
        assert 34 <= first <= last < image.stat().st_size // 512
        if subprocess.run(['mdir', '-i', f'{image}@@{first * 512}', name], capture_output=True).returncode == 0:
            found.append(first * 512)
    assert len(found) == 1
    return found[0]

def main():
    output, firmware, mode, old = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3], Path(sys.argv[4]).resolve()
    output.relative_to(REPO / 'plan/ws025-io-memory-cache/temp')
    old.relative_to(REPO / 'plan/ws025-io-memory-cache/temp')
    assert mode in ('old-loader', 'old-kernel') and firmware in ('bios', 'uefi')
    output.mkdir(parents=True, exist_ok=False)
    current = REPO / 'build/amd64/hdd-image.img'
    hashes = {str(path): boot.base.digest(path) for path in (old, current)}
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', current, output / 'boot.img'], check=True)
    if mode == 'old-kernel':
        name = '::/vmunix'
    elif firmware == 'bios':
        name = '::/BOOTZBSD.EXE'
    else:
        name = '::/EFI/BOOT/BOOTX64.EFI'
    source_offset = partition(old, name)
    target_offset = partition(current, name)
    artifact = output / 'legacy-artifact'
    subprocess.run(['mcopy', '-i', f'{old}@@{source_offset}', name, artifact], check=True)
    subprocess.run(['mcopy', '-o', '-i', f'{output / "boot.img"}@@{target_offset}', artifact, name], check=True)
    memory = '256' if firmware == 'bios' and mode == 'old-loader' else '4096'
    guest = boot.MemoryGuest(output, firmware, memory)
    try:
        if mode == 'old-kernel':
            text, _ = guest.wait(r'(unsupported|invalid) amd64 ZBL6 handoff', timeout=120)
            assert 'login:' not in text
        else:
            guest.login()
            assert 'A64 MEMORY legacy degraded:' in guest.text()
            at = len(guest.text())
            guest.send('sysctl hw.memory.stats')
            text = guest.prompt(at)
            assert 'source=0' in text
            (output / 'memory.txt').write_text(text)
        (output / 'provenance.json').write_text(json.dumps({'sources': hashes, 'artifact_sha256': boot.base.digest(artifact), 'mode': mode, 'firmware': firmware}, indent=2) + '\n')
        print('PASS:', firmware, mode)
    finally:
        if guest.proc.poll() is None:
            guest.proc.stdin.write('quit\n')
            guest.proc.stdin.flush()
            try:
                guest.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                guest.proc.kill()
                guest.proc.wait()
        guest.commands.close()
        guest.monitor.close()
        for path, digest in hashes.items():
            assert boot.base.digest(Path(path)) == digest

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Mount a disposable UFS persistence profile and verify features after reboot."""
from pathlib import Path
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import time

repo = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('format_runner', repo / 'plan/ws019-installation/tests/run-formatter-qemu.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
sys.path.insert(0, str(repo / 'tools/build'))
from ufs_format import create

class FeatureGuest(base.FormatterGuest):
    def __init__(self, output):
        self.output = output
        self.log = output / 'guest.log'
        self.deadline = time.monotonic() + 600
        self.commands = (output / 'commands.log').open('w')
        self.monitor = (output / 'qemu.log').open('w')
        args = ['qemu-system-x86_64', '-machine', 'q35', '-m', '2048', '-smp', '4',
            '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
            '-drive', f'if=pflash,format=raw,file={output / "vars.fd"}',
            '-device', 'qemu-xhci,id=xhci',
            '-drive', f'if=none,id=boot,format=raw,file={output / "boot.img"}',
            '-device', 'usb-storage,bus=xhci.0,drive=boot,bootindex=1',
            '-drive', f'if=none,id=features,format=raw,file={output / "features.img"}',
            '-device', 'nvme,drive=features,serial=ws024features',
            '-nic', 'none', '-display', 'none', '-serial', 'none',
            '-debugcon', f'file:{self.log}', '-monitor', 'stdio']
        self.commands.write(repr(args) + '\n')
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=self.monitor,
            stderr=subprocess.STDOUT, text=True)

out = Path(sys.argv[1]).resolve()
out.relative_to(repo / 'plan/ws024-unified-ufs/temp')
out.mkdir(exist_ok=False)
source = repo / 'build/amd64/hdd-image.img'
root = repo / 'build/arch-images/amd64-ws024-features.ufs'
original = base.digest(source)
base.copy_image(source, out / 'boot.img')
start = base.boot_payload(source)
subprocess.run(['mcopy', '-o', '-i', f'{out / "boot.img"}@@{start * 512}',
    str(root), '::/rootfs.img'], check=True)
shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', out / 'vars.fd')
orphan_mode = '--orphans' in sys.argv[2:]
deferred_mode = '--deferred' in sys.argv[2:]
legacy = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 and not (orphan_mode or deferred_mode) else None
if legacy:
    shutil.copyfile(legacy, out / 'features.img')
    legacy_hash = base.digest(legacy)
elif orphan_mode:
    spec = importlib.util.spec_from_file_location('orphan_image',
        repo / 'plan/ws025-io-memory-cache/tests/orphan-image.py')
    orphan_image = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(orphan_image)
    data, orphan_metadata = orphan_image.seed_orphans()
    (out / 'features.img').write_bytes(data)
    (out / 'orphan-seed.json').write_text(json.dumps(orphan_metadata, indent=2) + '\n')
else:
    (out / 'features.img').write_bytes(create(32 * 1024**2, profile='journal-snapshot'))
guest = FeatureGuest(out)
try:
    guest.login()
    if legacy:
        guest.run('mkdir /legacy')
        if len(sys.argv) > 3 and sys.argv[3] == 'reject':
            guest.run('mount -t ufs nvme0n1 /legacy', status=1)
        else:
            guest.run('mount -t ufs -o ro nvme0n1 /legacy')
            guest.run('cat /legacy/etc/zedbsd-root', 'zedBSD ufs2 root v1')
            guest.run('umount /legacy')
        assert base.digest(out / 'features.img') == legacy_hash
    else:
        guest.run('ufs-features ' + ('run-deferred' if deferred_mode else 'run'), 'UFS FEATURES RUN PASS')
        at = len(guest.text())
        guest.send('reboot')
        guest.login(at)
        guest.run('ufs-features verify', 'UFS FEATURES VERIFY PASS')
finally:
    guest.stop()
    assert base.digest(source) == original, 'source image changed'
    (out / 'source.sha256').write_text(original + '  build/amd64/hdd-image.img\n')
result = {'legacy_mount_or_refusal': 'PASS', 'media_unchanged': True} if legacy else {
    'mounted_features': 'PASS', 'remount': 'PASS', 'reboot': 'PASS'}
if orphan_mode:
    orphan_image.verify_reclaimed(out / 'features.img', orphan_metadata)
    result['orphan_recovery'] = 'PASS'
if deferred_mode:
    result['deferred_metadata_policy'] = 'PASS'
(out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
print('UFS acceptance PASS', result)

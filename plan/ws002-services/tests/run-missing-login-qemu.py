#!/usr/bin/env python3
"""Current PC98/PCAT missing-login reproduction; disposable, validated images."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import runpy
import socket
import os
import sys
import struct
import subprocess
import time

REPO = Path(__file__).resolve().parents[3]
UFS = runpy.run_path(str(REPO / 'tools/build/check-ufs-image.py'))['UFS']
Base = runpy.run_path(str(REPO / 'plan/ws025-io-memory-cache/tests/run-p032-pc98-session.py'))['Guest']

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()

class Guest(Base):
    def __init__(self, directory, disk, platform):
        self.directory = directory
        directory.mkdir()
        self.deadline = time.monotonic() + 180
        self.log = (directory / 'screen.log').open('w')
        self.previous = None
        self.sequence = 0
        self.platform = platform
        monitor = directory / 'monitor.sock'
        self.output = (directory / 'qemu.log').open('w')
        if platform == 'pc98':
            command = [str(REPO / 'build/qemu-pc98/build/qemu-system-i386'), '-M', 'pc9821,pegc=off,coregraph=on', '-cpu', '486']
        else:
            command = ['qemu-system-i386', '-machine', 'pc']
        command += ['-smp', '1', '-m', '64M', '-display', 'none', '-serial', 'none', '-no-reboot', '-drive', f'if=ide,format=raw,file={disk}', '-debugcon', f'file:{directory}/debugcon.log', '-monitor', f'unix:{monitor},server=on,wait=off']
        (directory / 'argv.json').write_text(json.dumps(command, indent=2))
        self.proc = subprocess.Popen(command, stdout=self.output, stderr=subprocess.STDOUT)
        self.socket = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.socket.connect(str(monitor))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(.05)
        else:
            self.close()
            raise RuntimeError('monitor did not start')
        self.socket.settimeout(.2)
        self.drain()

    def screen(self):
        vram = self.directory / 'vram.bin'
        vram.unlink(missing_ok=True)
        address = '0xa0000' if self.platform == 'pc98' else '0xb8000'
        self.socket.sendall(f'pmemsave {address} 0x1000 "{vram}"\n'.encode())
        self.drain()
        raw = vram.read_bytes()
        text = '\n'.join(''.join(chr(c) if 32 <= c < 127 else ' ' for c in raw[row * 160:row * 160 + 160:2]).rstrip() for row in range(25))
        if text != self.previous:
            self.log.write(text + '\n---\n')
            self.log.flush()
            self.previous = text
        if re.search(r'panic:|fatal:|invalid kernel allocation free|\[INT\]|LIFECYCLE FAIL', text, re.I):
            raise RuntimeError('kernel failure: ' + text)
        return text

def missing_root(source, output):
    data = bytearray(source.read_bytes())
    fs = UFS(data)
    fs.validate()
    login = fs.lookup('/bin/login')
    directory = fs.lookup('/bin')
    raw = fs.read_file(directory)
    offset = 0
    changed = []
    while offset < len(raw):
        ino, length, kind, nlen = struct.unpack_from('<IHBB', raw, offset)
        if ino == login and raw[offset+8:offset+8+nlen] == b'login':
            physical = fs.blocks(directory)[offset // fs.bsize] * fs.fsize + offset % fs.bsize + 8
            assert data[physical:physical+5] == b'login'
            data[physical:physical+5] = b'nogin'
            changed.append(physical)
        offset += length
    assert len(changed) == 1
    after = UFS(data)
    after.validate()
    assert after.lookup('/bin/nogin') == login
    assert not any(name == 'login' for name, _, _ in after.entries(directory))
    output.write_bytes(data)
    return {'renamed_directory_offset': changed[0], 'source_root_sha256': digest(source), 'test_root_sha256': digest(output)}

def lifecycle_root(image, program, directory):
    fs = UFS(image.read_bytes())
    root = directory / 'tree'
    def extract(number, path):
        inode = fs.inode(number)
        mode = fs.u16(inode, 0)
        kind = mode & 0o170000
        if kind == 0o040000:
            path.mkdir()
            for name, child, _ in fs.entries(number):
                if name not in ('.', '..'):
                    extract(child, path / name)
        elif kind == 0o120000:
            path.symlink_to(fs.read_file(number).decode())
        elif kind == 0o100000:
            path.write_bytes(fs.read_file(number))
        else:
            raise ValueError(f'unsupported fixture inode type {kind:o}')
        if kind != 0o120000:
            os.chmod(path, mode & 0o7777)
    extract(2, root)
    (root / 'sbin/init').write_bytes(program.read_bytes())
    os.chmod(root / 'sbin/init', 0o755)
    sys.path.insert(0, str(REPO / 'tools/build'))
    from ufs_format import create
    image.write_bytes(create(image.stat().st_size, root))
    UFS(image.read_bytes()).validate()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    parser.add_argument('--platform', choices=['pcat', 'pc98'], required=True)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--lifecycle', type=Path)
    modes.add_argument('--normal', action='store_true')
    parser.add_argument('--runs', type=int, choices=range(1, 5), default=3)
    args = parser.parse_args()
    out = args.output.resolve()
    out.relative_to(REPO / 'plan/ws002-services/temp')
    out.mkdir(parents=True, exist_ok=False)
    source = REPO / f'build/{args.platform}/hdd-image.img'
    root = out / 'original-root.ufs'
    subprocess.run(['mcopy', '-i', f'{source}@@1048576', '::/rootfs.img', str(root)], check=True)
    if args.normal:
        (out / 'missing-login.ufs').write_bytes(root.read_bytes())
        metadata = {'source_root_sha256': digest(root), 'test_root_sha256': digest(root)}
        UFS(root.read_bytes()).lookup('/bin/login')
    else:
        metadata = missing_root(root, out / 'missing-login.ufs')
    if args.lifecycle:
        lifecycle_root(out / 'missing-login.ufs', args.lifecycle.resolve(), out)
        metadata['lifecycle_program_sha256'] = digest(args.lifecycle)
        metadata['test_root_sha256'] = digest(out / 'missing-login.ufs')
    metadata.update(platform=args.platform, production_sha256=digest(source), runs=[])
    try:
        for index in range(args.runs):
            disk = out / f'run{index}.img'
            subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(source), str(disk)], check=True)
            subprocess.run(['mcopy', '-o', '-i', f'{disk}@@1048576', str(out / 'missing-login.ufs'), '::/rootfs.img'], check=True)
            guest = Guest(out / f'run{index}', disk, args.platform)
            record = {'index': index}
            metadata['runs'].append(record)
            try:
                if args.normal:
                    for session in range(3):
                        guest.login()
                        guest.command('pwd', r'^/root *$')
                        guest.send('exit')
                        guest.wait(r'login: *$')
                    record['result'] = 'PASS three normal login/logout/respawn cycles'
                    print(args.platform, index, record['result'], flush=True)
                    continue
                if args.lifecycle:
                    guest.wait(r'LIFECYCLE PASS 100 getty exec failures and owner recovery', 120)
                    record['result'] = 'PASS 100 lifecycle owner comparisons'
                    print(args.platform, index, record['result'], flush=True)
                    continue
                guest.wait(r'getty: /bin/login: No such file or directory', 90)
                end = time.monotonic() + 35
                maximum = 0
                counts = []
                while time.monotonic() < end:
                    text = guest.screen()
                    count = text.count('getty: /bin/login: No such file or directory')
                    maximum = max(maximum, count)
                    counts.append(count)
                    assert maximum <= 6, text
                    assert guest.proc.poll() is None
                    time.sleep(.3)
                record.update(maximum_visible_failures=maximum, last_counts=counts[-10:])
                assert maximum == 6 and all(count == 6 for count in counts[-10:]), counts
                record['result'] = 'PASS bounded missing-login reproduction cell'
            finally:
                guest.close()
            print(args.platform, index, record['result'], flush=True)
        metadata['result'] = ('PASS lifecycle counters; historical allocation provenance unproven' if args.lifecycle else 'PASS normal sessions' if args.normal else 'PASS finite reproduction; provenance/accounting not established')
    except BaseException as error:
        metadata['failure'] = str(error)
        raise
    finally:
        metadata['production_sha256_after'] = digest(source)
        (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
    assert metadata['production_sha256'] == metadata['production_sha256_after']

if __name__ == '__main__':
    main()

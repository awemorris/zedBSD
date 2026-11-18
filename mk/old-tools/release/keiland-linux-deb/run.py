#!/usr/bin/env python3
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Create native debs and verify installation in fresh QEMU guests."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import socket
import subprocess
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
INPUTS = json.loads(Path(__file__).with_name('inputs.json').read_text())
BUILD_PACKAGES = ('build-essential', 'pkg-config', 'python3', 'binutils', 'dpkg-dev',
                  'libvulkan-dev', 'libdrm-dev', 'curl', 'ca-certificates', 'xz-utils')


def interrupted(_signal, _frame):
    # Unwind the active Guest context before leaving the target.
    raise KeyboardInterrupt('package build interrupted')


def run(args, **kwargs):
    return subprocess.run(args, check=True, timeout=kwargs.pop('timeout', 120), **kwargs)


def digest(path, algorithm='sha256'):
    value = hashlib.new(algorithm)
    with path.open('rb') as stream:
        while data := stream.read(1024 * 1024):
            value.update(data)
    return value.hexdigest()


def image(base, distro):
    config = INPUTS[distro]
    directory = base / 'images'
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / (distro + '.qcow2')
    with (directory / (distro + '.lock')).open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if path.exists():
            if digest(path, config['algorithm']) != config['hash']:
                raise RuntimeError('cached image checksum mismatch: ' + str(path))
            return path
        temporary = directory / (distro + '.download')
        run(['curl', '-fsSL', '--retry', '2', '--max-time', '1200',
             config['url'], '-o', str(temporary)], timeout=1220)
        if digest(temporary, config['algorithm']) != config['hash']:
            raise RuntimeError('downloaded image checksum mismatch: ' + str(temporary))
        temporary.replace(path)
    return path


class Guest:
    """Own one emulator process, seed, overlay and loopback forwarding port."""

    def __init__(self, directory, source, label):
        self.directory = directory
        self.source = source
        self.label = label
        self.process = None
        self.key_path = directory / 'id_ed25519'
        self.qmp_socket = directory / 'qmp.sock'
        self.port = None

    def ssh(self, command, **kwargs):
        args = ['ssh', '-i', str(self.key_path), '-o', 'StrictHostKeyChecking=no',
                '-o', 'UserKnownHostsFile=/dev/null', '-o', 'LogLevel=ERROR',
                '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=2',
                '-p', str(self.port), 'keiland@127.0.0.1', command]
        return run(args, **kwargs)

    def copy(self, source, destination, retrieve=False):
        remote = 'keiland@127.0.0.1:'
        paths = [remote + str(source), str(destination)] if retrieve else [str(source), remote + destination]
        run(['scp', '-q', '-i', str(self.key_path), '-o', 'StrictHostKeyChecking=no',
             '-o', 'UserKnownHostsFile=/dev/null', '-o', 'LogLevel=ERROR',
             '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', '-P', str(self.port),
             *paths], timeout=180)

    def qmp(self, command, arguments=None):
        run(['python3', str(ROOT / 'plan/tools/qmp.py'), str(self.qmp_socket),
             command, json.dumps(arguments or {})], stdout=subprocess.DEVNULL)

    def screenshot(self, path):
        self.qmp('screendump', {'filename': str(path.resolve()), 'format': 'png'})

    def key(self, code):
        for down in (True, False):
            self.qmp('input-send-event', {'device': 'video0', 'head': 0, 'events': [
                {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': code}}}]})

    def type(self, text):
        codes = {' ': 'spc', '\n': 'ret', '-': 'minus', '/': 'slash', '.': 'dot'}
        for character in text:
            self.key(codes.get(character, character))
            time.sleep(0.15 if self.acceleration == 'tcg' else 0.04)

    def __enter__(self):
        try:
            self.start()
        except BaseException:
            self.close()
            raise
        return self

    def start(self):
        self.directory.mkdir(parents=True)
        run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(self.key_path)])
        public = self.key_path.with_suffix('.pub').read_text().strip()
        (self.directory / 'user-data').write_text(
            '#cloud-config\nusers:\n  - name: keiland\n'
            '    sudo: ALL=(ALL) NOPASSWD:ALL\n    shell: /bin/bash\n'
            '    ssh_authorized_keys:\n      - ' + public + '\n'
            'ssh_pwauth: false\ndisable_root: true\n')
        (self.directory / 'meta-data').write_text('instance-id: ' + self.label + '\nlocal-hostname: ' + self.label + '\n')
        run(['xorriso', '-as', 'mkisofs', '-quiet', '-V', 'cidata', '-o',
             str(self.directory / 'seed.iso'), str(self.directory / 'user-data'),
             str(self.directory / 'meta-data')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-F', 'qcow2', '-b', str(self.source),
             str(self.directory / 'overlay.qcow2'), '20G'])
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            self.port = listener.getsockname()[1]
        selected = os.environ.get('KEILAND_DEB_ACCEL', 'auto')
        if selected not in ('auto', 'kvm', 'tcg'):
            raise RuntimeError('KEILAND_DEB_ACCEL must be auto, kvm or tcg')
        available = os.access('/dev/kvm', os.R_OK | os.W_OK)
        if selected == 'kvm' and not available:
            raise RuntimeError('requested KVM is unavailable')
        kvm = available and selected != 'tcg'
        self.acceleration = 'kvm' if kvm else 'tcg'
        errors = (self.directory / 'qemu-errors.log').open('wb')
        try:
            self.process = subprocess.Popen([
                'qemu-system-x86_64', '-machine', 'q35,vmport=off', '-accel', self.acceleration,
                '-cpu', 'host' if kvm else 'max', '-m', '8G', '-smp', '4',
                '-drive', 'file=' + str(self.directory / 'overlay.qcow2') + ',if=virtio',
                '-drive', 'file=' + str(self.directory / 'seed.iso') + ',media=cdrom,readonly=on',
                '-device', 'virtio-vga,id=video0', '-device', 'virtio-keyboard-pci,display=video0',
                '-device', 'virtio-tablet-pci,display=video0', '-display', 'none', '-serial', 'null',
                '-netdev', f'user,id=n0,hostfwd=tcp:127.0.0.1:{self.port}-:22',
                '-device', 'virtio-net-pci,netdev=n0', '-qmp',
                'unix:' + str(self.qmp_socket) + ',server=on,wait=off'],
                stdout=subprocess.DEVNULL, stderr=errors)
        finally:
            errors.close()
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError('QEMU exited; inspect ' + str(self.directory / 'qemu-errors.log'))
            try:
                self.ssh('cloud-init status --wait >/dev/null; sudo -n true',
                         timeout=15, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                print(self.label + ': ready (' + self.acceleration + ')', flush=True)
                return
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                time.sleep(2)
        raise RuntimeError('guest SSH readiness exceeded 240 seconds')

    def close(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)

    def __exit__(self, *_error):
        self.close()


def snapshot(destination):
    paths = ['LICENSE', 'userland/desktop', 'userland/base/common', 'userland/base/libz-compat',
             'userland/base/passkey', 'userland/base/login/verify.c', 'userland/base/login/verify.h',
             'userland/tests/wlshm', 'userland/tests/wltest', 'userland/tests/vkdemo',
             'userland/tests/mview', 'userland/tests/kuidemo', 'include/libc/compat',
             'include/libc/pdf.h', 'include/libc/sha2.h', 'include/libc/sha1.h',
             'include/libc/md5.h', 'src/libc/openbsd-sha2.c', 'src/libc/openbsd-digest.c',
             'userland/packages/fonts/noto-color-emoji/OFL-1.1.txt',
             'plan/tools/keiland-linux/vk-chain-test.c', 'plan/tools/keiland-linux/elf-check.sh']
    names = set(run(['git', 'ls-files', '-z', '--', *paths], capture_output=True).stdout.decode().split('\0'))
    names.discard('')
    names.update(str(p.relative_to(ROOT)) for p in Path(__file__).parent.rglob('*')
                 if p.is_file() and '__pycache__' not in p.parts)
    epoch = int(run(['git', 'log', '-1', '--format=%ct'], capture_output=True, text=True).stdout)
    with tarfile.open(destination, 'w:gz') as archive:
        for name in sorted(names):
            if '.internal' in Path(name).parts or 'build' in Path(name).parts:
                raise RuntimeError('forbidden source archive entry: ' + name)
            info = archive.gettarinfo(name)
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            info.mtime = epoch
            with open(name, 'rb') as source:
                archive.addfile(info, source)
    commit = run(['git', 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    version = '0~git' + time.strftime('%Y%m%d', time.gmtime(epoch)) + '.' + commit[:12]
    dirty = bool(run(['git', 'status', '--porcelain', '--', *paths,
                      'tools/release/keiland-linux-deb'], capture_output=True, text=True).stdout.strip())
    return version, commit, epoch, dirty


def check_os(guest, distro, output):
    text = guest.ssh('cat /etc/os-release; uname -m', capture_output=True, text=True).stdout
    values = dict(line.split('=', 1) for line in text.splitlines() if '=' in line)
    config = INPUTS[distro]
    if values['ID'].strip('"') != config['id'] or values['VERSION_ID'].strip('"') != config['version']:
        raise RuntimeError('guest distribution identity mismatch')
    if text.splitlines()[-1] != 'x86_64':
        raise RuntimeError('guest architecture mismatch')
    (output / (distro + '-os.txt')).write_text(text)
    guest.screenshot(output / (distro + '-console.png'))


def smoke(guest, package, client, output, distro):
    guest.copy(package, '/tmp/keiland.deb')
    guest.copy(client, '/tmp/vk-chain-test')
    guest.ssh('sudo apt-get update -qq && sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y /tmp/keiland.deb', timeout=1200)
    guest.ssh('sudo sh -eu -c ' + shlex.quote('''
        dpkg-query -W keiland
        test -f /usr/share/wayland-sessions/keiland.desktop
        test -f /opt/keiland/share/fonts/keiland.ttf
        test ! -e /opt/keiland/bin/mview
        test ! -e /opt/keiland/bin/kuidemo
        for p in /opt/keiland/bin/* /opt/keiland/lib/* /opt/keiland/libexec/*; do
            ldd "$p" > /tmp/ldd-check; ! grep -q 'not found' /tmp/ldd-check
        done
        install -d -m 700 /tmp/keiland-runtime
        env -u DISPLAY -u WAYLAND_DISPLAY KEILAND_DRM_DEVICE=none XDG_RUNTIME_DIR=/tmp/keiland-runtime /tmp/vk-chain-test
        mkdir -p /root/.config/keiland
        echo preserved > /root/.config/keiland/deb-smoke
        echo '# package smoke edit' >> /opt/keiland/etc/keiland/apps.conf
        dpkg --force-confold -i /tmp/keiland.deb
        grep -q 'package smoke edit' /opt/keiland/etc/keiland/apps.conf
        dpkg-deb --raw-extract /tmp/keiland.deb /tmp/keiland-upgrade
        sed -i '/^Version:/s/$/+smoke1/' /tmp/keiland-upgrade/DEBIAN/control
        dpkg-deb -Znone --build --root-owner-group /tmp/keiland-upgrade /tmp/keiland-upgrade.deb
        dpkg --force-confold -i /tmp/keiland-upgrade.deb
        dpkg-query -W -f='${Version}' keiland | grep -q '+smoke1$'
        grep -q 'package smoke edit' /opt/keiland/etc/keiland/apps.conf
    '''), timeout=600)
    guest.ssh('sudo systemd-run --unit=keiland-deb-smoke --setenv=HOME=/root --setenv=WAYLAND_DISPLAY=wayland-keiland --setenv=XDG_RUNTIME_DIR=/tmp/keiland-runtime --setenv=KEILAND_SEAT=direct /opt/keiland/bin/wayland --session --glass --wallpaper=/opt/keiland/share/keiland/wallpaper.png', timeout=30)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        try:
            guest.ssh('sudo test -S /tmp/keiland-runtime/wayland-keiland', stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            break
        except subprocess.CalledProcessError:
            time.sleep(1)
    else:
        diagnostic = guest.ssh('sudo journalctl -u keiland-deb-smoke --no-pager -o cat; sudo ls -l /dev/dri',
                               capture_output=True, text=True, timeout=20).stdout
        (output / (distro + '-failed-desktop.log')).write_text(diagnostic)
        guest.screenshot(output / (distro + '-failed-desktop.png'))
        raise RuntimeError('installed compositor failed to create its socket')
    guest.ssh('sudo systemd-run --unit=keiland-deb-terminal --setenv=HOME=/root --setenv=XDG_RUNTIME_DIR=/tmp/keiland-runtime --setenv=WAYLAND_DISPLAY=wayland-keiland /opt/keiland/bin/terminal', timeout=30)
    time.sleep(4)
    # App evidence is journal output via SSH, never the guest console or serial log.
    events = guest.ssh('sudo journalctl -u keiland-deb-smoke --no-pager -o cat', capture_output=True, text=True).stdout
    (output / (distro + '-desktop.log')).write_text(events)
    if 'ZWL MAP' not in events or 'ZWL VULKAN_ERROR' in events or 'ZWL ERROR' in events:
        raise RuntimeError('installed terminal did not map without protocol/GPU errors')
    matches = re.findall(r'ZWL MAP client=\d+ surface=\d+ x=(-?\d+) y=(-?\d+)', events)
    if not matches:
        raise RuntimeError('terminal position was not reported')
    guest.screenshot(output / (distro + '-desktop.png'))
    spec = importlib.util.spec_from_file_location('png_probe', ROOT / 'plan/tools/keiland-linux/png-probe.py')
    png = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(png)
    width, height, *_ = png.read_png(output / (distro + '-desktop.png'))
    x, y = map(int, matches[-1])
    guest.qmp('input-send-event', {'device': 'video0', 'head': 0, 'events': [
        {'type': 'abs', 'data': {'axis': 'x', 'value': (x + 200) * 32767 // (width - 1)}},
        {'type': 'abs', 'data': {'axis': 'y', 'value': (y + 200) * 32767 // (height - 1)}}]})
    for down in (True, False):
        guest.qmp('input-send-event', {'device': 'video0', 'head': 0, 'events': [
            {'type': 'btn', 'data': {'down': down, 'button': 'left'}}]})
    # TCG software composition can take seconds; allow focus to reach the client
    # before sending keyboard events, then observe the command's actual effect.
    time.sleep(5 if guest.acceleration == 'tcg' else 1)
    guest.type('echo package-smoke\ntouch /tmp/package-smoke\n')
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            guest.ssh('sudo test -f /tmp/package-smoke', stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            break
        except subprocess.CalledProcessError:
            time.sleep(1)
    else:
        diagnostic = guest.ssh('sudo journalctl -u keiland-deb-terminal -u keiland-deb-smoke --no-pager -o cat', capture_output=True, text=True).stdout
        (output / (distro + '-failed-input.log')).write_text(diagnostic)
        guest.screenshot(output / (distro + '-failed-input.png'))
        raise RuntimeError('terminal input did not create the expected file within 30 seconds')
    guest.screenshot(output / (distro + '-terminal.png'))
    guest.ssh('sudo systemctl is-active --quiet keiland-deb-smoke && sudo systemctl is-active --quiet keiland-deb-terminal')
    guest.ssh('sudo systemctl stop keiland-deb-terminal keiland-deb-smoke')
    guest.ssh('sudo sh -eu -c ' + shlex.quote('''
        dpkg --remove keiland
        test -f /opt/keiland/etc/keiland/apps.conf
        test ! -e /opt/keiland/bin/wayland
        test ! -e /usr/share/wayland-sessions/keiland.desktop
        test "$(cat /root/.config/keiland/deb-smoke)" = preserved
        dpkg --purge keiland
        test "$(cat /root/.config/keiland/deb-smoke)" = preserved
    '''))
    (output / (distro + '.smoke.json')).write_text(json.dumps({
        'distro': distro, 'package_sha256': digest(package), 'fresh_overlay': True, 'install': True, 'reinstall': True, 'upgrade': True,
        'elf_dependencies': True, 'public_vulkan_client': True, 'direct_session': True,
        'terminal_map_and_input': True, 'remove': True, 'purge_keeps_user_data': True,
        'display_manager_registration': True, 'acceleration': guest.acceleration}, indent=2) + '\n')


def main():
    signal.signal(signal.SIGTERM, interrupted)
    parser = argparse.ArgumentParser()
    parser.add_argument('distro', choices=INPUTS)
    args = parser.parse_args()
    os.chdir(ROOT)
    base = Path(os.environ.get('KEILAND_DEB_BUILD', ROOT / 'build/keiland-deb')).resolve()
    base.mkdir(parents=True, exist_ok=True)
    output = base / 'artifacts' / args.distro
    with (base / (args.distro + '.lock')).open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        # This driver owns only its distribution output directory. Reject stale
        # successful output before starting a new release attempt.
        if output.exists():
            shutil.rmtree(output)
        output.mkdir(parents=True)
        source = image(base, args.distro)
        with tempfile.TemporaryDirectory(prefix=args.distro + '-', dir=base) as temporary:
            work = Path(temporary)
            archive = work / 'source.tar.gz'
            version, commit, epoch, dirty = snapshot(archive)
            source_hash = digest(archive)
            with Guest(work / 'build', source, args.distro + '-build') as guest:
                check_os(guest, args.distro, output)
                guest.ssh('sudo apt-get update -qq && sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y ' + ' '.join(BUILD_PACKAGES), timeout=1200)
                guest.copy(archive, '/tmp/source.tar.gz')
                guest.ssh('mkdir /tmp/keiland-source; tar -C /tmp/keiland-source -xzf /tmp/source.tar.gz')
                command = ['python3', 'tools/release/keiland-linux-deb/build.py', args.distro, version, source_hash, commit]
                if dirty:
                    command.append('--source-dirty')
                guest.ssh('cd /tmp/keiland-source && SOURCE_DATE_EPOCH=' + str(epoch) + ' ' + shlex.join(command), timeout=1500)
                guest.ssh('tar -C /tmp/keiland-output -czf /tmp/output.tar.gz .')
                guest.copy('/tmp/output.tar.gz', work / 'output.tar.gz', retrieve=True)
            extracted = work / 'output'
            extracted.mkdir()
            with tarfile.open(work / 'output.tar.gz') as built:
                built.extractall(extracted, filter='data')
            packages = list(extracted.glob('*.deb'))
            if len(packages) != 1:
                raise RuntimeError('expected one native deb')
            candidate = base / 'candidates' / args.distro
            if candidate.exists():
                shutil.rmtree(candidate)
            candidate.mkdir(parents=True)
            for path in extracted.iterdir():
                shutil.copy2(path, candidate / path.name)
            with Guest(work / 'test', source, args.distro + '-test') as guest:
                check_os(guest, args.distro, output)
                smoke(guest, packages[0], extracted / 'vk-chain-test', output, args.distro)
            for path in extracted.iterdir():
                if path.name != 'vk-chain-test':
                    shutil.copy2(path, output / path.name)
            print('verified release artifacts:', output, flush=True)


if __name__ == '__main__':
    main()

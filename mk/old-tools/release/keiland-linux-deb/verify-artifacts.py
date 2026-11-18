#!/usr/bin/env python3
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Fail CI/release when a distribution package or its evidence is incomplete."""
import argparse
import hashlib
import json
from pathlib import Path


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(directory, distro, require_clean):
    packages = list(directory.glob('keiland_*+' + distro + '_amd64.deb'))
    if len(packages) != 1:
        raise RuntimeError(distro + ': expected exactly one native deb')
    package = packages[0]
    stem = package.name.removesuffix('.deb')
    expected = {package.name, stem + '.manifest.json', stem + '.buildinfo.json'}
    checked = set()
    for line in (directory / (stem + '.sha256')).read_text().splitlines():
        digest, name = line.split('  ', 1)
        if name not in expected or name in checked:
            raise RuntimeError('unexpected or repeated checksum entry: ' + name)
        if sha256(directory / name) != digest:
            raise RuntimeError('checksum mismatch: ' + name)
        checked.add(name)
    if checked != expected:
        raise RuntimeError(distro + ': incomplete checksum set')
    info = json.loads((directory / (stem + '.buildinfo.json')).read_text())
    if info['distro'] != distro or info['architecture'] != 'amd64' or info['sha256'] != sha256(package):
        raise RuntimeError(distro + ': native build metadata mismatch')
    if require_clean and info.get('source_dirty', True):
        raise RuntimeError(distro + ': CI source must be clean')
    manifest = json.loads((directory / (stem + '.manifest.json')).read_text())
    paths = [entry['path'] for entry in manifest]
    if len(paths) != len(set(paths)) or '/usr/share/wayland-sessions/keiland.desktop' not in paths:
        raise RuntimeError(distro + ': payload/session manifest mismatch')
    for name in ('wlshm', 'wltest', 'vkdemo', 'mview', 'kuidemo'):
        if '/opt/keiland/bin/' + name in paths:
            raise RuntimeError(distro + ': test app in runtime manifest')
    smoke = json.loads((directory / (distro + '.smoke.json')).read_text())
    checks = ('fresh_overlay', 'install', 'reinstall', 'upgrade', 'elf_dependencies',
              'public_vulkan_client', 'direct_session', 'terminal_map_and_input',
              'remove', 'purge_keeps_user_data', 'display_manager_registration')
    if smoke['distro'] != distro or smoke['package_sha256'] != sha256(package):
        raise RuntimeError(distro + ': runtime evidence does not match package')
    if any(smoke.get(check) is not True for check in checks):
        raise RuntimeError(distro + ': incomplete runtime evidence')
    for name in ('-console.png', '-desktop.png', '-terminal.png'):
        if (directory / (distro + name)).read_bytes()[:8] != b'\x89PNG\r\n\x1a\n':
            raise RuntimeError(distro + ': missing PNG evidence')
    print(distro + ': verified ' + package.name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--require-clean', action='store_true')
    parser.add_argument('directory', type=Path)
    parser.add_argument('distros', nargs='+', choices=('debian13', 'ubuntu2604'))
    args = parser.parse_args()
    for distro in args.distros:
        verify(args.directory, distro, args.require_clean)


if __name__ == '__main__':
    main()

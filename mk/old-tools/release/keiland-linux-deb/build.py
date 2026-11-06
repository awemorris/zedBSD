#!/usr/bin/env python3
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Build the runtime deb natively: inside the selected QEMU guest (debian13, ubuntu2604) or a target rootfs (rootfs.py)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def run(args, **kwargs):
    return subprocess.run(args, check=True, timeout=kwargs.pop('timeout', 120), **kwargs)


def main():
    parser = argparse.ArgumentParser()
    rootfs = json.loads(Path('tools/release/keiland-linux-deb/rootfs.json').read_text())
    parser.add_argument('distro', choices=('debian13', 'ubuntu2604', 'deb13-amd64', 'deb13-arm64', 'rpios13-arm64'))
    parser.add_argument('version')
    parser.add_argument('source_hash')
    parser.add_argument('commit')
    parser.add_argument('--source-dirty', action='store_true')
    args = parser.parse_args()
    # A QEMU guest's (inputs.json) or a target rootfs's (rootfs.json, WS112 p002/p003) distribution and CPU.
    if args.distro in rootfs:
        target = rootfs[args.distro]
        architecture = target['architecture']
        suffix = target['suffix']
        label = target['label']
    else:
        target = json.loads(Path('tools/release/keiland-linux-deb/inputs.json').read_text())[args.distro]
        architecture = 'amd64'
        suffix = args.distro
        label = args.distro
    release = dict(line.split('=', 1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)
    if release['ID'].strip('"') != target['id'] or release['VERSION_ID'].strip('"') != target['version']:
        raise RuntimeError('native build environment does not match the requested distribution')
    if run(['dpkg', '--print-architecture'], capture_output=True, text=True).stdout.strip() != architecture:
        raise RuntimeError('native build environment is not ' + architecture)
    if target.get('raspberrypi'):
        run(['dpkg-query', '-W', 'raspberrypi-archive-keyring'], stdout=subprocess.DEVNULL)
    version = args.version + '-1+' + suffix
    output = Path('/tmp/keiland-output')
    output.mkdir()
    stage = Path('build/deb-stage').resolve()
    env = dict(os.environ, LC_ALL='C.UTF-8')
    make = ['make', '-j' + str(os.cpu_count() or 4), '-f', 'userland/desktop/keiland-linux.mk', 'KEILAND_LINUX_BUILD=build/deb-native']
    run([*make, 'all'], timeout=7200, env=env)
    # Install goals share staging parents; serialize them even with -j4.
    stage.mkdir(parents=True)
    run([*make, 'install', 'DESTDIR=' + str(stage)], env=env, timeout=1200)
    run([*make, 'install-session', 'DESTDIR=' + str(stage)], env=env)
    prefix = stage / 'opt/keiland'
    for name in ('wlshm', 'wltest', 'vkdemo', 'mview', 'kuidemo'):
        (prefix / 'bin' / name).unlink()
    shutil.rmtree(prefix / 'share/mview')
    config = prefix / 'etc/keiland/apps.conf'
    config.write_text(''.join(line for line in config.read_text().splitlines(True)
                              if not line.startswith(('Model viewer|', 'Widget Demo|'))))
    run(['sh', 'plan/tools/keiland-linux/elf-check.sh', str(stage)])
    notice = prefix / 'share/doc/keiland'
    notice.mkdir(parents=True)
    shutil.copy2('LICENSE', notice / 'copyright')
    (notice / 'README').write_text(
        'Keiland for ' + label + '\n\n'
        'Run /opt/keiland/bin/keiland-desktop as root on a text console,\n'
        'or select Keiland in a display manager. Vulkan uses the system driver.\n'
        'WiFi uses a system wpa_supplicant control socket; IP configuration is\n'
        'provided by the distribution. Audio uses the kernel ALSA interface.\n'
        'Fonts and notices are in /opt/keiland/share/fonts and share/licenses.\n'
        'The project dictionary uses the project license (WS095 relicensing).\n'
        'No test apps or development headers are included.\n')
    control = stage / 'DEBIAN'
    control.mkdir()
    # Private libraries are in this package; the versioned Vulkan proxy must not
    # create a self-dependency or replace the system Vulkan backend dependency.
    debian = Path('debian')
    debian.mkdir()
    (debian / 'control').write_text('Source: keiland\nMaintainer: Awe Morris <awemorris@users.noreply.github.com>\n\nPackage: keiland\nArchitecture: ' + architecture + '\nDescription: Keiland desktop\n')
    (debian / 'shlibs.local').write_text('libvulkan 1 keiland\n')
    elf = [p for p in prefix.rglob('*') if p.is_file() and p.read_bytes()[:4] == b'\x7fELF']
    dependencies = run(['dpkg-shlibdeps', '-O', '-xkeiland', '-l' + str(prefix / 'lib'),
                        *[str(p) for p in elf]], capture_output=True, text=True, timeout=1800)
    print(dependencies.stderr, end='')
    needed = dependencies.stdout.strip().removeprefix('shlibs:Depends=')
    if not needed or 'libc6' not in needed:
        raise RuntimeError('native shared-library dependencies were not computed')
    needed += ', libvulkan1, mesa-vulkan-drivers, libpam-systemd, kbd'
    size = sum(p.stat().st_size for p in prefix.rglob('*') if p.is_file()) // 1024
    (control / 'control').write_text(
        f'Package: keiland\nVersion: {version}\nArchitecture: {architecture}\n'
        'Maintainer: Awe Morris <awemorris@users.noreply.github.com>\nSection: x11\nPriority: optional\n'
        f'Installed-Size: {size}\nDepends: {needed}\n'
        'Suggests: gdm3, wpasupplicant\nHomepage: https://github.com/awemorris/zedBSD\n'
        'Description: Keiland desktop for ' + label + '\n'
        ' Independent compositor and applications under /opt/keiland,\n'
        ' using the distribution Vulkan driver and Linux service backends.\n')
    (control / 'conffiles').write_text('/opt/keiland/etc/keiland/apps.conf\n')
    manifest = []
    md5sums = []
    for p in sorted(stage.rglob('*')):
        if not p.is_file() or control in p.parents:
            continue
        manifest.append({'path': '/' + p.relative_to(stage).as_posix(),
                         'sha256': hashlib.sha256(p.read_bytes()).hexdigest(),
                         'mode': oct(p.stat().st_mode & 0o777)})
        md5sums.append(hashlib.md5(p.read_bytes(), usedforsecurity=False).hexdigest()
                       + '  ' + p.relative_to(stage).as_posix() + '\n')
    (control / 'md5sums').write_text(''.join(md5sums))
    # A public Vulkan client is a verification asset, never part of the runtime package.
    run(['cc', '-Wall', '-Wextra', '-Werror', '-o', str(output / 'vk-chain-test'),
         'plan/tools/keiland-linux/vk-chain-test.c', '-ldl', '-Wl,--no-as-needed',
         '-Lbuild/deb-native/lib', '-l:libvulkan.so.1',
         '-Wl,-rpath-link,build/deb-native/lib', '-Wl,-rpath,/opt/keiland/lib'])
    package = output / f'keiland_{version}_{architecture}.deb'
    run(['dpkg-deb', '--build', '--root-owner-group', str(stage), str(package)], timeout=3600)
    stem = package.name.removesuffix('.deb')
    (output / (stem + '.manifest.json')).write_text(json.dumps(manifest, indent=2) + '\n')
    packages = run(['dpkg-query', '-W', '-f=${binary:Package}\t${Version}\n'], capture_output=True, text=True).stdout
    buildinfo = {'format': 'keiland-buildinfo-1', 'distro': args.distro, 'os_release': release,
                 'architecture': architecture, 'version': version, 'source_commit': args.commit,
                 'source_archive_sha256': args.source_hash, 'source_dirty': args.source_dirty,
                 'guest_image': target,
                 'compiler': run(['cc', '--version'], capture_output=True, text=True).stdout,
                 'installed_build_packages': packages.splitlines(), 'depends': needed,
                 'sha256': hashlib.sha256(package.read_bytes()).hexdigest()}
    (output / (stem + '.buildinfo.json')).write_text(json.dumps(buildinfo, indent=2) + '\n')
    for p in sorted(output.glob('*.json')):
        print('metadata:', p.name)
    files = [package, *sorted(output.glob('*.json'))]
    (output / (stem + '.sha256')).write_text(''.join(
        hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n' for p in files))
    print('native package:', package.name)


if __name__ == '__main__':
    main()

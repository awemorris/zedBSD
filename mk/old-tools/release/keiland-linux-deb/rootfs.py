#!/usr/bin/env python3
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Build a Keiland deb natively in a throwaway target rootfs and check it (WS112 p002, p003).

    rootfs.py deb13-amd64|deb13-arm64|rpios13-arm64

mmdebstrap (unshare mode, no root) makes the target's rootfs from its archives (rootfs.json), with
the build packages; build.py builds the deb inside it with the target's own compiler, headers and C
library (arm64 runs through the host's qemu-aarch64 binfmt: user-mode, no VM).  The deb is then
checked: its fields, every ELF's machine, no test program, and its dependencies resolved by apt
(--simulate) in a fresh rootfs of each distribution it is for.  mmdebstrap removes its own trees.
Each run writes a new directory, KEILAND_DEB_BUILD/rootfs/TARGET/STAMP; nothing is deleted.
"""
import argparse
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
CONFIG = json.loads((HERE / 'rootfs.json').read_text())

# Each architecture's ELF machine (EM_X86_64, EM_AARCH64).
MACHINES = {'amd64': 62, 'arm64': 183}

# What the runtime package must not carry: the test and demo programs and their data.
EXCLUDED = ('./opt/keiland/bin/mview', './opt/keiland/bin/kuidemo', './opt/keiland/bin/wlshm',
            './opt/keiland/bin/wltest', './opt/keiland/bin/vkdemo', './opt/keiland/share/mview/')

# The Debian archive's keys, for a Debian or a Raspberry Pi OS rootfs.
DEBIAN_KEYRING = '/usr/share/keyrings/debian-archive-keyring.gpg'


def run(args, **kwargs):
    return subprocess.run(args, check=True, timeout=kwargs.pop('timeout', 120), **kwargs)


def snapshot_function():
    """run.py's source snapshot: the same archive the QEMU guests build from."""
    spec = importlib.util.spec_from_file_location('keiland_deb_run', HERE / 'run.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.snapshot


def raspberrypi_keyring(directory):
    """
    The Raspberry Pi archive's keyring as apt (sqv) takes it.  The key at rootfs.json's URL, its
    fingerprint checked, verifies the archive's InRelease (gpgv); its SHA256 of the arm64 Packages
    leads to raspberrypi-archive-keyring's deb, whose keyring carries the same key with binding
    signatures sqv's policy accepts (the published key's are SHA-1).  apt reads the keyring as the
    namespace's root, which cannot enter a private home: it is kept in KEILAND_DEB_TMPDIR (default
    /var/tmp) under its hash, made once and reused.
    """
    config = CONFIG['raspberrypi_key']
    archive = config['archive']

    # The published key, by its fingerprint.
    armored = directory / 'raspberrypi.gpg.key'
    fetch(config['url'], armored)
    listing = run(['gpg', '--batch', '--with-colons', '--show-keys', str(armored)],
                  capture_output=True, text=True).stdout
    fingerprints = [line.split(':')[9] for line in listing.splitlines() if line.startswith('fpr:')]
    if not fingerprints or fingerprints[0] != config['fingerprint']:
        raise RuntimeError('Raspberry Pi archive key fingerprint mismatch: ' + ' '.join(fingerprints))
    published = directory / 'raspberrypi.gpg'
    run(['gpg', '--batch', '--yes', '--dearmor', '-o', str(published), str(armored)])

    # The archive's InRelease, signed by it.
    release = directory / 'InRelease'
    fetch(archive + '/dists/trixie/InRelease', release)
    verdict = run(['gpgv', '--keyring', str(published), str(release)], capture_output=True, text=True).stderr
    if config['fingerprint'] not in verdict.replace(' ', ''):
        raise RuntimeError('InRelease not signed by the pinned key')

    # The arm64 Packages by its SHA256 there, and the keyring package by its SHA256 in it.
    packages_name = 'main/binary-arm64/Packages.gz'
    expected = released_sha256(release.read_text(), packages_name)
    packages = directory / 'Packages.gz'
    fetch(archive + '/dists/trixie/' + packages_name, packages)
    if sha256(packages) != expected:
        raise RuntimeError('Packages.gz hash mismatch')
    stanza = package_stanza(gzip.decompress(packages.read_bytes()).decode(), 'raspberrypi-archive-keyring')
    package = directory / 'raspberrypi-archive-keyring.deb'
    fetch(archive + '/' + stanza['Filename'], package)
    if sha256(package) != stanza['SHA256']:
        raise RuntimeError('raspberrypi-archive-keyring hash mismatch')

    # Its keyring, which must carry the pinned key.
    data = run(['dpkg-deb', '--fsys-tarfile', str(package)], capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:') as contents:
        keyring_data = contents.extractfile('./usr/share/keyrings/raspberrypi-archive-keyring.pgp').read()
    keys = Path(os.environ.get('KEILAND_DEB_TMPDIR', '/var/tmp')) / 'keiland-deb-keys'
    keys.mkdir(mode=0o755, exist_ok=True)
    keyring = keys / ('raspberrypi-archive-keyring-' + hashlib.sha256(keyring_data).hexdigest()[:16] + '.pgp')
    if not keyring.exists():
        keyring.write_bytes(keyring_data)
        keyring.chmod(0o644)
    listing = run(['gpg', '--batch', '--with-colons', '--show-keys', str(keyring)], capture_output=True, text=True).stdout
    if config['fingerprint'] not in listing:
        raise RuntimeError('the packaged keyring lacks the pinned key')
    return keyring


def fetch(url, path):
    """Downloads a file, bounded in time."""
    run(['curl', '-fsSL', '--retry', '2', '--max-time', '300', url, '-o', str(path)], timeout=320)


def sha256(path):
    """A file's SHA-256 in hexadecimal."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def released_sha256(text, name):
    """The SHA256 an InRelease gives a file."""
    section = text.split('\nSHA256:\n', 1)[1]
    for line in section.splitlines():
        if not line.startswith(' '):
            break
        fields = line.split()
        if len(fields) == 3 and fields[2] == name:
            return fields[0]
    raise RuntimeError('no SHA256 for ' + name)


def package_stanza(text, name):
    """A package's fields in a Packages file (the highest version is not chosen: the first is taken)."""
    for stanza in text.split('\n\n'):
        fields = dict(line.split(': ', 1) for line in stanza.splitlines() if ': ' in line and not line.startswith(' '))
        if fields.get('Package') == name:
            return fields
    raise RuntimeError('no package ' + name)


def mirrors(lines, keyrings):
    """The mirror lines with the Raspberry Pi keyring's path put in."""
    path = str(keyrings[0]) if keyrings else ''
    return [line.replace('{raspberrypi_keyring}', path) for line in lines]


def mmdebstrap(architecture, suite, mirrors, include, keyrings, hooks, log, timeout):
    """One throwaway rootfs (--format=null): its hooks run in it, then mmdebstrap removes it."""
    command = ['mmdebstrap', '--mode=unshare', '--variant=apt', '--format=null', '--architectures=' + architecture]
    if include:
        command.append('--include=' + ','.join(include))
    for keyring in keyrings:
        command.append('--keyring=' + str(keyring))
    for hook in hooks:
        command.append('--customize-hook=' + hook)
    command += [suite, '/dev/null', *mirrors]
    # The rootfs is made where the namespace's root (a subordinate uid) can go: not under a private home.
    environment = dict(os.environ, LC_ALL='C.UTF-8', TMPDIR=os.environ.get('KEILAND_DEB_TMPDIR', '/var/tmp'))
    with log.open('w') as stream:
        stream.write('$ ' + shlex.join(command) + '\n')
        stream.flush()
        run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout, env=environment)


def inspect(package, architecture):
    """The deb's fields, and its files: every ELF of the architecture, no test program."""
    fields = run(['dpkg-deb', '--field', str(package)], capture_output=True, text=True).stdout
    values = dict(line.split(': ', 1) for line in fields.splitlines() if ': ' in line and not line.startswith(' '))
    if values.get('Package') != 'keiland' or values.get('Architecture') != architecture:
        raise RuntimeError('package fields: ' + values.get('Package', '?') + ' ' + values.get('Architecture', '?'))
    data = run(['dpkg-deb', '--fsys-tarfile', str(package)], capture_output=True, timeout=600).stdout
    elves = 0
    files = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:') as archive:
        for member in archive:
            if any(member.name == path or member.name.startswith(path) for path in EXCLUDED if path.endswith('/')) or \
                    member.name in EXCLUDED:
                raise RuntimeError('test program in the package: ' + member.name)
            if not member.isfile():
                continue
            files += 1
            head = archive.extractfile(member).read(20)
            if head[:4] != b'\x7fELF':
                continue
            elves += 1
            machine = int.from_bytes(head[18:20], 'little')
            if machine != MACHINES[architecture]:
                raise RuntimeError('ELF of another machine (%d): %s' % (machine, member.name))
    if elves == 0:
        raise RuntimeError('no ELF in the package')
    return {'fields': values, 'files': files, 'elf': elves, 'elf_machine': MACHINES[architecture]}


def resolve(package, architecture, name, directory, keyrings):
    """apt's --simulate of the deb in a fresh rootfs of one distribution; the transcript and its status."""
    environment = CONFIG['environments'][name]
    keys = [environment['keyring']] if 'keyring' in environment else [DEBIAN_KEYRING]
    out = directory / ('check-' + name)
    out.mkdir()
    script = 'apt-get install --simulate /tmp/' + package.name + ' > /tmp/simulate.txt 2>&1; echo exit=$? >> /tmp/simulate.txt'
    hooks = ['copy-in ' + shlex.quote(str(package)) + ' /tmp',
             'chroot "$1" sh -c ' + shlex.quote(script),
             'copy-out /tmp/simulate.txt ' + shlex.quote(str(out))]
    mmdebstrap(architecture, environment['suite'], mirrors(environment['mirrors'][architecture], keyrings), [], keys, hooks,
               directory / ('check-' + name + '.log'), 3600)
    transcript = (out / 'simulate.txt').read_text()
    return {'environment': name, 'resolved': transcript.rstrip().endswith('exit=0'),
            'installs': [line.split()[1] for line in transcript.splitlines() if line.startswith('Inst ')]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('target', choices=[name for name in CONFIG if name.endswith(('-amd64', '-arm64'))])
    args = parser.parse_args()
    os.chdir(ROOT)
    target = CONFIG[args.target]
    architecture = target['architecture']

    # A new directory for this run.
    base = Path(os.environ.get('KEILAND_DEB_BUILD', ROOT / 'build/keiland-deb')).resolve()
    commit = run(['git', 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    directory = base / 'rootfs' / args.target / (time.strftime('%Y%m%dT%H%M%S', time.gmtime()) + '-' + commit[:12])
    directory.mkdir(parents=True)
    print('rootfs build:', directory, flush=True)

    # The source, and the Raspberry Pi archive's key when it is used.
    archive = directory / 'source.tar.gz'
    version, commit, epoch, dirty = snapshot_function()(archive)
    source_hash = run(['sha256sum', str(archive)], capture_output=True, text=True).stdout.split()[0]
    keyrings = [raspberrypi_keyring(directory)] if target.get('raspberrypi') else []

    # The build in the target's rootfs.
    output = directory / 'out'
    output.mkdir()
    command = ['python3', 'tools/release/keiland-linux-deb/build.py', args.target, version, source_hash, commit]
    if dirty:
        command.append('--source-dirty')
    script = ('mkdir /tmp/keiland-source && tar -C /tmp/keiland-source -xzf /tmp/source.tar.gz && '
              'cd /tmp/keiland-source && ' + shlex.join(command))
    include = list(CONFIG['build_packages'])
    if target.get('raspberrypi'):
        include.append('raspberrypi-archive-keyring')
    hooks = ['copy-in ' + shlex.quote(str(archive)) + ' /tmp',
             'chroot "$1" env SOURCE_DATE_EPOCH=%d LC_ALL=C.UTF-8 sh -c %s' % (epoch, shlex.quote(script)),
             'sync-out /tmp/keiland-output ' + shlex.quote(str(output))]
    started = time.monotonic()
    mmdebstrap(architecture, target['suite'], mirrors(target['mirrors'], keyrings), include, [DEBIAN_KEYRING], hooks,
               directory / 'build.log', 14400)
    elapsed = int(time.monotonic() - started)
    packages = sorted(output.glob('*.deb'))
    if len(packages) != 1:
        raise RuntimeError('expected one deb in ' + str(output))
    package = packages[0]
    print('built:', package.name, '(%d s)' % elapsed, flush=True)

    # The checks: the package itself, then its dependencies in each distribution it is for.
    record = {'target': args.target, 'label': target['label'], 'package': package.name, 'build_seconds': elapsed,
              'inspection': inspect(package, architecture), 'resolution': []}
    for name in target['checks']:
        result = resolve(package, architecture, name, directory, keyrings)
        record['resolution'].append(result)
        print('dependencies on %s: %s' % (name, 'resolved' if result['resolved'] else 'NOT resolved'), flush=True)
    (directory / 'check.json').write_text(json.dumps(record, indent=2) + '\n')
    if not all(result['resolved'] for result in record['resolution']):
        raise RuntimeError('dependencies not resolved; see ' + str(directory))
    print('verified:', package, flush=True)


if __name__ == '__main__':
    main()

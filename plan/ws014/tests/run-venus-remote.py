#!/usr/bin/env python3
"""Build a zedBSD image and capture one isolated remote QEMU/Venus attempt.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

The configured SSH account receives the complete test image.  The user has
explicitly authorized this transfer to awe@10.0.10.25 for WS014 p003.  The build
uses its own output directory; configuration changes affect a disposable image.
config.mk, existing remote VMs and system configuration are never modified.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import signal
import stat
import struct
import subprocess
import sys
import time
import zlib


REPO = Path(__file__).resolve().parents[3]
SSH_OPTIONS = ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
               '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=2']
EVIDENCE_FILES = ['result.json', 'guest.log', 'qemu-renderer.log', 'qmp.jsonl',
                  'boot.ppm', 'frame.ppm', 'console.log', 'console.bin']
SOURCE_DIRECTORIES = ['src/drivers/gpu', 'userland/gpu']
SOURCE_FILES = ['Makefile', 'config/kernel-options.list',
                'plan/ws014/tests/config-wayland-amd64.mk', 'include/drivers/gpu/gpu.h', 'include/drivers/pci/pci-venus.h',
                'userland/base/libc/pthread.c', 'include/libc/sys/socket.h',
                'include/drivers/gpu/gpu-fence.h', 'src/drivers/gpu/gpu-fence.c', 'include/uapi/gpu-fence.h',
                'include/uapi/gpu-scanout.h', 'include/drivers/gpu/gpu-scanout.h',
                'include/kern/handle.h', 'include/kern/fd-object.h',
                'include/kern/filedesc.h', 'src/kern/handle.c',
                'src/kern/fd-object.c', 'src/kern/filedesc.c', 'src/kern/poll.c',
                'include/uapi/gpu.h', 'include/uapi/gpu-allocation.h',
                'include/uapi/gpu-job.h',
                'src/kern/platform/pcat.c', 'src/hal/amd64/asm.c',
                'platform/amd64/vmunix.mk', 'platform/amd64/zedbsd.cfg',
                'include/hal/hal.h', 'src/hal/amd64/space.c',
                'include/kern/vm-device.h', 'include/kern/vmspace.h',
                'include/kern/cdev.h', 'include/kern/file.h',
                'src/kern/vm-device.c', 'src/kern/vmspace.c', 'src/kern/uaccess.c',
                'src/kern/syscall.c', 'src/kern/cdev.c',
                'include/drivers/gpu/gpu-display.h', 'include/uapi/gpu-display.h',
                'include/drivers/pci/pci.h', 'src/drivers/pci/pci-pcat.c',
                'include/kern/text-display.h', 'src/kern/text-display.c',
                'src/drivers/platform/pcat/graphics/text.c',
                'src/drivers/platform/pcat/graphics/text.h',
                'src/drivers/platform/pcat/graphics/backend.c',
                'config/drivers/pci.drivers', 'plan/ws014/tests/venus-qemu.py',
                'plan/ws014/tests/run-venus-remote.py', 'plan/ws014/tests/venus_rfb.py']

PROFILES = {
    'venus': {'application': 'venus-frame', 'harness': 'venus-qemu.py',
              'modules': {'rfb_client': 'venus_rfb.py'},
              'evidence': EVIDENCE_FILES, 'source_directories': [], 'source_files': []},
    'vkdemo': {'application': 'vkdemo', 'harness': 'vkdemo-qemu.py',
               'modules': {'rfb_client': 'venus_rfb.py', 'transport_harness': 'venus-qemu.py',
                           'oracle': 'vkdemo_oracle.py'},
               'evidence': [name for name in EVIDENCE_FILES if name != 'frame.ppm'] +
                           [f'frame-{index}.ppm' for index in range(1, 7)] +
                           [f'oracle-{index}.json' for index in range(1, 7)] +
                           ['console-return.ppm', 'console-write.ppm',
                            'ordinary-1.ppm', 'ordinary-2.ppm'],
               'source_directories': ['userland/base/vkdemo', 'userland/base/libvulkan',
                                      'include/libc/vulkan'],
               'source_files': ['userland/base/common/sha256.c', 'userland/base/common/sha256.h',
                                'plan/ws014/tests/vkdemo-qemu.py',
                                'plan/ws014/tests/vkdemo_oracle.py',
                                'plan/ws014/tests/run-vkdemo-remote.py']},
    'wayland': {'application': 'wltest', 'harness': 'wayland-qemu.py',
                'modules': {'rfb_client': 'venus_rfb.py', 'transport_harness': 'venus-qemu.py',
                            'oracle': 'wayland_oracle.py'},
                'evidence': [name for name in EVIDENCE_FILES if name != 'frame.ppm'] +
                            [f'frame-{index}.ppm' for index in range(1, 13)] +
                            [f'oracle-{index}.json' for index in range(1, 13)] +
                            ['console-return.ppm', 'console-write.ppm', 'console-killed.ppm',
                             'wayland-observed.log'],
                'source_directories': ['userland/base/wltest', 'userland/base/zdesktop',
                                       'userland/base/tests/gpu-share',
                                       'userland/base/tests/gpu-fence',
                                       'userland/base/tests/gpu-admission',
                                       'userland/base/tests/gpu-recovery',
                                       'userland/base/libwayland', 'include/libc/wayland',
                                       'userland/base/libvulkan', 'include/libc/vulkan'],
                'source_files': ['include/kern/handle.h', 'include/kern/fd-object.h',
                                 'include/drivers/gpu/gpu-fence.h', 'src/drivers/gpu/gpu-fence.c',
                                 'include/uapi/gpu-fence.h', 'include/uapi/gpu-scanout.h',
                                 'include/drivers/gpu/gpu-scanout.h',
                                 'include/kern/filedesc.h', 'include/kern/net/socket.h',
                                 'include/libc/wayland-client.h', 'include/libc/wayland-client-core.h',
                                 'include/libc/wayland-client-protocol.h', 'include/libc/wayland-util.h',
                                 'include/libc/xdg-shell-client-protocol.h',
                                 'src/kern/handle.c', 'src/kern/fd-object.c', 'src/kern/filedesc.c',
                                 'src/kern/net/unix-socket.c', 'src/kern/poll.c',
                                 'include/drivers/gpu/gpu-share.h', 'include/libc/errno.h', 'include/libc/stddef.h', 'src/libc/string.c',
                                 'userland/base/tests/gpu-share/main.c',
                                 'plan/ws014/tests/wayland-qemu.py',
                                 'plan/ws014/tests/wayland_oracle.py',
                                 'plan/ws014/tests/run-wayland-remote.py'],
                'additional_artifacts': {'vulkan_library': 'dynamic/libvulkan.so',
                                         'wayland_library': 'dynamic/libwayland-client.so',
                                         'compositor': 'bin/zdesktop',
                                         'sharing_test': 'bin/gpu-share-test',
                                         'fence_test': 'bin/gpu-fence-test',
                                         'recovery_test': 'bin/gpu-recovery-test',
                                         'admission_test': 'bin/gpu-admission-test'}},
}


PROFILES['mview'] = {
    'application': 'mview', 'harness': 'mview-qemu.py', 'harness_dir': 'plan/ws031/tests',
    'config': 'plan/ws031/tests/config-mview-amd64.mk',
    'modules': {'rfb_client': 'venus_rfb.py', 'transport_harness': 'venus-qemu.py'},
    'evidence': [name for name in EVIDENCE_FILES if name != 'frame.ppm'] +
                ['initial.ppm', 'rotate.ppm', 'pan.ppm', 'zoom.ppm', 'keys.ppm', 'reset.ppm',
                 'mview-observed.log'],
    'source_directories': ['userland/base/mview', 'userland/base/zdesktop', 'userland/base/libwayland',
                           'include/libc/wayland', 'userland/base/libvulkan', 'include/libc/vulkan'],
    'source_files': ['plan/ws031/tests/mview-qemu.py', 'plan/ws031/tests/run-mview-remote.py',
                     'plan/ws031/tests/config-mview-amd64.mk'],
    'additional_artifacts': {'vulkan_library': 'dynamic/libvulkan.so',
                             'wayland_library': 'dynamic/libwayland-client.so',
                             'compositor': 'bin/zdesktop'}}

# The profiles whose harness runs its own guest commands with a token.
TOKEN_PROFILES = ('vkdemo', 'wayland', 'mview')


def selected_profile(args):
    return PROFILES[getattr(args, 'profile', 'venus')]


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1048576), b''):
            result.update(chunk)
    return result.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def snapshot_sources(config, profile='venus'):
    selected = PROFILES[profile]
    paths = {REPO / name for name in SOURCE_FILES + selected['source_files']}
    paths.add(config)
    for name in SOURCE_DIRECTORIES + selected['source_directories']:
        paths.update(path for path in (REPO / name).rglob('*')
                     if path.is_file() and '.internal' not in path.parts)
    result = {}
    for path in sorted(paths):
        if not path.is_file():
            raise FileNotFoundError(f'required source artifact missing: {path}')
        try:
            name = str(path.relative_to(REPO))
        except ValueError:
            name = str(path)
        result[name] = digest(path)
    encoded = json.dumps(result, sort_keys=True, separators=(',', ':')).encode()
    return {'files': result, 'sha256': hashlib.sha256(encoded).hexdigest()}


def logged(argv, log_path, timeout, check=True):
    """Own one bounded process group and preserve its complete command output."""
    with log_path.open('ab') as log:
        log.write((json.dumps({'argv': [str(arg) for arg in argv]}) + '\n').encode())
        log.flush()
        process = subprocess.Popen(argv, cwd=REPO, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
        except BaseException:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=10)
            raise
        log.write((json.dumps({'exit_code': code}) + '\n').encode())
    if check and code != 0:
        raise RuntimeError(f'command exited {code}; see {log_path.name}')
    return code


def ssh_command(host, argv):
    # Every remote shell argument is quoted once; local execution never uses a shell.
    return ['ssh', *SSH_OPTIONS, host, shlex.join([str(arg) for arg in argv])]


def capture(argv, timeout=30):
    result = subprocess.run(argv, cwd=REPO, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f'{argv[0]} failed ({result.returncode}): {result.stderr.strip()}')
    return result.stdout


def gpt_partitions(image):
    """Decode bounded, CRC-checked GPT entries from the builder's 512-byte image."""
    image_bytes = image.stat().st_size
    with image.open('rb') as stream:
        protective = stream.read(512)
        header = bytearray(stream.read(512))
        if len(header) != 512 or protective[510:512] != b'\x55\xaa':
            raise ValueError('test image lacks a valid sector-zero signature')
        if header[:8] != b'EFI PART':
            raise ValueError('remote acceptance requires the generated GPT disk image')
        header_size, header_crc = struct.unpack_from('<II', header, 12)
        if not 92 <= header_size <= 512:
            raise ValueError('GPT header size is outside one sector')
        struct.pack_into('<I', header, 16, 0)
        if zlib.crc32(header[:header_size]) != header_crc:
            raise ValueError('GPT header checksum mismatch')
        table_lba, count, entry_bytes, table_crc = struct.unpack_from('<QIII', header, 72)
        table_bytes = count * entry_bytes
        if not 1 <= count <= 4096 or not 128 <= entry_bytes <= 4096:
            raise ValueError('GPT entry geometry exceeds bounded parser limits')
        if table_bytes > 16 * 1024 * 1024 or table_lba * 512 + table_bytes > image_bytes:
            raise ValueError('GPT entry array is outside the supplied image')
        stream.seek(table_lba * 512)
        entries = stream.read(table_bytes)
        if len(entries) != table_bytes or zlib.crc32(entries) != table_crc:
            raise ValueError('GPT entry array checksum mismatch')
    result = []
    for index in range(count):
        entry = entries[index * entry_bytes:(index + 1) * entry_bytes]
        if entry[:16] == bytes(16):
            continue
        first, last = struct.unpack_from('<QQ', entry, 32)
        if first > last or first < 2 or (last + 1) * 512 > image_bytes:
            raise ValueError(f'GPT partition {index + 1} exceeds image bounds')
        result.append({'partition': index + 1, 'first_lba': first,
                       'last_lba': last, 'offset_bytes': first * 512})
    if not result:
        raise ValueError('GPT image has no usable partitions')
    return result


def payload_partition(image, log_path):
    """Select the FAT containing vmunix, never the first ESP by position alone."""
    matches = []
    for partition in gpt_partitions(image):
        source = f'{image}@@{partition["offset_bytes"]}'
        code = logged(['mdir', '-i', source, '::vmunix'], log_path, 30, check=False)
        if code == 0:
            matches.append(partition)
    if len(matches) != 1:
        raise ValueError(f'expected one FAT partition containing vmunix, found {len(matches)}')
    return matches[0]


def configure_init(data, init_path):
    lines = data.splitlines(keepends=True)
    matches = [index for index, line in enumerate(lines)
               if re.match(rb'^[ \t]*init[ \t]*=', line)]
    if len(matches) > 1:
        raise ValueError('boot configuration contains ambiguous duplicate init entries')
    replacement = f'init={init_path}\n'.encode('ascii')
    if matches:
        lines[matches[0]] = replacement
    else:
        if lines and not lines[-1].endswith(b'\n'):
            lines[-1] += b'\n'
        lines.append(replacement)
    return b''.join(lines)


def console_range(kernel):
    """Translate the complete vt_history ELF symbol into its load segment's GPA."""
    compiler_nm = REPO / 'build/llvm/bin/llvm-nm'
    nm = str(compiler_nm) if compiler_nm.is_file() else 'nm'
    symbols = capture([nm, '-S', '--defined-only', str(kernel)])
    matches = []
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[3] == 'vt_history':
            matches.append((int(fields[0], 16), int(fields[1], 16)))
    if len(matches) != 1:
        raise ValueError('matching kernel must define exactly one sized vt_history symbol')
    address, size = matches[0]
    if not 0 < size <= 1048576:
        raise ValueError('vt_history symbol size exceeds bounded console capture')
    with kernel.open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:6] != b'\x7fELF\x02\x01':
            raise ValueError('console translation requires the little-endian ELF64 kernel')
        if struct.unpack_from('<H', header, 18)[0] != 62:
            raise ValueError('console translation requires the amd64 kernel')
        phoff = struct.unpack_from('<Q', header, 32)[0]
        entry_bytes, count = struct.unpack_from('<HH', header, 54)
        if entry_bytes < 56 or not 1 <= count <= 1024:
            raise ValueError('ELF program-header geometry is unsupported')
        if phoff + count * entry_bytes > kernel.stat().st_size:
            raise ValueError('ELF program-header array is outside the kernel')
        loads = []
        for index in range(count):
            stream.seek(phoff + index * entry_bytes)
            entry = stream.read(56)
            kind, flags, offset, virtual, physical, filesz, memsz, align = struct.unpack('<IIQQQQQQ', entry)
            if kind == 1 and virtual <= address and address + size <= virtual + memsz:
                loads.append(physical + address - virtual)
    if len(loads) != 1 or loads[0] + size > 1024 * 1024 * 1024:
        raise ValueError('vt_history is not wholly inside one load segment and the 1 GiB guest RAM')
    return {'symbol': 'vt_history', 'virtual_address': address,
            'physical_address': loads[0], 'bytes': size}


def prepare_payload(args, output, report):
    build = args.build_directory.resolve()
    config = args.config.resolve()
    profile = selected_profile(args)
    before = snapshot_sources(config, getattr(args, 'profile', 'venus'))
    report['git_commit'] = capture(['git', 'rev-parse', 'HEAD']).strip()
    report['sources_before_build'] = before
    report['build'] = {'skipped': args.skip_build, 'directory': str(build),
                       'config': str(config)}
    write_json(output / 'result.json', report)
    if args.skip_build:
        (output / 'build.log').write_text('Build explicitly skipped; existing artifacts reused.\n')
    else:
        command = ['make', '-j16', f'BUILD={build}', f'ZEDBSD_CONFIG={config}',
                   f'ARCH_IMAGE_DIR={build / "arch-images"}',
                   f'DATA_IMAGE={build / "data.img"}',
                   f'SWAP_IMAGE={build / "swapfile"}', 'disk-image']
        report['build']['argv'] = command
        logged(command, output / 'build.log', args.build_timeout)
    after = snapshot_sources(config, getattr(args, 'profile', 'venus'))
    report['sources_after_build'] = after
    if before != after:
        raise RuntimeError('source files changed during this build/reuse snapshot; start a fresh attempt')
    kernel = build / 'vmunix'
    application = build / 'bin' / profile['application']
    image = args.image.resolve() if args.image else build / 'hdd-image.img'
    harness = REPO / profile.get('harness_dir', 'plan/ws014/tests') / profile['harness']
    artifact_paths = {'kernel': kernel, 'application': application,
                      'base_image': image, 'harness': harness}
    artifact_paths.update({role: build / name for role, name in profile.get('additional_artifacts', {}).items()})
    artifact_paths.update({role: REPO / 'plan/ws014/tests' / name
                           for role, name in profile['modules'].items()})
    report['artifacts'] = {name: {'path': str(path), 'sha256': digest(path),
                                 'bytes': path.stat().st_size}
                           for name, path in artifact_paths.items()}
    payload = output / 'payload'
    payload.mkdir()
    disposable = payload / 'boot.img'
    logged(['cp', '--reflink=auto', '--sparse=always', str(image), str(disposable)],
           output / 'prepare.log', 600)
    if digest(disposable) != report['artifacts']['base_image']['sha256']:
        raise RuntimeError('base image changed while making the disposable copy')
    partition = payload_partition(disposable, output / 'prepare.log')
    source = f'{disposable}@@{partition["offset_bytes"]}'
    extracted_kernel = payload / 'embedded-vmunix'
    logged(['mcopy', '-i', source, '::vmunix', str(extracted_kernel)],
           output / 'prepare.log', 60)
    embedded_hash = digest(extracted_kernel)
    if embedded_hash != report['artifacts']['kernel']['sha256']:
        raise RuntimeError('payload FAT vmunix differs from the selected build kernel')
    extracted_kernel.unlink()
    original = payload / 'original-zedbsd.cfg'
    modified = payload / 'zedbsd.cfg'
    verified = payload / 'verified-zedbsd.cfg'
    logged(['mcopy', '-i', source, '::zedbsd.cfg', str(original)], output / 'prepare.log', 30)
    modified.write_bytes(configure_init(original.read_bytes(), args.init))
    logged(['mcopy', '-o', '-i', source, str(modified), '::zedbsd.cfg'], output / 'prepare.log', 30)
    logged(['mcopy', '-i', source, '::zedbsd.cfg', str(verified)], output / 'prepare.log', 30)
    if verified.read_bytes() != modified.read_bytes():
        raise RuntimeError('disposable boot configuration failed read-back verification')
    if digest(image) != report['artifacts']['base_image']['sha256']:
        raise RuntimeError('base image changed during disposable-image preparation')
    for role, path in [('harness', harness), *[(role, artifact_paths[role])
                        for role in profile['modules']]]:
        shutil.copyfile(path, payload / path.name)
        if digest(payload / path.name) != report['artifacts'][role]['sha256']:
            raise RuntimeError(f'{role} changed while preparing its exact copy')
    shutil.copyfile(kernel, payload / 'vmunix')
    if digest(payload / 'vmunix') != embedded_hash:
        raise RuntimeError('selected kernel changed while preparing its exact copy')
    report['console'] = console_range(payload / 'vmunix')
    report['payload_partition'] = partition
    report['disposable_image'] = {'path': str(disposable), 'sha256': digest(disposable),
                                  'embedded_kernel_sha256': embedded_hash,
                                  'init': args.init,
                                  'original_config_sha256': digest(original),
                                  'modified_config_sha256': digest(modified)}
    write_json(payload / 'manifest.json', report)
    return payload


def fetch_evidence(args, remote_attempt, output, report):
    """Fetch only named regular evidence files; guest disks and sockets stay remote."""
    remote_capture = remote_attempt / 'capture'
    evidence_files = selected_profile(args)['evidence']
    script = ('import json,pathlib,stat,sys; root=pathlib.Path(sys.argv[1]); '
              'names=json.loads(sys.argv[2]); result=[]; '
              '\nfor name in names:\n p=root/name\n'
              ' try:\n  info=p.lstat()\n except FileNotFoundError:\n  continue\n'
              ' if stat.S_ISREG(info.st_mode):\n  result.append(name)\n'
              'print(json.dumps(result))')
    result = capture(ssh_command(args.host, ['python3', '-c', script,
                     str(remote_capture), json.dumps(evidence_files)]), timeout=45)
    names = json.loads(result)
    if not isinstance(names, list) or any(name not in evidence_files for name in names):
        raise RuntimeError('remote evidence listing contains an unexpected filename')
    evidence = output / 'evidence'
    evidence.mkdir(exist_ok=True)
    fetched = {}
    failures = []
    for name in names:
        try:
            logged(['scp', *SSH_OPTIONS, f'{args.host}:{remote_capture / name}', str(evidence / name)],
                   output / 'transfer.log', 180)
            fetched[name] = digest(evidence / name)
        except Exception as error:
            failures.append({'file': name, 'error': str(error)})
    report['fetched_evidence'] = fetched
    if failures:
        report['fetch_failures'] = failures
        raise RuntimeError('one or more evidence files could not be fetched')


def verify_remote_result(args, report, remote):
    """Accept only this attempt's successful capture and exact retrieved artifacts."""
    expected_status = 'boot-pass' if args.boot_only else 'pass'
    if report.get('remote_exit_code') != 0 or remote.get('status') != expected_status:
        raise RuntimeError('remote attempt did not complete successfully')
    if remote.get('qemu_exit_code') != 0:
        raise RuntimeError('remote QEMU did not exit cleanly')
    if args.renderer_library_dir is not None:
        library = remote.get('renderer_library', {})
        if (library.get('directory') != args.renderer_library_dir.rstrip('/') or
                library.get('mapped_by_qemu') is not True or
                library.get('final_sha256') != library.get('sha256') or
                re.fullmatch(r'[a-f0-9]{64}', library.get('sha256', '')) is None):
            raise RuntimeError('remote QEMU did not prove the selected isolated renderer identity')
    if remote.get('phase') != args.phase or remote.get('frame') != args.frame:
        raise RuntimeError('remote result belongs to a different phase or frame')
    if (remote.get('console_address') != report['console']['physical_address'] or
            remote.get('console_size') != report['console']['bytes']):
        raise RuntimeError('remote console capture differs from the matching kernel symbols')
    image_hash = report['disposable_image']['sha256']
    if remote.get('image_sha256') != image_hash or remote.get('source_image_sha256') != image_hash:
        raise RuntimeError('remote QEMU image differs from the exact disposable artifact')
    if remote.get('environment', {}).get('RENDER_SERVER_EXEC_PATH') != args.render_server:
        raise RuntimeError('remote capture used a different render server path')
    if not re.fullmatch(r'[0-9a-f]{64}', remote.get('render_server_sha256', '')):
        raise RuntimeError('remote capture omitted the render server binary hash')
    if remote.get('harness_sha256') != report['artifacts']['harness']['sha256']:
        raise RuntimeError('remote capture used a different harness revision')
    if remote.get('rfb_client_sha256') != report['artifacts']['rfb_client']['sha256']:
        raise RuntimeError('remote capture used a different RFB client revision')
    fetched = report.get('fetched_evidence', {})
    required = ['result.json', 'guest.log', 'qemu-renderer.log', 'qmp.jsonl',
                'console.log', 'console.bin']
    if remote.get('boot_surface_available') is True:
        required.append('boot.ppm')
    elif remote.get('boot_surface_available') is not False:
        raise RuntimeError('remote result does not record firmware surface availability')
    if not args.boot_only and not args.fault_test:
        if args.profile in ('vkdemo', 'wayland'):
            required += [f'frame-{i}.ppm' for i in range(1, 7)]
            required += [f'oracle-{i}.json' for i in range(1, 7)]
        elif args.profile == 'mview':
            required += ['initial.ppm', 'rotate.ppm', 'pan.ppm', 'zoom.ppm', 'keys.ppm', 'reset.ppm']
        else:
            required.append('frame.ppm')
    if any(name not in fetched for name in required):
        raise RuntimeError('successful remote attempt is missing required evidence files')
    if (fetched['guest.log'] != remote.get('guest_log_sha256') or
            fetched['qemu-renderer.log'] != remote.get('renderer_log_sha256')):
        raise RuntimeError('retrieved logs differ from the completed remote capture')
    if args.fault_test:
        if remote.get('transport_harness_sha256') != report['artifacts']['transport_harness']['sha256']:
            raise RuntimeError('isolated fault run used a different bounded QEMU harness')
        if (remote.get('token') != args.token or remote.get('fault_test') != args.fault_test or
                remote.get('guest_completed') is not True):
            raise RuntimeError('isolated fault result differs from the requested scenario')
        observed = (args.output_root.resolve() / args.attempt / 'evidence/wayland-observed.log').read_text()
        expected = {
            'completion-delay': 'GPUFENCE COMPLETION_DELAY PASS',
            'context-timeout': 'GPUFENCE CONTEXT_TIMEOUT PASS',
            'submit-load': 'GPUFENCE SUBMIT_LOAD PASS processes=2 rounds=3 submits=576 verified_bytes=25165824 verified_submits=576',
            'recovery': 'GPURECOVERY PASS timeout=1 peer_failed=1 retirement_gate=1 fresh_roundtrip=4096 decoder=1',
            'producer-exit': 'GPUFENCE PRODUCER_EXIT_REAL_RESULT PASS',
            'producer-stop': 'GPUFENCE PRODUCER_STOP_ERROR PASS',
            'producer-exit-delayed': 'GPUFENCE PRODUCER_EXIT_DELAYED PASS',
            'producer-exit-hang': 'GPUFENCE PRODUCER_EXIT_HANG PASS',
        }[args.fault_test]
        if remote.get('fault_marker') != expected or expected not in observed:
            raise RuntimeError('isolated fault evidence lacks successful guest assertions')
        if args.fault_test in ('submit-load', 'completion-delay', 'context-timeout', 'producer-stop', 'producer-exit-delayed'):
            load_spec = importlib.util.spec_from_file_location(
                'submit_load_harness', REPO / 'plan/ws014/tests/wayland-qemu.py')
            load_harness = importlib.util.module_from_spec(load_spec)
            load_spec.loader.exec_module(load_harness)
            if args.fault_test == 'submit-load':
                if remote.get('load_samples') != load_harness.verify_submit_load(observed):
                    raise RuntimeError('submit load report differs from the captured per-round measurements')
                cpu = remote.get('host_cpu_samples', {})
                if (not isinstance(cpu.get('ticks_per_second'), int) or cpu['ticks_per_second'] <= 0 or
                        {item.get('kind') for item in cpu.get('processes', [])} != {'qemu', 'renderer'}):
                    raise RuntimeError('submit load lacks separate QEMU and renderer CPU observations')
            else:
                renderer_text = (args.output_root.resolve() / args.attempt / 'evidence/qemu-renderer.log').read_text(errors='replace')
                delay = load_harness.verify_completion_delay(observed, renderer_text, args.fault_test)
                if remote.get('completion_delay') != delay or remote.get('test_renderer_delay') is not True:
                    raise RuntimeError('completion-delay report differs from actual guest and renderer observations')
                if remote.get('delay_renderer') != {key: delay[key] for key in ('pid', 'context', 'fence')}:
                    raise RuntimeError('delayed callback did not retain the verified owned renderer identity')
        if args.fault_test == 'recovery' and not 9000 <= remote.get('watchdog_elapsed_ms', 0) <= 20000:
            raise RuntimeError('isolated recovery did not measure the expected watchdog interval')
        if args.fault_test == 'producer-stop':
            if (remote.get('producer_stopped') is not True or
                    'GPUFENCE PRODUCER_STOPPED pending=1 fd_live=1' not in observed or
                    not 7000 <= remote.get('watchdog_elapsed_ms', 0) <= 13000):
                raise RuntimeError('producer-stop lacks stopped-process proof and autonomous terminal timing')
        if args.fault_test == 'producer-exit':
            outcome = remote.get('producer_exit', {})
            if outcome.get('result') not in (0, -4) or not 1000 <= outcome.get('elapsed_ms', 0) <= 75000:
                raise RuntimeError('producer-exit lacks a recorded real job result within the deadline budget')
        if args.fault_test == 'producer-exit-hang':
            if (remote.get('isolation_peer') is not True or
                    not 7000 <= remote.get('watchdog_elapsed_ms', 0) <= 13000 or
                    not isinstance(remote.get('reclaim'), dict)):
                raise RuntimeError('producer-exit-hang lacks isolation proof, deadline timing and a recorded reclaim run')
        if args.fault_test == 'recovery':
            pause = remote.get('renderer_pause', {})
            if (pause.get('stopped') is not True or pause.get('resumed') is not True or
                    len(pause.get('pids', [])) < 3 or pause.get('qemu_pid') in pause.get('pids', [])):
                raise RuntimeError('isolated recovery lacks bounded renderer suspension and restoration')
    elif args.profile == 'wayland':
        verify_wayland_result(args, report, remote)
    elif args.profile == 'vkdemo':
        verify_vkdemo_result(args, report, remote)
    elif args.profile == 'mview':
        verify_mview_result(args, report, remote)
    elif not args.boot_only:
        if remote.get('expected') != [args.left, args.right]:
            raise RuntimeError('remote frame used different independent RGB expectations')
        if fetched['frame.ppm'] != remote.get('frame_sha256'):
            raise RuntimeError('retrieved frame differs from the validated remote pixels')
    return expected_status


def verify_mview_result(args, report, remote):
    """Require the viewer's images and the harness's view checks (WS031 p013)."""
    fetched = report.get('fetched_evidence', {})
    for name in ('initial.ppm', 'rotate.ppm', 'pan.ppm', 'zoom.ppm', 'keys.ppm', 'reset.ppm'):
        if fetched.get(name) != remote.get('images', {}).get(name):
            raise RuntimeError(f'retrieved {name} differs from the remote capture')
    checks = remote.get('checks', {})
    if not checks or not all(value is True for value in checks.values()):
        raise RuntimeError(f'viewer checks failed: {checks}')
    if remote.get('token') != args.token:
        raise RuntimeError('remote result belongs to a different token')


def verify_wayland_result(args, report, remote):
    """Verify every fetched fullscreen sample against the independent rectangle specification."""
    if remote.get('token') != args.token or remote.get('guest_completed') is not True:
        raise RuntimeError('Wayland process completion does not match this exact attempt')
    if remote.get('transport_harness_sha256') != report['artifacts']['transport_harness']['sha256']:
        raise RuntimeError('Wayland run used a different bounded QEMU harness')
    if remote.get('oracle_sha256') != report['artifacts']['oracle']['sha256']:
        raise RuntimeError('Wayland run used a different independent oracle')
    samples = remote.get('samples', [])
    if len(samples) != 12 or {sample['mode'] for sample in samples} != {'fifo', 'mailbox'}:
        raise RuntimeError('both Wayland presentation modes require six real captured frames')
    spec = importlib.util.spec_from_file_location('wayland_oracle', REPO / 'plan/ws014/tests/wayland_oracle.py')
    oracle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(oracle)
    evidence = args.output_root.resolve() / args.attempt / 'evidence'
    for number, sample in enumerate(samples, 1):
        filename = f'frame-{number}.ppm'
        if report['fetched_evidence'].get(filename) != sample['frame_sha256']:
            raise RuntimeError('Wayland capture hash differs after transfer')
        checked = oracle.verify(evidence / filename, sample['frame'], sample['width'], sample['height'])
        if not checked['passed'] or checked != sample['oracle']:
            raise RuntimeError('Wayland capture disagrees with independent host expectation')
    if any(remote.get(key) is not True for key in ('shared_gpu_path', 'compositor_stopped',
                                                  'independent_renderer_import',
                                                  'concurrent_admission', 'external_fence')):
        raise RuntimeError('Wayland acceptance lacks shared allocation or compositor lifecycle evidence')
    imports = remote.get('allocation_imports', [])
    if len(imports) != 2 or {item.get('kind') for item in imports} != {'buffer', 'optimal'}:
        raise RuntimeError('Wayland acceptance lacks standard buffer and optimal allocation imports')
    notifications = remote.get('display_notifications', {})
    if (notifications.get('sequence', 0) < 1 or notifications.get('outputs') != 1 or
            not 1 <= notifications.get('attempts', 0) <= 8):
        raise RuntimeError('Wayland acceptance lacks initial display inventory poll/QUERY/ACK evidence')
    if args.lifecycle:
        lifecycle = remote.get('lifecycle', {})
        required = ('client_aborted', 'client_reopened', 'compositor_reopened',
                    'compositor_killed', 'surface_loss', 'after_kill_reopened',
                    'killed_console_restored', 'console_restored')
        if any(lifecycle.get(key) is not True for key in required):
            raise RuntimeError('Wayland acceptance lacks completed process/surface/console recovery')
        for name, key in [('console-return.ppm', 'console_before_sha256'),
                          ('console-killed.ppm', 'killed_console_sha256'),
                          ('console-write.ppm', 'console_after_sha256')]:
            if report['fetched_evidence'].get(name) != lifecycle.get(key):
                raise RuntimeError('Wayland console evidence hash differs after transfer')
        before = oracle.read_ppm(evidence / 'console-return.ppm')
        after = oracle.read_ppm(evidence / 'console-write.ppm')
        if oracle.read_ppm(evidence / 'console-killed.ppm')[:2] != (640, 480):
            raise RuntimeError('fetched SIGKILL capture does not show the native console')
        if before[:2] != (640, 480) or after[:2] != (640, 480) or before[2] == after[2]:
            raise RuntimeError('fetched Wayland console captures do not prove visible recovery')


def verify_vkdemo_result(args, report, remote):
    """Recheck fetched RGB with the same independently specified host geometry."""
    if remote.get('token') != args.token or remote.get('guest_completed') is not True:
        raise RuntimeError('remote vkdemo did not finish this exact run token')
    ordinary = remote.get('ordinary', {})
    ordinary_samples = ordinary.get('samples', [])
    if (ordinary.get('token') != args.token + '-ordinary' or
            ordinary.get('completed') is not True or ordinary.get('start_seen') is not True or
            len(ordinary_samples) < 2 or len(ordinary_samples) > 4096 or
            ordinary.get('frames') != ordinary_samples[-1]['frame'] or
            any(b['frame'] <= a['frame'] or b['time_ms'] <= a['time_ms']
                for a, b in zip(ordinary_samples, ordinary_samples[1:])) or
            any(sample.get('readback_disabled') is not True for sample in ordinary_samples)):
        raise RuntimeError('ordinary vkdemo run did not prove progress and clean reopening/exit')
    if getattr(args, 'lifecycle', False):
        lifecycle = remote.get('lifecycle', {})
        if any(lifecycle.get(key) is not True for key in ('abnormal_exit', 'reopened', 'console_restored')):
            raise RuntimeError('requested abnormal-exit and console restoration acceptance is incomplete')
        competition = remote.get('competition', {})
        if (competition.get('owner') != args.token + '-owner' or
                competition.get('contender') != args.token + '-contender' or
                competition.get('rejected') is not True or
                competition.get('owner_completed') is not True or
                competition.get('owner_frames', 0) < 3 or
                competition.get('error') != 'VK_ERROR_NATIVE_WINDOW_IN_USE_KHR'):
            raise RuntimeError('requested independent process display ownership check is incomplete')
        for stage, name in [('before', 'console-return.ppm'), ('after', 'console-write.ppm')]:
            if report['fetched_evidence'].get(name) != lifecycle.get('console_' + stage + '_sha256'):
                raise RuntimeError('returned console capture hash differs from the remote evidence')
    for role in ('transport_harness', 'oracle'):
        if remote.get(role + '_sha256') != report['artifacts'][role]['sha256']:
            raise RuntimeError(f'remote {role} differs from the uploaded version')
    spec = importlib.util.spec_from_file_location('vkdemo_oracle',
               REPO / 'plan/ws014/tests/vkdemo_oracle.py')
    oracle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(oracle)
    samples = remote.get('samples', [])
    oracle.verify_sequence(samples)
    evidence = args.output_root.resolve() / args.attempt / 'evidence'
    captures = ordinary.get('captures', [])
    if len(captures) != 2:
        raise RuntimeError('ordinary no-readback run needs two independent display captures')
    captured_pixels = []
    for number, capture in enumerate(captures, 1):
        name = f'ordinary-{number}.ppm'
        if (capture.get('filename') != name or
                report['fetched_evidence'].get(name) != capture.get('sha256')):
            raise RuntimeError('ordinary display capture identity or hash differs after transfer')
        captured_pixels.append(oracle.read_ppm(evidence / name))
    if captured_pixels[0] == captured_pixels[1]:
        raise RuntimeError('ordinary no-readback captures do not show visible motion')
    transport_spec = importlib.util.spec_from_file_location(
        'venus_trace_verifier', REPO / 'plan/ws014/tests/venus-qemu.py')
    transport = importlib.util.module_from_spec(transport_spec)
    transport_spec.loader.exec_module(transport)
    scanout = transport.verify_blob_scanout_trace((evidence / 'qemu-renderer.log').read_bytes(), 320, 240)
    if scanout != remote.get('scanout_trace'):
        raise RuntimeError('fetched direct scanout selection differs from the completed trace evidence')
    for sample in samples:
        if sample['run'] != args.token:
            raise RuntimeError('remote sample carries a different run token')
        name = f'frame-{sample["frame"]}.ppm'
        if sample.get('filename') != name or report['fetched_evidence'][name] != sample.get('frame_sha256'):
            raise RuntimeError('fetched vkdemo frame differs from validated remote pixels')
        pixels = oracle.read_ppm(evidence / name)
        if hashlib.sha256(pixels).hexdigest() != sample['rgb_sha256']:
            raise RuntimeError('VNC pixels differ from the GPU readback SHA256')
        checked = oracle.verify_pixels(pixels, sample['time_ms'])
        if not checked['passed']:
            raise RuntimeError('fetched vkdemo pixels fail the independent cuboid/texture oracle')
        oracle_name = f'oracle-{sample["frame"]}.json'
        if report['fetched_evidence'][oracle_name] != sample.get('oracle_result_sha256'):
            raise RuntimeError('fetched oracle diagnostics differ from the completed capture')
        stored = json.loads((evidence / oracle_name).read_text())
        if stored != checked or sample.get('oracle') != checked:
            raise RuntimeError('saved and independently recomputed oracle results differ')


def run(args):
    profile = selected_profile(args)
    output = args.output_root.resolve() / args.attempt
    output.mkdir(parents=True, exist_ok=False)
    remote_attempt = PurePosixPath(args.remote_root) / args.attempt
    report = {'attempt': args.attempt, 'status': 'running',
              'started': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'host': args.host, 'remote_attempt': str(remote_attempt),
              'phase': args.phase, 'frame': args.frame, 'render_server': args.render_server,
              'expected': {'left': args.left, 'right': args.right},
              'wrapper_sha256': digest(__file__), 'remote_created': False}
    if args.profile in TOKEN_PROFILES:
        wrapper_dir = profile.get('harness_dir', 'plan/ws014/tests')
        report.update(profile=args.profile, token=args.token,
                      profile_wrapper_sha256=digest(REPO / wrapper_dir / f'run-{args.profile}-remote.py'))
    started = time.monotonic()
    write_json(output / 'result.json', report)
    try:
        payload = prepare_payload(args, output, report)
        script = ('import pathlib,sys; '
                  'pathlib.Path(sys.argv[1]).mkdir(parents=True,exist_ok=True); '
                  'pathlib.Path(sys.argv[2]).mkdir()')
        logged(ssh_command(args.host, ['python3', '-c', script, args.remote_root,
                                      str(remote_attempt)]), output / 'transfer.log', 45)
        report['remote_created'] = True
        write_json(output / 'result.json', report)
        for name in ['boot.img', profile['harness'], *profile['modules'].values(),
                     'vmunix', 'manifest.json']:
            logged(['scp', *SSH_OPTIONS, str(payload / name), f'{args.host}:{remote_attempt / name}'],
                   output / 'transfer.log', args.transfer_timeout)
        # Venus renders on the host GPU: the iGPU leaves vfio-pci (the i915 passthrough tests) for the
        # host i915 driver for this attempt and goes back afterwards (bigbang/igpu-mode.sh on the host).
        logged(ssh_command(args.host, ['bigbang/igpu-mode.sh', 'host']), output / 'transfer.log', 60)
        report['igpu_mode'] = 'host'
        remote_limit = 2 * args.timeout + 180
        command = ['timeout', '--signal=TERM', '--kill-after=15s', str(remote_limit),
                   'python3', str(remote_attempt / profile['harness']),
                   '--image', str(remote_attempt / 'boot.img'),
                   '--output', str(remote_attempt / 'capture'), '--timeout', str(args.timeout),
                   '--console-address', str(report['console']['physical_address']),
                   '--console-size', str(report['console']['bytes']),
                   '--render-server', args.render_server]
        if args.renderer_library_dir is not None:
            command += ['--renderer-library-dir', args.renderer_library_dir]
        if args.fault_test is not None:
            command += ['--fault-test', args.fault_test]
        if args.profile in TOKEN_PROFILES:
            command += ['--token', args.token]
            if args.lifecycle:
                command.append('--lifecycle')
        else:
            command += ['--phase', args.phase, '--frame', str(args.frame)]
        if args.boot_only:
            command.append('--boot-only')
        elif args.profile not in TOKEN_PROFILES:
            command += ['--left', ','.join(map(str, args.left)),
                        '--right', ','.join(map(str, args.right))]
        report['remote_argv'] = command
        report['remote_watchdog_seconds'] = remote_limit
        write_json(output / 'result.json', report)
        report['remote_exit_code'] = logged(ssh_command(args.host, command),
                                            output / 'remote.log', remote_limit + 45,
                                            check=False)
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='fail', error=f'{type(error).__name__}: {error}')
    finally:
        if report.get('igpu_mode') == 'host':
            try:
                logged(ssh_command(args.host, ['bigbang/igpu-mode.sh', 'vfio']), output / 'transfer.log', 60)
                report['igpu_mode'] = 'vfio'
            except Exception as error:
                report.update(status='fail', igpu_error=str(error))
        if report['remote_created']:
            try:
                fetch_evidence(args, remote_attempt, output, report)
            except Exception as error:
                report.update(status='fail', evidence_error=str(error))
        try:
            remote = json.loads((output / 'evidence/result.json').read_text())
            report['remote_result'] = remote
            if report['status'] == 'running':
                report['status'] = verify_remote_result(args, report, remote)
        except Exception as error:
            if report['status'] == 'running':
                report.update(status='fail', error=str(error))
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        write_json(output / 'result.json', report)
    print(json.dumps({'status': report['status'], 'attempt': args.attempt,
                      'result': str(output / 'result.json'), 'error': report.get('error'),
                      'evidence_error': report.get('evidence_error')}, indent=2))
    return 0 if report['status'] in ('pass', 'boot-pass') else 1


def rgb(text):
    try:
        values = [int(part) for part in text.split(',')]
    except ValueError as error:
        raise argparse.ArgumentTypeError('RGB must be three integer bytes') from error
    if len(values) != 3 or any(value < 0 or value > 255 for value in values):
        raise argparse.ArgumentTypeError('RGB must be three integer bytes')
    return values


def main(profile='venus'):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--attempt', required=True)
    parser.add_argument('--host', default='awe@10.0.10.25')
    parser.add_argument('--remote-root', default='/home/awe/zedbsd-q306-venus')
    parser.add_argument('--render-server', help='remote virgl_render_server; defaults to the isolated dependency')
    parser.add_argument('--renderer-library-dir', help='isolated remote libvirglrenderer directory for a paired renderer build')
    parser.add_argument('--output-root', type=Path, default=REPO / 'plan/ws014/temp/remote')
    parser.add_argument('--build-directory', type=Path, default=REPO / f'build/{profile}-amd64')
    parser.add_argument('--config', type=Path,
                        default=REPO / PROFILES[profile].get('config', f'plan/ws014/tests/config-{profile}-amd64.mk'))
    parser.add_argument('--image', type=Path, help='explicit base image; defaults to the selected build directory')
    parser.add_argument('--init', default='/bin/sh', help='init path written only into the disposable image')
    parser.add_argument('--skip-build', action='store_true', help='explicitly reuse and record existing artifacts')
    parser.add_argument('--lifecycle', action='store_true', help='also verify SIGINT cleanup and visible console restoration')
    parser.add_argument('--fault-test', choices=['recovery', 'producer-exit', 'producer-stop', 'submit-load', 'completion-delay', 'context-timeout', 'producer-exit-delayed', 'producer-exit-hang'],
                        help='run one isolated fault or submit-load regression instead of the Wayland suite')
    parser.add_argument('--phase', choices=['2d', 'venus'] if profile == 'venus' else [profile],
                        default=profile)
    parser.add_argument('--frame', type=int, default=1 if profile == 'venus' else 0)
    parser.add_argument('--left', type=rgb)
    parser.add_argument('--right', type=rgb)
    parser.add_argument('--boot-only', action='store_true')
    parser.add_argument('--timeout', type=int, default=120 if profile == 'venus' else 180)
    parser.add_argument('--build-timeout', type=int, default=1800)
    parser.add_argument('--transfer-timeout', type=int, default=600)
    args = parser.parse_args()
    args.profile = profile
    if args.fault_test and (profile != 'wayland' or args.lifecycle):
        parser.error('fault-test requires the Wayland image and an isolated VM without lifecycle')
    if args.lifecycle and profile not in ('vkdemo', 'wayland'):
        parser.error('lifecycle verification requires the standard vkdemo profile')
    args.token = 'r' + hashlib.sha256(args.attempt.encode()).hexdigest()[:24]
    if not re.fullmatch(r'[a-z0-9][a-z0-9_.-]{0,63}', args.attempt):
        parser.error('attempt must be one lowercase identifier, at most 64 characters')
    if not re.fullmatch(r'(?:[a-z_][a-z0-9_-]*@)?[a-zA-Z0-9][a-zA-Z0-9.-]*', args.host):
        parser.error('host must be one plain SSH hostname or user@hostname')
    remote = PurePosixPath(args.remote_root)
    if (not re.fullmatch(r'/home/[a-zA-Z0-9_./-]+', args.remote_root) or
            '..' in remote.parts or len(remote.parts) < 4):
        parser.error('remote-root must be a task directory below a user home, without parent traversal')
    if args.render_server is None:
        args.render_server = str(remote / 'dependencies/virgl-server-1.1.0-2/usr/libexec/virgl_render_server')
    if (not re.fullmatch(r'/[a-zA-Z0-9_./-]+', args.render_server) or
            '..' in PurePosixPath(args.render_server).parts):
        parser.error('render-server must be one absolute remote executable path')
    if args.renderer_library_dir is not None:
        if (not re.fullmatch(r'/home/[a-zA-Z0-9_./-]+', args.renderer_library_dir) or
                '..' in PurePosixPath(args.renderer_library_dir).parts):
            parser.error('renderer-library-dir must be an isolated absolute path below a user home')
    if len(str(remote / args.attempt / 'capture/qmp.sock').encode()) >= 104:
        parser.error('remote attempt path is too long for a Unix QMP socket')
    if not re.fullmatch(r'/[a-zA-Z0-9_./-]+', args.init) or '..' in PurePosixPath(args.init).parts:
        parser.error('init must be one absolute executable path without whitespace or parent traversal')
    if not 0 <= args.frame <= 1000000 or not 1 <= args.timeout <= 600:
        parser.error('frame must be 0..1000000 and timeout 1..600 seconds')
    if not 1 <= args.build_timeout <= 7200 or not 1 <= args.transfer_timeout <= 1800:
        parser.error('build timeout must be 1..7200 and transfer timeout 1..1800 seconds')
    if profile in TOKEN_PROFILES and (args.boot_only or args.frame != 0 or args.left is not None or args.right is not None):
        parser.error('vkdemo runs exactly six frames; boot-only/frame/left/right overrides are unsupported')
    if profile == 'venus' and not args.boot_only and (args.left is None or args.right is None):
        parser.error('frame acceptance requires independent --left and --right RGB expectations')
    return run(args)


if __name__ == '__main__':
    sys.exit(main())

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
SOURCE_FILES = ['Makefile', 'include/drivers/gpu.h', 'include/drivers/venus.h',
                'include/uapi/gpu.h', 'src/kern/platform/pcat.c', 'src/hal/amd64/asm.c',
                'platform/amd64/vmunix.mk', 'platform/amd64/zedbsd.cfg',
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
                           [f'oracle-{index}.json' for index in range(1, 7)],
               'source_directories': ['userland/base/vkdemo'],
               'source_files': ['userland/base/common/sha256.c', 'userland/base/common/sha256.h',
                                'plan/ws014/tests/vkdemo-qemu.py',
                                'plan/ws014/tests/vkdemo_oracle.py',
                                'plan/ws014/tests/run-vkdemo-remote.py']},
}


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
    harness = REPO / 'plan/ws014/tests' / profile['harness']
    artifact_paths = {'kernel': kernel, 'application': application,
                      'base_image': image, 'harness': harness}
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
    if not args.boot_only:
        if args.profile == 'vkdemo':
            required += [f'frame-{i}.ppm' for i in range(1, 7)]
            required += [f'oracle-{i}.json' for i in range(1, 7)]
        else:
            required.append('frame.ppm')
    if any(name not in fetched for name in required):
        raise RuntimeError('successful remote attempt is missing required evidence files')
    if (fetched['guest.log'] != remote.get('guest_log_sha256') or
            fetched['qemu-renderer.log'] != remote.get('renderer_log_sha256')):
        raise RuntimeError('retrieved logs differ from the completed remote capture')
    if args.profile == 'vkdemo':
        verify_vkdemo_result(args, report, remote)
    elif not args.boot_only:
        if remote.get('expected') != [args.left, args.right]:
            raise RuntimeError('remote frame used different independent RGB expectations')
        if fetched['frame.ppm'] != remote.get('frame_sha256'):
            raise RuntimeError('retrieved frame differs from the validated remote pixels')
    return expected_status


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
            len({sample['rgb_sha256'] for sample in ordinary_samples}) < 2):
        raise RuntimeError('ordinary vkdemo run did not prove progress and clean reopening/exit')
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
    if args.profile == 'vkdemo':
        report.update(profile=args.profile, token=args.token,
                      profile_wrapper_sha256=digest(REPO / 'plan/ws014/tests/run-vkdemo-remote.py'))
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
        remote_limit = 2 * args.timeout + 180
        command = ['timeout', '--signal=TERM', '--kill-after=15s', str(remote_limit),
                   'python3', str(remote_attempt / profile['harness']),
                   '--image', str(remote_attempt / 'boot.img'),
                   '--output', str(remote_attempt / 'capture'), '--timeout', str(args.timeout),
                   '--console-address', str(report['console']['physical_address']),
                   '--console-size', str(report['console']['bytes']),
                   '--render-server', args.render_server]
        if args.profile == 'vkdemo':
            command += ['--token', args.token]
        else:
            command += ['--phase', args.phase, '--frame', str(args.frame)]
        if args.boot_only:
            command.append('--boot-only')
        elif args.profile != 'vkdemo':
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
    parser.add_argument('--output-root', type=Path, default=REPO / 'plan/ws014/temp/remote')
    parser.add_argument('--build-directory', type=Path, default=REPO / f'build/{profile}-amd64')
    parser.add_argument('--config', type=Path, default=REPO / f'plan/ws014/tests/config-{profile}-amd64.mk')
    parser.add_argument('--image', type=Path, help='explicit base image; defaults to the selected build directory')
    parser.add_argument('--init', default='/bin/sh', help='init path written only into the disposable image')
    parser.add_argument('--skip-build', action='store_true', help='explicitly reuse and record existing artifacts')
    parser.add_argument('--phase', choices=['2d', 'venus'] if profile == 'venus' else ['vkdemo'],
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
    if len(str(remote / args.attempt / 'capture/qmp.sock').encode()) >= 104:
        parser.error('remote attempt path is too long for a Unix QMP socket')
    if not re.fullmatch(r'/[a-zA-Z0-9_./-]+', args.init) or '..' in PurePosixPath(args.init).parts:
        parser.error('init must be one absolute executable path without whitespace or parent traversal')
    if not 0 <= args.frame <= 1000000 or not 1 <= args.timeout <= 600:
        parser.error('frame must be 0..1000000 and timeout 1..600 seconds')
    if not 1 <= args.build_timeout <= 7200 or not 1 <= args.transfer_timeout <= 1800:
        parser.error('build timeout must be 1..7200 and transfer timeout 1..1800 seconds')
    if profile == 'vkdemo' and (args.boot_only or args.frame != 0 or args.left is not None or args.right is not None):
        parser.error('vkdemo runs exactly six frames; boot-only/frame/left/right overrides are unsupported')
    if profile == 'venus' and not args.boot_only and (args.left is None or args.right is None):
        parser.error('frame acceptance requires independent --left and --right RGB expectations')
    return run(args)


if __name__ == '__main__':
    sys.exit(main())

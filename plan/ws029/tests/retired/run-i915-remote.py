#!/usr/bin/env python3
"""Build the i915 selftest image and run one IGD passthrough attempt on the test host.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

The build, payload preparation, transfer and evidence collection reuse
plan/ws014/tests/run-venus-remote.py. Around the guest run, host-igd.sh moves
the host IGD from i915 to vfio-pci and back; restore always runs, and a
failed restore fails the attempt and reports the host state. The user
permitted GDM stop, i915 unbind and VFIO passthrough for WS029; reboot,
package installation and kernel command line changes are not permitted and
are never attempted here.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('run_venus_remote', REPO / 'plan/ws014/tests/run-venus-remote.py')
venus = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(venus)

EVIDENCE = ['result.json', 'guest.log', 'qemu.log', 'qmp.jsonl', 'console.log', 'console.bin', 'src.bin', 'dst.bin']
venus.PROFILES['i915'] = {
    'application': 'gpu-i915-test',
    'harness': '../../ws029/tests/i915-qemu.py',
    'modules': {'transport_harness': 'venus-qemu.py', 'rfb_client': 'venus_rfb.py'},
    'evidence': EVIDENCE,
    'source_directories': ['src/drivers/gpu/i915', 'userland/base/tests/gpu-i915', 'plan/ws029/tests'],
    'source_files': ['include/drivers/pci/pci-i915.h'],
}
HOST_SCRIPT = REPO / 'plan/ws029/tests/host-igd.sh'
STATE_KEYS = ['driver', 'gdm', 'dev_vfio', 'drm_nodes']


def host_igd(args, action, output, timeout):
    """Runs one host-igd.sh action through sudo and returns its parsed JSON status."""
    log = output / 'host.log'
    remote_script = PurePosixPath(args.remote_root) / 'host-igd.sh'
    argv = venus.ssh_command(args.host, ['sudo', '-n', 'sh', str(remote_script), action])
    with log.open('ab') as stream:
        stream.write((json.dumps({'argv': argv, 'action': action}) + '\n').encode())
    result = subprocess.run(argv, cwd=REPO, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    with log.open('ab') as stream:
        stream.write(result.stdout.encode() + result.stderr.encode() + (json.dumps({'exit_code': result.returncode}) + '\n').encode())
    status = None
    match = re.search(r'\{.*\}', result.stdout, re.S)
    if match:
        try:
            status = json.loads(match.group(0))
        except json.JSONDecodeError:
            status = None
    return {'action': action, 'exit_code': result.returncode, 'status': status,
            'stderr': result.stderr.strip()[-2000:]}


def same_state(before, after):
    if before is None or after is None:
        return False
    return all(before.get(key) == after.get(key) for key in STATE_KEYS)


def run(args):
    output = args.output_root.resolve() / args.attempt
    output.mkdir(parents=True, exist_ok=False)
    remote_attempt = PurePosixPath(args.remote_root) / args.attempt
    report = {'attempt': args.attempt, 'status': 'running',
              'started': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'host': args.host, 'remote_attempt': str(remote_attempt), 'profile': 'i915',
              'mode': 'hang' if args.hang else ('test' if args.test else 'boot-only'),
              'wrapper_sha256': venus.digest(__file__), 'host_script_sha256': venus.digest(HOST_SCRIPT),
              'remote_created': False, 'host_before': None, 'host_after': None, 'attach': None, 'restore': None}
    started = time.monotonic()
    venus.write_json(output / 'result.json', report)
    attached = False
    try:
        payload = venus.prepare_payload(args, output, report)
        script = ('import pathlib,sys; '
                  'pathlib.Path(sys.argv[1]).mkdir(parents=True,exist_ok=True); '
                  'pathlib.Path(sys.argv[2]).mkdir()')
        venus.logged(venus.ssh_command(args.host, ['python3', '-c', script, args.remote_root, str(remote_attempt)]),
                     output / 'transfer.log', 45)
        report['remote_created'] = True
        venus.write_json(output / 'result.json', report)
        venus.logged(['scp', *venus.SSH_OPTIONS, str(HOST_SCRIPT), f'{args.host}:{PurePosixPath(args.remote_root) / "host-igd.sh"}'],
                     output / 'transfer.log', 60)
        for name in ['boot.img', 'i915-qemu.py', 'venus-qemu.py', 'venus_rfb.py', 'vmunix', 'manifest.json']:
            venus.logged(['scp', *venus.SSH_OPTIONS, str(payload / name), f'{args.host}:{remote_attempt / name}'],
                         output / 'transfer.log', args.transfer_timeout)
        owner = venus.capture(venus.ssh_command(args.host, ['id', '-u'])).strip() + ':' + \
            venus.capture(venus.ssh_command(args.host, ['id', '-g'])).strip()
        report['host_before'] = host_igd(args, 'status', output, 60)
        if report['host_before']['exit_code'] != 0 or report['host_before']['status'] is None:
            raise RuntimeError('host status before the attempt could not be read')
        if report['host_before']['status'].get('qemu_processes'):
            raise RuntimeError('another QEMU is running on the host; refusing to attach')
        report['attach'] = host_igd(args, 'attach', output, 150)
        attached = True
        venus.write_json(output / 'result.json', report)
        if report['attach']['exit_code'] != 0:
            raise RuntimeError(f'host attach failed: {report["attach"]["stderr"]}')
        remote_limit = 2 * args.timeout + 180
        command = ['sudo', '-n', 'timeout', '--signal=TERM', '--kill-after=15s', str(remote_limit),
                   'python3', str(remote_attempt / 'i915-qemu.py'),
                   '--image', str(remote_attempt / 'boot.img'),
                   '--output', str(remote_attempt / 'capture'), '--timeout', str(args.timeout),
                   '--console-address', str(report['console']['physical_address']),
                   '--console-size', str(report['console']['bytes']),
                   '--host-device', args.host_device, '--owner', owner,
                   '--hang' if args.hang else ('--test' if args.test else '--boot-only')]
        report['remote_argv'] = command
        report['remote_watchdog_seconds'] = remote_limit
        venus.write_json(output / 'result.json', report)
        report['remote_exit_code'] = venus.logged(venus.ssh_command(args.host, command), output / 'remote.log',
                                                  remote_limit + 45, check=False)
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='fail', error=f'{type(error).__name__}: {error}')
    finally:
        if attached:
            try:
                report['restore'] = host_igd(args, 'restore', output, 200)
            except Exception as error:
                report['restore'] = {'action': 'restore', 'exit_code': -1, 'status': None, 'stderr': str(error)}
            try:
                report['host_after'] = host_igd(args, 'status', output, 60)
            except Exception as error:
                report['host_after'] = {'action': 'status', 'exit_code': -1, 'status': None, 'stderr': str(error)}
        if report['remote_created']:
            try:
                venus.fetch_evidence(args, remote_attempt, output, report)
            except Exception as error:
                report.update(evidence_error=str(error))
        try:
            remote = json.loads((output / 'evidence/result.json').read_text())
            report['remote_result'] = remote
        except Exception as error:
            remote = None
            if report['status'] == 'running':
                report.update(status='fail', error=f'remote result unavailable: {error}')
        if report['status'] == 'running':
            try:
                report['status'] = verify(args, report, remote)
            except Exception as error:
                report.update(status='fail', error=str(error))
        if attached:
            restore = report.get('restore') or {}
            before = (report.get('host_before') or {}).get('status')
            after = (report.get('host_after') or {}).get('status')
            report['host_restored'] = restore.get('exit_code') == 0 and same_state(before, after)
            if not report['host_restored']:
                report['status'] = 'fail'
                report['host_error'] = 'host restore failed or host state differs from before the attempt; see host.log'
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        venus.write_json(output / 'result.json', report)
    print(json.dumps({'status': report['status'], 'attempt': args.attempt, 'result': str(output / 'result.json'),
                      'error': report.get('error'), 'host_restored': report.get('host_restored'),
                      'i915': (report.get('remote_result') or {}).get('i915')}, indent=2))
    return 0 if report['status'] in ('pass', 'boot-pass') else 1


def verify(args, report, remote):
    expected = 'boot-pass' if args.boot_only else 'pass'
    if remote is None or report.get('remote_exit_code') != 0 or remote.get('status') != expected:
        raise RuntimeError('remote attempt did not complete successfully')
    if remote.get('qemu_exit_code') != 0:
        raise RuntimeError('remote QEMU did not exit cleanly')
    if (remote.get('console_address') != report['console']['physical_address'] or
            remote.get('console_size') != report['console']['bytes']):
        raise RuntimeError('remote console capture differs from the matching kernel symbols')
    image_hash = report['disposable_image']['sha256']
    if remote.get('image_sha256') != image_hash or remote.get('source_image_sha256') != image_hash:
        raise RuntimeError('remote QEMU image differs from the exact disposable artifact')
    if remote.get('harness_sha256') != report['artifacts']['harness']['sha256']:
        raise RuntimeError('remote capture used a different harness revision')
    if remote.get('host_device') != args.host_device:
        raise RuntimeError('remote capture used a different host device')
    i915 = remote.get('i915') or {}
    if i915.get('attach_stopped') is not None or not i915.get('registered'):
        raise RuntimeError('remote result does not show a complete i915 attach')
    if (i915.get('selftest') or {}).get('store') != 'ok':
        raise RuntimeError('remote result does not show a passing selftest')
    required = ['result.json', 'guest.log', 'qemu.log', 'qmp.jsonl', 'console.log', 'console.bin']
    if args.test:
        required += ['src.bin', 'dst.bin']
        if (remote.get('memory_check') or {}).get('status') != 'pass':
            raise RuntimeError('remote memory check did not pass')
        if (remote.get('guest_result') or {}).get('copy') != 1:
            raise RuntimeError('guest test did not report copy=1')
    fetched = report.get('fetched_evidence', {})
    missing = [name for name in required if name not in fetched]
    if missing:
        raise RuntimeError(f'required evidence missing: {missing}')
    return expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--attempt', required=True)
    parser.add_argument('--host', default='awe@10.0.10.25')
    parser.add_argument('--remote-root', default='/home/awe/zedbsd-q314-i915')
    parser.add_argument('--host-device', default='0000:00:02.0')
    parser.add_argument('--output-root', type=Path, default=REPO / 'plan/ws029/temp/remote')
    parser.add_argument('--build-directory', type=Path, default=REPO / 'build/i915-selftest-amd64')
    parser.add_argument('--config', type=Path, default=REPO / 'plan/ws029/tests/config-i915-selftest-amd64.mk')
    parser.add_argument('--image', type=Path)
    parser.add_argument('--init', default='/bin/sh')
    parser.add_argument('--skip-build', action='store_true')
    parser.add_argument('--boot-only', action='store_true')
    parser.add_argument('--test', action='store_true')
    parser.add_argument('--hang', action='store_true')
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--build-timeout', type=int, default=1800)
    parser.add_argument('--transfer-timeout', type=int, default=1200)
    args = parser.parse_args()
    args.profile = 'i915'
    if sum((args.boot_only, args.test, args.hang)) != 1:
        parser.error('choose exactly one of --boot-only, --test and --hang')
    if not re.fullmatch(r'[a-z0-9][a-z0-9_.-]{0,63}', args.attempt):
        parser.error('attempt must be one lowercase identifier, at most 64 characters')
    if not re.fullmatch(r'/home/[a-zA-Z0-9_./-]+', args.remote_root) or '..' in PurePosixPath(args.remote_root).parts:
        parser.error('remote-root must be a task directory below a user home')
    if not re.fullmatch(r'[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]', args.host_device):
        parser.error('host-device must be a PCI address like 0000:00:02.0')
    if len(str(PurePosixPath(args.remote_root) / args.attempt / 'capture/qmp.sock').encode()) >= 104:
        parser.error('remote attempt path is too long for a Unix QMP socket')
    if not 1 <= args.timeout <= 600:
        parser.error('timeout must be 1..600 seconds')
    return run(args)


if __name__ == '__main__':
    sys.exit(main())

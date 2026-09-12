#!/usr/bin/env python3
"""Capture six textured cuboid frames from one bounded zedBSD vkdemo process.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys
import time

from venus_rfb import capture as capture_rfb
import vkdemo_oracle as oracle

_spec = importlib.util.spec_from_file_location('venus_qemu', Path(__file__).with_name('venus-qemu.py'))
common = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(common)

MARKER = re.compile(r'VKDEMO PRESENT run=([a-z0-9-]{1,64}) mode=(fixed|live) '
                    r'sample=(\d+) frame=(\d+) time_ms=(\d+) '
                    r'rgb_sha256=([0-9a-f]{64}) width=(\d+) height=(\d+)')


def parse_marker(text, token, frame):
    matches = []
    for match in MARKER.finditer(text):
        run, mode, sample, observed_frame, at, sha, width, height = match.groups()
        if run == token and int(observed_frame) == frame:
            matches.append({'run': run, 'mode': mode, 'sample': int(sample),
                            'frame': int(observed_frame), 'time_ms': int(at),
                            'rgb_sha256': sha, 'width': int(width), 'height': int(height)})
    if not matches:
        return None
    if any(match != matches[0] for match in matches[1:]):
        raise ValueError('conflicting guest markers for the same run/frame')
    result = matches[0]
    expected_mode = 'fixed' if frame <= 3 else 'live'
    if (result['mode'] != expected_mode or result['sample'] != (frame - 1) % 3 + 1 or
            (result['width'], result['height']) != (oracle.WIDTH, oracle.HEIGHT) or
            not 0 <= result['time_ms'] <= 3600000):
        raise ValueError('guest marker identity/geometry/time is invalid')
    if frame <= 3 and result['time_ms'] != oracle.FIXED_TIMES[frame - 1]:
        raise ValueError('fixed timestamp differs from the independent schedule')
    return result


def ordinary_run(args, qmp, output, report, console, deadline):
    token = args.token + '-ordinary'
    command = f'/bin/vkdemo --duration=2 --token={token}\n'
    result = {'token': token, 'guest_command': command.rstrip(), 'start_seen': False,
              'samples': [], 'completed': False}
    report['ordinary'] = result
    qmp.text(command)
    observed = {}
    done = re.compile(r'VKDEMO DONE run=' + re.escape(token) + r' frames=(\d+)')
    start = re.compile(r'VKDEMO START run=' + re.escape(token) + r'(?:[ \r\n]|$)')
    while time.monotonic() < deadline:
        text = console()
        result['start_seen'] = result['start_seen'] or bool(start.search(text))
        for match in MARKER.finditer(text):
            run, mode, sample, frame, at, sha, width, height = match.groups()
            if run != token:
                continue
            sample, frame, at, width, height = map(int, (sample, frame, at, width, height))
            if (mode != 'live' or sample != frame or not 1 <= frame <= 4096 or
                    not 0 <= at <= 3600000 or (width, height) != (320, 240)):
                raise ValueError('ordinary run has an invalid live marker')
            value = {'frame': frame, 'time_ms': at, 'rgb_sha256': sha}
            if frame in observed and observed[frame] != value:
                raise ValueError('ordinary run has conflicting markers for one frame')
            observed[frame] = value
        result['samples'] = [observed[key] for key in sorted(observed)]
        finished = done.search(text)
        if finished and re.search(r'root@[^\r\n]*\$ ', text[finished.end():]):
            count = int(finished.group(1))
            samples = result['samples']
            if (not result['start_seen'] or len(samples) < 2 or count != samples[-1]['frame'] or
                    any(b['time_ms'] <= a['time_ms'] for a, b in zip(samples, samples[1:])) or
                    len({value['rgb_sha256'] for value in samples}) < 2):
                raise RuntimeError('ordinary run did not prove progressing frames and clean completion')
            result.update(completed=True, frames=count,
                          first_time_ms=samples[0]['time_ms'], last_time_ms=samples[-1]['time_ms'])
            return
        time.sleep(0.05)
    raise TimeoutError('ordinary vkdemo START, live progress, DONE and returned shell prompt')


def display_competition(args, qmp, output, debug, process, report, deadline):
    """Require a second process to fail its claim while its owner keeps drawing."""
    owner = args.token + '-owner'
    contender = args.token + '-contender'
    result = {'owner': owner, 'contender': contender, 'rejected': False,
              'owner_completed': False}
    report['competition'] = result
    baseline = debug.stat().st_size

    def text():
        if process.poll() is not None:
            raise RuntimeError('QEMU exited during display ownership competition')
        value = common.guest_text(debug, baseline) + common.console_text(qmp, output, args)
        if re.search(r'kernel panic|amd64 fault v=', value):
            raise RuntimeError('kernel failure during display ownership competition')
        return value

    qmp.text(f'/bin/vkdemo --duration=12 --token={owner} &\n')
    while time.monotonic() < deadline:
        value = text()
        if re.search(r'VKDEMO PRESENT run=' + re.escape(owner) + ' ', value):
            break
        time.sleep(0.05)
    else:
        raise TimeoutError('background owner did not present before the competing claim')

    qmp.text(f'/bin/vkdemo --duration=1 --token={contender}\n')
    failure = re.compile(r'VKDEMO FAILED run=' + re.escape(contender) +
                         r' api=direct display swapchain result=(-?\d+) frames=(\d+)')
    owner_done = re.compile(r'VKDEMO DONE run=' + re.escape(owner) + r' frames=(\d+)')
    while time.monotonic() < deadline:
        value = text()
        rejected = failure.search(value)
        if rejected:
            if rejected.groups() != ('-1000000001', '0'):
                raise RuntimeError('contender failed for a reason other than native window ownership')
            result['rejected'] = True
            result['error'] = 'VK_ERROR_NATIVE_WINDOW_IN_USE_KHR'
        if re.search(r'VKDEMO FAILED run=' + re.escape(owner) + ' ', value):
            raise RuntimeError('display owner failed during a rejected competing claim')
        done = owner_done.search(value)
        if done and result['rejected']:
            frames = int(done.group(1))
            if frames < 3:
                raise RuntimeError('owner did not continue rendering through the rejected claim')
            result.update(owner_completed=True, owner_frames=frames)
            save_report(output, report)
            return
        time.sleep(0.05)
    raise TimeoutError('competing claim rejection and unaffected owner completion')


def lifecycle_run(args, qmp, output, debug_path, vnc_path, process, report, console, deadline):
    """Interrupt a presenting process and capture the returned physical console."""
    result = {'abnormal_exit': False, 'reopened': False, 'console_restored': False}
    report['lifecycle'] = result
    token = args.token + '-abort'
    qmp.text(f'/bin/vkdemo --verify-session --token={token}\n')
    marker = None
    while time.monotonic() < deadline:
        text = console()
        marker = parse_marker(text, token, 1)
        if marker is not None:
            break
        time.sleep(0.05)
    if marker is None:
        raise TimeoutError('abnormal-exit run did not present its first held frame')
    result['held_frame'] = marker
    qmp.call('human-monitor-command', {'command-line': 'sendkey ctrl-c 1'})
    while time.monotonic() < deadline:
        text = console()
        last = text.rfind('VKDEMO PRESENT run=' + token)
        if last >= 0 and re.search(r'root@[^\r\n]*\$ ', text[last:]):
            if 'VKDEMO DONE run=' + token in text:
                raise RuntimeError('interrupted process unexpectedly completed normally')
            result['abnormal_exit'] = True
            break
        time.sleep(0.05)
    if not result['abnormal_exit']:
        raise TimeoutError('SIGINT did not terminate the presenting process')

    # This distinct run proves file-close cleanup made the display claim available.
    saved_token = args.token
    try:
        args.token = saved_token + '-after-abort'
        reopened = {}
        ordinary_run(args, qmp, output, reopened, console, deadline)
        result['reopen'] = reopened['ordinary']
        result['reopened'] = True
    finally:
        args.token = saved_token

    # A shell prompt in retained VT memory alone does not prove visible restoration.
    before = output / 'console-return.ppm'
    result['console_before'] = capture_rfb(vnc_path, before, 5)
    result['console_before_sha256'] = common.digest(before)
    qmp.text('echo console-restoration-check\n')
    time.sleep(0.30)
    after = output / 'console-write.ppm'
    result['console_after'] = capture_rfb(vnc_path, after, 5)
    result['console_after_sha256'] = common.digest(after)
    result['console_restored'] = before.read_bytes() != after.read_bytes()
    save_report(output, report)
    display_competition(args, qmp, output, debug_path, process, report, deadline)
    save_report(output, report)
    if not result['console_restored']:
        raise RuntimeError('returned shell output did not change the physical console framebuffer')


def save_report(output, report):
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


def exercise(args, qmp, output, debug, vnc_path, process, report):
    report.update(token=args.token, oracle_sha256=common.digest(Path(oracle.__file__)),
                  samples=[], frames=6)
    baseline = debug.stat().st_size
    command = f'/bin/vkdemo --verify-session --token={args.token}\n'
    report['guest_command'] = command.rstrip()
    qmp.text(command)
    deadline = time.monotonic() + args.timeout

    def console():
        if process.poll() is not None:
            raise RuntimeError('QEMU exited during vkdemo')
        text = common.guest_text(debug, baseline) + common.console_text(qmp, output, args)
        if re.search(r'VKDEMO FAIL|vkdemo:|kernel panic|amd64 fault v=', text):
            raise RuntimeError('guest vkdemo failure (see console.log and renderer log)')
        return text

    for frame in range(1, 7):
        while time.monotonic() < deadline:
            marker = parse_marker(console(), args.token, frame)
            if marker is not None:
                break
            time.sleep(0.05)
        else:
            raise TimeoutError(f'vkdemo frame {frame} PRESENT marker')
        # The guest holds this already-presented GPU result until our newline.
        present_at = time.monotonic()
        capture_deadline = min(deadline, present_at + 20)
        mismatch = 'no matching VNC RGB hash'
        shot = output / f'frame-{frame}.ppm'
        while time.monotonic() < capture_deadline:
            info = capture_rfb(vnc_path, shot, min(5, capture_deadline - time.monotonic()))
            try:
                pixels = oracle.read_ppm(shot)
            except ValueError as error:
                mismatch = str(error)
                time.sleep(0.05)
                continue
            rgb_hash = hashlib.sha256(pixels).hexdigest()
            if rgb_hash == marker['rgb_sha256']:
                result = oracle.verify_pixels(pixels, marker['time_ms'])
                sample = dict(marker, capture=info, oracle=result,
                              frame_sha256=common.digest(shot), filename=shot.name)
                oracle_path = output / f'oracle-{frame}.json'
                oracle_path.write_text(json.dumps(result, indent=2) + '\n')
                sample['oracle_result_sha256'] = common.digest(oracle_path)
                report['samples'].append(sample)
                save_report(output, report)
                if not result['passed']:
                    raise RuntimeError(f'frame {frame} disagrees with independent ray/texture oracle: '
                                       f'{result["mismatch_pixels"]} RGB pixels')
                break
            mismatch = f'VNC RGB SHA256 {rgb_hash} differs from GPU readback {marker["rgb_sha256"]}'
            time.sleep(0.05)
        else:
            raise TimeoutError(f'frame {frame}: {mismatch}')
        # Require real elapsed time before the next live sample; no guest time injection.
        remaining = present_at + 0.30 - time.monotonic()
        if remaining > 0:
            time.sleep(remaining)
        qmp.text('\n')

    report['sequence'] = oracle.verify_sequence(report['samples'])
    done = f'VKDEMO DONE run={args.token} frames=6'
    while time.monotonic() < deadline:
        text = console()
        where = text.find(done)
        if where >= 0 and re.search(r'root@[^\r\n]*\$ ', text[where + len(done):]):
            report['guest_completed'] = True
            ordinary_run(args, qmp, output, report, console, deadline)
            if getattr(args, 'lifecycle', False):
                lifecycle_run(args, qmp, output, debug, vnc_path, process, report, console, deadline)
            report['status'] = 'pass'
            save_report(output, report)
            return
        time.sleep(0.05)
    raise TimeoutError('vkdemo DONE followed by the returned shell prompt')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--render-server', type=Path, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--token', required=True)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--lifecycle', action='store_true')
    parser.add_argument('--console-address', type=lambda text: int(text, 0), required=True)
    parser.add_argument('--console-size', type=int, default=32768)
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--icd', default='/usr/share/vulkan/icd.d/intel_icd.json')
    parser.add_argument('--firmware', default='/usr/share/OVMF/OVMF_CODE_4M.fd')
    parser.add_argument('--variables', default='/usr/share/OVMF/OVMF_VARS_4M.fd')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9-]{1,55}', args.token):
        parser.error('token must contain 1..55 lowercase letters, digits or hyphens')
    if args.lifecycle and len(args.token) > 43:
        parser.error('lifecycle token must leave room for per-process suffixes (at most 43)')
    if not 1 <= args.timeout <= 600 or not 1 <= args.console_size <= 1048576:
        parser.error('timeout must be 1..600 and console-size 1..1048576')
    if not 0 <= args.console_address < 1024 ** 3 - args.console_size:
        parser.error('console capture must lie within the 1 GiB guest memory')
    args.phase, args.frame, args.boot_only = 'vkdemo', 0, False
    return common.run(args, exercise=exercise, harness_path=__file__)


if __name__ == '__main__':
    sys.exit(main())

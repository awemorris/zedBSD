#!/usr/bin/env python3
"""Run the model viewer on zdesktop in a finite QEMU/Venus session and check its views (WS031 p013).

The compositor and the viewer are started from the guest shell.  Input reaches the guest as a
QEMU USB tablet and the PS/2 keyboard through QMP input-send-event; the screen is read back over
VNC.  The checks: the first frame shows the model, a drag rotates it, a right drag pans it, the
wheel zooms it, the keyboard turns it, and R brings back the first frame pixel for pixel.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys
import time

from venus_rfb import capture as capture_rfb

_spec = importlib.util.spec_from_file_location('venus_qemu', Path(__file__).with_name('venus-qemu.py'))
common = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(common)

# The compositor's size; the viewer fills it.
WIDTH, HEIGHT = 640, 480

# The absolute range of the QEMU tablet.
ABS_MAX = 0x7fff


def read_ppm(path):
    """Returns the width, the height and the RGB bytes of a binary PPM."""
    data = Path(path).read_bytes()
    fields = []
    index = 0
    while len(fields) < 4:
        while data[index:index + 1].isspace():
            index += 1
        if data[index:index + 1] == b'#':
            while data[index:index + 1] not in (b'\n', b''):
                index += 1
            continue
        start = index
        while not data[index:index + 1].isspace():
            index += 1
        fields.append(data[start:index])
    if fields[0] != b'P6' or fields[3] != b'255':
        raise ValueError(f'{path}: not an 8-bit binary PPM')
    width, height = int(fields[1]), int(fields[2])
    pixels = data[index + 1:index + 1 + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError(f'{path}: truncated PPM')
    return width, height, pixels


def difference(left, right):
    """Returns the share of pixels that differ between two captures of one size."""
    width, height, a = read_ppm(left)
    other_width, other_height, b = read_ppm(right)
    if (width, height) != (other_width, other_height):
        return 1.0
    changed = sum(1 for index in range(0, len(a), 3) if a[index:index + 3] != b[index:index + 3])
    return changed / (width * height)


def coloured(path):
    """Returns the share of pixels that are neither the clear colour nor black (the model covers them)."""
    width, height, pixels = read_ppm(path)
    counts = {}
    for index in range(0, len(pixels), 3):
        key = pixels[index:index + 3]
        counts[key] = counts.get(key, 0) + 1
    background = max(counts.values())
    black = counts.get(b'\x00\x00\x00', 0)
    return (width * height - background - black) / (width * height)


def exercise(args, qmp, output, debug, vnc_path, process, report):
    deadline = time.monotonic() + args.timeout
    baseline = debug.stat().st_size
    report.update(token=args.token, images={}, checks={}, views={}, commands=[])
    seen = set()

    def console():
        if process.poll() is not None:
            raise RuntimeError('QEMU exited during the viewer test')
        value = common.guest_text(debug, baseline) + common.console_text(qmp, output, args)
        fresh = [line for line in value.splitlines() if line and line not in seen]
        seen.update(fresh)
        if fresh:
            with (output / 'mview-observed.log').open('a') as stream:
                stream.write('\n'.join(fresh) + '\n')
        if re.search(r'kernel panic|amd64 fault v=|ZWL FAILED|MVIEW FAILED|Segmentation fault', value):
            raise RuntimeError('guest compositor/viewer failure; inspect console and renderer logs')
        return value

    def command(value):
        report['commands'].append(value)
        qmp.text(value + '\n')

    def wait(pattern, description, after=0):
        matcher = re.compile(pattern)
        while time.monotonic() < deadline:
            found = [m for m in matcher.finditer(console())]
            if len(found) > after:
                return found[-1]
            time.sleep(0.05)
        raise TimeoutError(description)

    def count(pattern):
        return len(re.findall(pattern, console()))

    def view(tag):
        """Waits until the view settles (no new frame for a while), then captures it."""
        frame = re.compile(r'MVIEW FRAME run=' + re.escape(args.token) + r' frame=(\d+)([^\r\n]*)')
        last, stable_since = None, time.monotonic()
        while time.monotonic() < deadline:
            found = frame.findall(console())
            current = found[-1] if found else None
            if current != last:
                last, stable_since = current, time.monotonic()
            elif current is not None and time.monotonic() - stable_since > 1.5:
                break
            time.sleep(0.1)
        else:
            raise TimeoutError(f'settled view for {tag}')
        shot = output / f'{tag}.ppm'
        report.setdefault('rfb_captures', {})[tag] = capture_rfb(vnc_path, shot, 10)
        report['images'][f'{tag}.ppm'] = common.digest(shot)
        report['views'][tag] = last[1].strip() if last else None
        return shot

    def events(items):
        qmp.call('input-send-event', {'events': items})
        time.sleep(0.03)

    def move(x, y):
        events([{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * ABS_MAX / (WIDTH - 1))}},
                {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * ABS_MAX / (HEIGHT - 1))}}])

    def button(name, down):
        events([{'type': 'btn', 'data': {'button': name, 'down': down}}])

    def drag(name, start, end, steps=12):
        move(*start)
        button(name, True)
        for step in range(1, steps + 1):
            move(start[0] + (end[0] - start[0]) * step / steps, start[1] + (end[1] - start[1]) * step / steps)
        button(name, False)

    def key(name):
        # The viewer's keys go to the USB keyboard; the shell's typed commands use the PS/2 one.
        for down in (True, False):
            qmp.call('input-send-event', {'events': [
                {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': name}}}]})
            time.sleep(0.05)

    command(f'/bin/zdesktop --socket=/tmp/wayland-0 --width={WIDTH} --height={HEIGHT} --timeout={args.timeout} &')
    ready = wait(r'ZWL READY[^\r\n]*', 'compositor startup')
    report['compositor_ready'] = ready.group(0)
    report['compositor_inputs'] = re.findall(r'ZWL INPUT [^\r\n]*', console())
    command(f'/bin/mview --display=/tmp/wayland-0 --token={args.token} --timeout-s={args.timeout}')
    report['viewer_start'] = wait(r'MVIEW START run=' + re.escape(args.token) + r'[^\r\n]*', 'viewer startup').group(0)
    wait(r'ZWL SEAT[^\r\n]*', 'viewer seat binding')

    initial = view('initial')
    report['checks']['model_visible'] = coloured(initial) > 0.02

    inputs = count(r'MVIEW INPUT run=')
    drag('left', (320, 240), (420, 260))
    wait(r'MVIEW INPUT run=' + re.escape(args.token), 'rotation input', after=inputs)
    rotate = view('rotate')
    report['checks']['drag_rotates'] = difference(initial, rotate) > 0.01

    inputs = count(r'MVIEW INPUT run=')
    drag('right', (320, 240), (260, 200))
    wait(r'MVIEW INPUT run=' + re.escape(args.token), 'pan input', after=inputs)
    pan = view('pan')
    report['checks']['right_drag_pans'] = difference(rotate, pan) > 0.01

    inputs = count(r'MVIEW INPUT run=')
    for _ in range(3):
        button('wheel-up', True)
        button('wheel-up', False)
    wait(r'MVIEW INPUT run=' + re.escape(args.token), 'zoom input', after=inputs)
    zoom = view('zoom')
    report['checks']['wheel_zooms'] = difference(pan, zoom) > 0.01

    inputs = count(r'MVIEW INPUT run=')
    for _ in range(4):
        key('left')
    wait(r'MVIEW INPUT run=' + re.escape(args.token), 'keyboard input', after=inputs)
    keys = view('keys')
    report['checks']['keys_turn'] = difference(zoom, keys) > 0.01

    inputs = count(r'MVIEW INPUT run=')
    key('r')
    wait(r'MVIEW INPUT run=' + re.escape(args.token), 'reset input', after=inputs)
    reset = view('reset')
    report['checks']['reset_restores_first_frame'] = difference(initial, reset) == 0.0

    key('q')
    report['viewer_done'] = wait(r'MVIEW DONE run=' + re.escape(args.token) + r'[^\r\n]*', 'viewer exit').group(0)
    # The viewer's keys also reached the shell's line; an empty command clears it first.
    command('')
    command('kill ' + re.search(r'pid=(\d+)', report['compositor_ready']).group(1))
    report['compositor_exit'] = wait(r'ZWL EXIT[^\r\n]*', 'compositor exit').group(0)
    report['status'] = 'pass' if all(report['checks'].values()) else 'fail'
    if report['status'] != 'pass':
        report['error'] = 'viewer checks failed: ' + json.dumps(report['checks'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--render-server', type=Path, required=True)
    parser.add_argument('--renderer-library-dir', type=Path)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--token', required=True)
    parser.add_argument('--timeout', type=int, default=240)
    parser.add_argument('--console-address', type=lambda value: int(value, 0), required=True)
    parser.add_argument('--console-size', type=int, default=32768)
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--icd', default='/usr/share/vulkan/icd.d/intel_icd.json')
    parser.add_argument('--firmware', default='/usr/share/OVMF/OVMF_CODE_4M.fd')
    parser.add_argument('--variables', default='/usr/share/OVMF/OVMF_VARS_4M.fd')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9-]{1,43}', args.token):
        parser.error('token must be a short lowercase identifier')
    if not 1 <= args.timeout <= 600 or not 1 <= args.console_size <= 1048576:
        parser.error('bounded timeout or console size is invalid')
    args.phase, args.frame, args.boot_only = 'mview', 0, False
    # The viewer's pointer and keyboard: an absolute USB tablet and a USB keyboard on an xHCI controller.
    args.extra_qemu = ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-tablet,bus=xhci.0',
                       '-device', 'usb-kbd,bus=xhci.0,id=mviewkbd']
    return common.run(args, exercise=exercise, harness_path=__file__)


if __name__ == '__main__':
    sys.exit(main())

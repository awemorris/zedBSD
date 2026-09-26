#!/usr/bin/env python3
"""Read the i915 capture display out of guest RAM during a passthrough run (WS031 p014 A0).

Runs on the passthrough host next to QEMU (root, for the QMP socket).  The capture build of the
kernel copies every presented frame into a contiguous area of guest RAM and logs its guest
physical base; this harness reads the area with QMP pmemsave, writes PPM images and checks the
scenario:

  vkdemo   the captured frame's RGB SHA-256 equals the one vkdemo reports for the frame it presented
  wayland  frames of wltest through zwl are captured (saved for inspection)
  mview    the model viewer through zwl: the p013 input sequence and six checks, and the similarity of
           each view to the Venus images of p013 when a reference directory is given (--no-venus skips
           that comparison, for a viewer run the p013 images do not describe, such as --shading=pixel);
           the six views are also laid out on one sheet, sheet.png
  zdesktop-home WS035 p069: App Home opened by the launcher, then zdesktop-terminal and mview started
           from their icons (the run's ZDESKTOP_APP=home leaves the viewer's service idle)
  zdesktop WS035 p066: zwl --glass (Wiseman Mode) at 1920x1080 with three 800x560 wl_shm windows: the
           desktop, the top one docked by a double click on its title bar, Wiseview opened by a
           drag up from the bottom edge, Wiseview closed, and the docked viewer closed with the bar's close
           button (the desktop is drawn again); each view differs from the one before it (the
           serial log only tells when the viewer started; the checks are on the images); sheet.png

The area layout is the comment at the top of src/drivers/gpu/i915/display/capture.c.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import socket
import struct
import sys
import time
import zlib

# Formats of the gpu-display UAPI.
FORMAT_BGRA8888 = 1
FORMAT_RGBA8888 = 2

# The compositor's size in the mview scenario; the tablet's absolute range.
WIDTH, HEIGHT = 640, 480
ABS_MAX = 0x7fff


class QMP:
    """A minimal QMP client: commands and their replies, events are skipped."""

    def __init__(self, path, deadline):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        while True:
            try:
                self.socket.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() > deadline:
                    raise TimeoutError('QMP socket')
                time.sleep(0.2)
        self.stream = self.socket.makefile('rwb', buffering=0)
        self.sequence = 0
        self.receive()
        self.call('qmp_capabilities')

    def receive(self):
        raw = self.stream.readline()
        if not raw:
            raise RuntimeError('QMP disconnected')
        return json.loads(raw)

    def call(self, name, arguments=None):
        self.sequence += 1
        request = {'execute': name, 'id': self.sequence}
        if arguments is not None:
            request['arguments'] = arguments
        self.stream.write((json.dumps(request) + '\n').encode())
        while True:
            reply = self.receive()
            if reply.get('id') != self.sequence:
                continue
            if 'error' in reply:
                raise RuntimeError(f'{name}: {reply["error"]}')
            return reply.get('return')


class Capture:
    """The capture area of one run: its base from the serial log, its slots through pmemsave."""

    def __init__(self, qmp, output, base):
        self.qmp = qmp
        self.output = output
        self.base = base
        self.scratch = output / 'pmem.bin'
        header = self.read(base, 0x50)
        if header[:8] != b'I915CAP1':
            raise RuntimeError('capture area has no I915CAP1 header at the logged base')
        (self.version, self.header_bytes, self.slot_count, self.pixel_offset, self.slot_bytes,
         self.first_slot_offset, self.total_bytes, logged_base, self.max_width,
         self.max_height) = struct.unpack_from('<IIIIQQQQII', header, 8)
        if logged_base != base:
            raise RuntimeError('capture header names a different base than the log')

    def read(self, address, size):
        self.qmp.call('pmemsave', {'val': address, 'size': size, 'filename': str(self.scratch)})
        data = self.scratch.read_bytes()
        if len(data) != size:
            raise RuntimeError('short pmemsave')
        return data

    def write_count(self):
        header = self.read(self.base, 0x50)
        return struct.unpack_from('<Q', header, 0x40)[0]

    def slot_address(self, index):
        return self.base + self.first_slot_offset + index * self.slot_bytes

    def slot_header(self, index):
        data = self.read(self.slot_address(index), 0x60)
        if data[:8] != b'I915SLOT':
            return None
        sequence, width, height, stride, fmt = struct.unpack_from('<QIIII', data, 8)
        ready = struct.unpack_from('<Q', data, 0x58)[0]
        return {'sequence': sequence, 'width': width, 'height': height, 'stride': stride,
                'format': fmt, 'ready': ready}

    def latest(self):
        """Returns the newest complete capture as (sequence, width, height, RGB bytes)."""
        for _ in range(8):
            best, best_index = None, None
            for index in range(self.slot_count):
                header = self.slot_header(index)
                if header and header['ready'] != 0 and header['ready'] == header['sequence']:
                    if best is None or header['sequence'] > best['sequence']:
                        best, best_index = header, index
            if best is None:
                raise RuntimeError('no complete capture yet')
            size = best['stride'] * best['height']
            pixels = self.read(self.slot_address(best_index) + self.pixel_offset, size)
            again = self.slot_header(best_index)
            if again and again['ready'] == best['sequence']:
                return best['sequence'], best['width'], best['height'], to_rgb(best, pixels)
        raise RuntimeError('capture slot kept changing while it was read')

    def save(self, tag):
        sequence, width, height, rgb = self.latest()
        path = self.output / f'{tag}.ppm'
        path.write_bytes(b'P6\n%d %d\n255\n' % (width, height) + rgb)
        return {'sequence': sequence, 'width': width, 'height': height, 'path': str(path),
                'rgb_sha256': hashlib.sha256(rgb).hexdigest()}


def to_rgb(header, pixels):
    """Packs the rows of a BGRA or RGBA capture as RGB."""
    width, stride = header['width'], header['stride']
    out = bytearray(width * header['height'] * 3)
    for row in range(header['height']):
        line = pixels[row * stride:row * stride + width * 4]
        target = row * width * 3
        if header['format'] == FORMAT_BGRA8888:
            out[target:target + width * 3:3] = line[2::4]
            out[target + 1:target + width * 3:3] = line[1::4]
            out[target + 2:target + width * 3:3] = line[0::4]
        else:
            out[target:target + width * 3:3] = line[0::4]
            out[target + 1:target + width * 3:3] = line[1::4]
            out[target + 2:target + width * 3:3] = line[2::4]
    return bytes(out)


def read_ppm(path):
    """Returns the width, the height and the RGB bytes of a binary PPM."""
    data = Path(path).read_bytes()
    parts = data.split(maxsplit=4)
    if parts[0] != b'P6' or parts[3] != b'255':
        raise ValueError(f'{path}: not an 8-bit binary PPM')
    width, height = int(parts[1]), int(parts[2])
    return width, height, parts[4][:width * height * 3]


def difference(left, right):
    """The share of pixels that differ between two images of one size (1.0 if the sizes differ)."""
    width, height, a = read_ppm(left)
    other_width, other_height, b = read_ppm(right)
    if (width, height) != (other_width, other_height):
        return 1.0
    changed = sum(1 for index in range(0, len(a), 3) if a[index:index + 3] != b[index:index + 3])
    return changed / (width * height)


def psnr(left, right):
    """The peak signal-to-noise ratio of two images of one size, in dB (inf when equal)."""
    width, height, a = read_ppm(left)
    other_width, other_height, b = read_ppm(right)
    if (width, height) != (other_width, other_height):
        return 0.0
    error = sum((x - y) * (x - y) for x, y in zip(a, b)) / len(a)
    return math.inf if error == 0 else 10 * math.log10(255 * 255 / error)


def write_sheet(paths, output, columns=3, gap=4):
    """Lays images of one size out in a grid on a grey sheet and writes it as an RGB PNG."""
    images = [read_ppm(path) for path in paths]
    width, height = images[0][0], images[0][1]
    rows = (len(images) + columns - 1) // columns
    sheet_width = columns * width + (columns + 1) * gap
    sheet_height = rows * height + (rows + 1) * gap
    sheet = bytearray(b'\x60' * (sheet_width * sheet_height * 3))
    for number, (image_width, image_height, pixels) in enumerate(images):
        if (image_width, image_height) != (width, height):
            raise ValueError('sheet images differ in size')
        left = gap + (number % columns) * (width + gap)
        top = gap + (number // columns) * (height + gap)
        for row in range(height):
            start = ((top + row) * sheet_width + left) * 3
            sheet[start:start + width * 3] = pixels[row * width * 3:(row + 1) * width * 3]
    raw = b''.join(b'\x00' + bytes(sheet[row * sheet_width * 3:(row + 1) * sheet_width * 3])
                   for row in range(sheet_height))

    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data +
                struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))

    header = struct.pack('>IIBBBBB', sheet_width, sheet_height, 8, 2, 0, 0, 0)
    Path(output).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
                             chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def coloured(path):
    """The share of pixels that are neither the most common colour (the clear colour) nor black."""
    width, height, pixels = read_ppm(path)
    counts = {}
    for index in range(0, len(pixels), 3):
        key = pixels[index:index + 3]
        counts[key] = counts.get(key, 0) + 1
    background = max(counts.values())
    black = counts.get(b'\x00\x00\x00', 0)
    return (width * height - background - black) / (width * height)


def run(args):
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    report = {'scenario': args.scenario, 'status': 'running', 'checks': {}, 'images': {}}
    deadline = time.monotonic() + args.timeout
    serial = Path(args.serial)

    def log():
        try:
            return serial.read_bytes().decode(errors='replace')
        except FileNotFoundError:
            return ''

    def wait(pattern, description, after=0):
        matcher = re.compile(pattern)
        while time.monotonic() < deadline:
            text = log()
            if re.search(r'kernel panic|amd64 fault v=|MVIEW FAILED|WLTEST FAILED|VKDEMO FAILED', text):
                raise RuntimeError('guest failure; see the serial log')
            found = list(matcher.finditer(text))
            if len(found) > after:
                return found[-1]
            time.sleep(0.2)
        raise TimeoutError(description)

    try:
        # The area is found in guest memory by its header, not in the serial log (WS035 p066).
        qmp = QMP(args.qmp, deadline)
        base = scan_for_area(qmp, output, deadline)
        report['base'] = hex(base)
        capture = Capture(qmp, output, base)
        report['area'] = {'slots': capture.slot_count, 'slot_bytes': capture.slot_bytes}

        def settled(tag):
            """Waits until no capture has arrived for a while, then saves the newest."""
            last, since = None, time.monotonic()
            while time.monotonic() < deadline:
                count = capture.write_count()
                if count != last:
                    last, since = count, time.monotonic()
                elif count and time.monotonic() - since > args.settle:
                    break
                time.sleep(0.2)
            else:
                raise TimeoutError(f'settled capture for {tag}')
            shot = capture.save(tag)
            report['images'][tag] = shot
            return Path(shot['path'])

        if args.scenario == 'vkdemo':
            marker = wait(r'VKDEMO PRESENT run=\S+ [^\r\n]*rgb_sha256=([0-9a-f]{64}) width=(\d+) height=(\d+)',
                          'vkdemo present')
            time.sleep(1.0)
            shot = capture.save('vkdemo')
            report['images']['vkdemo'] = shot
            report['expected_rgb_sha256'] = marker.group(1)
            report['checks']['capture_equals_presented'] = shot['rgb_sha256'] == marker.group(1)
        elif args.scenario == 'wayland':
            wait(r'WLTEST START run=wl1', 'wltest start')
            wait(r'i915: capture: frame=\d+', 'first capture')
            time.sleep(2.0)
            report['images']['wltest-a'] = capture.save('wltest-a')
            time.sleep(1.0)
            report['images']['wltest-b'] = capture.save('wltest-b')
            report['checks']['frames_captured'] = capture.write_count() > 1
            report['checks']['animation_changes'] = (report['images']['wltest-a']['rgb_sha256'] !=
                                                     report['images']['wltest-b']['rgb_sha256'])
        elif args.scenario == 'zdesktop':
            zdesktop(args, qmp, capture, report, wait)
        elif args.scenario == 'zdesktop-home':
            zdesktop_home(args, qmp, capture, report)
        else:
            mview(args, qmp, capture, report, wait, settled)
        report['write_count'] = capture.write_count()
        report['status'] = 'pass' if report['checks'] and all(report['checks'].values()) else 'fail'
    except Exception as error:
        report.update(status='fail', error=f'{type(error).__name__}: {error}')
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report.get(key) for key in ('status', 'error', 'checks')}, indent=2))
    return 0 if report['status'] == 'pass' else 1


def mview(args, qmp, capture, report, wait, settled):
    """The p013 input sequence and checks on the capture display."""
    token = args.token
    # The frames the input causes must arrive within the run's budget.
    deadline = time.monotonic() + args.timeout

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
        for down in (True, False):
            events([{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': name}}}])

    def wait_frames(count, description):
        """Waits until the capture area holds at least `count` captures."""
        while time.monotonic() < deadline:
            if capture.write_count() >= count:
                return
            time.sleep(0.1)
        raise TimeoutError(description)

    # The serial console interleaves the viewer's lines with the kernel's, so frames are counted
    # in the capture area rather than read from the log.
    report['viewer_start'] = wait(r'MVIEW START run=' + re.escape(token) + r'[^\r\n]*', 'viewer start').group(0)
    wait_frames(1, 'first viewer frame')
    initial = settled('initial')
    report['checks']['model_visible'] = coloured(initial) > 0.02

    steps = [('rotate', lambda: drag('left', (320, 240), (420, 260)), 'drag_rotates'),
             ('pan', lambda: drag('right', (320, 240), (260, 200)), 'right_drag_pans'),
             ('zoom', lambda: [button('wheel-up', down) for _ in range(3) for down in (True, False)], 'wheel_zooms'),
             ('keys', lambda: [key('left') for _ in range(4)], 'keys_turn')]
    previous = initial
    for tag, action, check in steps:
        count = capture.write_count()
        action()
        wait_frames(count + 1, f'{tag} input')
        shot = settled(tag)
        report['checks'][check] = difference(previous, shot) > 0.01
        previous = shot
    count = capture.write_count()
    key('r')
    wait_frames(count + 1, 'reset input')
    reset = settled('reset')
    report['checks']['reset_restores_first_frame'] = difference(initial, reset) == 0.0
    key('q')
    report['viewer_done'] = wait(r'MVIEW DONE run=' + re.escape(token) + r'[^\r\n]*', 'viewer exit').group(0)

    # The six views on one sheet, in the order they were taken.
    tags = ('initial', 'rotate', 'pan', 'zoom', 'keys', 'reset')
    sheet = Path(args.output) / 'sheet.png'
    write_sheet([report['images'][tag]['path'] for tag in tags], sheet)
    report['sheet'] = str(sheet)

    # The Venus images of p013 are a reference: another GPU, so similar rather than equal.  A run they do not
    # describe (--no-venus) skips the comparison.
    if args.no_venus:
        report['psnr_vs_venus'] = 'skipped (--no-venus)'
    elif args.reference:
        report['psnr_vs_venus'] = {}
        for tag in ('initial', 'rotate', 'pan', 'zoom', 'keys', 'reset'):
            reference = Path(args.reference) / f'{tag}.ppm'
            if reference.exists():
                value = psnr(Path(report['images'][tag]['path']), reference)
                report['psnr_vs_venus'][tag] = value if value != math.inf else 'inf'
                report['checks'][f'{tag}_like_venus'] = value >= args.min_psnr


def scan_for_area(qmp, output, deadline, low=0x100000000, high=0x180000000, piece=0x4000000):
    """Finds the capture area by its I915CAP1 header (4 KiB aligned) in guest memory above 4 GiB,
    reading it in pieces with pmemsave; tries again until the kernel has made it."""
    scratch = output / 'scan.bin'
    while time.monotonic() < deadline:
        for start in range(low, high, piece):
            try:
                qmp.call('pmemsave', {'val': start, 'size': piece, 'filename': str(scratch)})
            except RuntimeError:
                break
            data = scratch.read_bytes()
            at = data.find(b'I915CAP1')
            while at >= 0:
                if at % 4096 == 0 and struct.unpack_from('<Q', data, at + 0x30)[0] == start + at:
                    scratch.unlink()
                    return start + at
                at = data.find(b'I915CAP1', at + 1)
        time.sleep(2.0)
    raise TimeoutError('capture area (memory scan)')


def zdesktop(args, qmp, capture, report, wait):
    """The glass look on the capture display: the desktop, docking, Wiseview."""
    width, height = 1920, 1080
    time_limit = time.monotonic() + args.timeout
    # Where zwl places the window on top: all are 800x560 (plan/ws031/tests/zdesktop/), each mapped one
    # cascade step of 32 after the one before, centred under the system bar and a title bar
    # (userland/base/zwl/shell.c zwl_glass_place).  The viewer is the fourth mapped: two wl_shm windows
    # and the Vulkan window killed while it draws (wlkill) come before it.
    top = 34 + 12 + 44 + 8
    mview_x = (width - 800) // 2 + 32 * 3
    mview_y = top + (height - top - 12 - 560) // 2 + 32 * 3
    title = (mview_x + 300, mview_y - 8 - 22)

    def events(items):
        qmp.call('input-send-event', {'events': items})
        time.sleep(0.03)

    def move(x, y):
        # zwl takes the pixel floor(v * (size - 1) / 32767): v is rounded up.
        events([{'type': 'abs', 'data': {'axis': 'x', 'value': (int(x) * ABS_MAX + width - 2) // (width - 1)}},
                {'type': 'abs', 'data': {'axis': 'y', 'value': (int(y) * ABS_MAX + height - 2) // (height - 1)}}])

    def button(down):
        events([{'type': 'btn', 'data': {'button': 'left', 'down': down}}])

    def shot(tag, pause):
        # The clients present once a second, so the view is taken a while after the input.
        time.sleep(pause)
        taken = capture.save(tag)
        report['images'][tag] = taken
        return Path(taken['path'])

    # The clients start some seconds after the compositor's first frame (plan/ws031/tests/zdesktop/).
    while capture.write_count() == 0:
        if time.monotonic() > time_limit:
            raise TimeoutError('first frame')
        time.sleep(0.5)
    move(width - 40, height - 200)
    desktop = shot('desktop', 25.0)
    report['checks']['desktop_drawn'] = coloured(desktop) > 0.02

    # A double click on the top window's title bar docks it.
    move(*title)
    time.sleep(0.3)
    for _ in range(2):
        button(True)
        time.sleep(0.05)
        button(False)
        time.sleep(0.05)
    move(width - 40, height - 200)
    docked = shot('docked', 4.0)
    report['checks']['dock_changes_view'] = difference(desktop, docked) > 0.05

    # A drag up from the bottom edge opens Wiseview.
    move(width // 2, height - 6)
    button(True)
    for step in range(1, 13):
        move(width // 2, height - 6 - step * 30)
        time.sleep(0.05)
    button(False)
    wiseview = shot('wiseview', 4.0)
    report['checks']['wiseview_changes_view'] = difference(docked, wiseview) > 0.05

    # A click on empty space closes it again.
    move(20, height // 2)
    button(True)
    button(False)
    closed = shot('closed', 4.0)
    report['checks']['wiseview_closes'] = difference(wiseview, closed) > 0.05

    # The close button of the window docked in the bar (x of the bar's buttons at 1920 wide, as docked.png
    # shows them) asks the viewer to close: it ends (MVIEW DONE reason=closed in its log) and the guest
    # powers off some seconds later (plan/ws031/tests/zdesktop/vkwait2).  The compositor keeps drawing: the
    # desktop comes back with the windows under it.
    move(1430, 17)
    time.sleep(0.5)
    button(True)
    time.sleep(0.05)
    button(False)
    move(width - 40, height - 200)
    ended = shot('ended', 3.0)
    report['checks']['close_ends_viewer'] = difference(closed, ended) > 0.05
    report['checks']['desktop_after_close'] = coloured(ended) > 0.02

    sheet = Path(args.output) / 'sheet.png'
    write_sheet([report['images'][tag]['path'] for tag in ('desktop', 'docked', 'wiseview', 'closed', 'ended')], sheet, columns=3)
    report['sheet'] = str(sheet)


def zdesktop_home(args, qmp, capture, report):
    """App Home (WS035 p069) on the capture display: the launcher opens it, the Terminal icon starts
    zdesktop-terminal, and the Model viewer icon starts mview; each view differs from the one before."""
    width, height = 1920, 1080
    time_limit = time.monotonic() + args.timeout
    # The icons of the built-in list (userland/base/zwl/home.c home_layout at 1920x1080, four
    # applications in one row): cells of 144 from x = (1920 - 4 * 144) / 2, the row's top 34 + 2/5 of the
    # space under the bar less the row, the icon 72 high 20 under the cell's top.
    left = (width - 4 * 144) // 2
    icon_y = 34 + (height - 34 - 152) * 2 // 5 + 20 + 36
    terminal = (left + 72, icon_y)
    viewer = (left + 144 + 72, icon_y)

    def events(items):
        qmp.call('input-send-event', {'events': items})
        time.sleep(0.03)

    def move(x, y):
        events([{'type': 'abs', 'data': {'axis': 'x', 'value': (int(x) * ABS_MAX + width - 2) // (width - 1)}},
                {'type': 'abs', 'data': {'axis': 'y', 'value': (int(y) * ABS_MAX + height - 2) // (height - 1)}}])

    def click(x, y):
        move(x, y)
        time.sleep(0.3)
        events([{'type': 'btn', 'data': {'button': 'left', 'down': True}}])
        time.sleep(0.05)
        events([{'type': 'btn', 'data': {'button': 'left', 'down': False}}])

    def shot(tag, pause):
        time.sleep(pause)
        taken = capture.save(tag)
        report['images'][tag] = taken
        return Path(taken['path'])

    # The desktop, once the compositor draws and the wl_shm windows are up.
    while capture.write_count() == 0:
        if time.monotonic() > time_limit:
            raise TimeoutError('first frame')
        time.sleep(0.5)
    move(width - 40, height - 200)
    desktop = shot('desktop', 25.0)
    report['checks']['desktop_drawn'] = coloured(desktop) > 0.02

    # The launcher opens Home.
    click(23, 17)
    move(width - 300, height - 300)
    home = shot('home', 2.0)
    report['checks']['home_opens'] = difference(desktop, home) > 0.05

    # The Terminal icon starts the terminal and Home closes.
    click(*terminal)
    move(width - 40, height - 200)
    started = shot('terminal', 8.0)
    report['checks']['terminal_starts'] = difference(home, started) > 0.05

    # Home again, and the Model viewer icon starts mview.
    click(23, 17)
    time.sleep(1.5)
    click(*viewer)
    move(width - 40, height - 200)
    viewer_shot = shot('mview', 15.0)
    report['checks']['mview_starts'] = difference(started, viewer_shot) > 0.02

    sheet = Path(args.output) / 'sheet.png'
    write_sheet([report['images'][tag]['path'] for tag in ('desktop', 'home', 'terminal', 'mview')], sheet, columns=2)
    report['sheet'] = str(sheet)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('scenario', choices=['vkdemo', 'wayland', 'mview', 'zdesktop', 'zdesktop-home'])
    parser.add_argument('--output', required=True)
    parser.add_argument('--serial', default='/home/awe/bigbang/run-parity-serial.log')
    parser.add_argument('--qmp', default='/home/awe/bigbang/qmp.sock')
    parser.add_argument('--timeout', type=int, default=300)
    parser.add_argument('--settle', type=float, default=1.5)
    parser.add_argument('--token', default='mv1')
    parser.add_argument('--reference', help='directory of the Venus images (initial.ppm ...)')
    parser.add_argument('--min-psnr', type=float, default=20.0)
    parser.add_argument('--no-venus', action='store_true',
                        help='skip the comparison with the Venus images (a viewer run they do not describe)')
    return run(parser.parse_args())


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Capture one complete QEMU framebuffer through an isolated Unix RFB socket.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

This finite RFB 3.8 client requests true-color RAW pixels.  It uses only the
Python standard library and neither opens TCP ports nor changes QEMU state.
"""
import hashlib
import os
from pathlib import Path
import socket
import struct
import tempfile
import time


MAX_DIMENSION = 4096
MAX_PIXELS = MAX_DIMENSION * MAX_DIMENSION
MAX_TEXT = 65536
MAX_RAW_BYTES = MAX_PIXELS * 8
MAX_RECTANGLES = 4096
MAX_MESSAGES = 128
RAW = 0
DESKTOP_SIZE = -223
LAST_RECT = -224


class _Reader:
    """Apply one absolute deadline to connect, partial receives and sends."""

    def __init__(self, stream, deadline):
        self.stream = stream
        self.deadline = deadline

    def remaining(self):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('RFB capture exceeded its total deadline')
        return remaining

    def receive(self, count):
        if count < 0 or count > MAX_PIXELS * 4:
            raise ValueError('RFB receive length exceeds the finite buffer limit')
        result = bytearray()
        while len(result) < count:
            self.stream.settimeout(self.remaining())
            part = self.stream.recv(min(count - len(result), 65536))
            if not part:
                raise EOFError('RFB peer disconnected before its message completed')
            result.extend(part)
        return result

    def send(self, data):
        self.stream.settimeout(self.remaining())
        self.stream.sendall(data)

    def text(self):
        count = struct.unpack('!I', self.receive(4))[0]
        if count > MAX_TEXT:
            raise ValueError('RFB text length exceeds the finite protocol limit')
        return self.receive(count)


def _geometry(width, height):
    if not 1 <= width <= MAX_DIMENSION or not 1 <= height <= MAX_DIMENSION:
        raise ValueError(f'RFB framebuffer dimensions are unsupported: {width}x{height}')
    if width * height > MAX_PIXELS:
        raise ValueError('RFB framebuffer pixel count exceeds its finite limit')


def _request(reader, width, height):
    # A nonincremental request requires the server to send actual current pixels.
    reader.send(struct.pack('!BBHHHH', 3, 0, 0, 0, width, height))


def _save_ppm(path, width, height, pixels, reader):
    # Publish only a complete capture; failed writes cannot replace a prior image.
    temporary = None
    try:
        reader.remaining()
        with tempfile.NamedTemporaryFile(prefix=path.name + '.', dir=path.parent,
                                         delete=False) as output:
            temporary = Path(output.name)
            output.write(f'P6\n{width} {height}\n255\n'.encode('ascii'))
            output.write(pixels)
        reader.remaining()
        digest = hashlib.sha256(temporary.read_bytes()).hexdigest()
        reader.remaining()
        os.replace(temporary, path)
        temporary = None
        return digest
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def capture(socket_path, output_ppm, timeout=10):
    """Save one complete current framebuffer as PPM and return bounded diagnostics.

    The socket must be an absolute local Unix path.  QEMU must advertise RFB 3.8
    and the None security type on this private test socket.  Network addresses,
    credential fallback, unsupported encodings and incomplete frames fail.
    """
    socket_path = Path(socket_path)
    output_ppm = Path(output_ppm)
    if not socket_path.is_absolute():
        raise ValueError('RFB capture accepts only an absolute Unix socket path')
    if not 0 < timeout <= 600:
        raise ValueError('RFB timeout must be greater than zero and at most 600 seconds')
    deadline = time.monotonic() + timeout
    report = {'method': 'rfb-raw-unix', 'rectangles': 0, 'framebuffer_updates': 0,
              'desktop_resizes': 0, 'last_rectangles': 0, 'raw_bytes': 0,
              'pixel_format': {'bits_per_pixel': 32, 'depth': 24, 'big_endian': False,
                               'red_shift': 16, 'green_shift': 8, 'blue_shift': 0}}
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stream:
        reader = _Reader(stream, deadline)
        stream.settimeout(reader.remaining())
        stream.connect(str(socket_path))

        # Refuse other protocol versions instead of inferring a different handshake.
        version = reader.receive(12)
        if version != b'RFB 003.008\n':
            raise ValueError('RFB peer does not advertise protocol 3.8')
        report['server_version'] = version.decode('ascii').strip()
        reader.send(version)
        count = reader.receive(1)[0]
        if count == 0:
            reader.text()
            raise RuntimeError('RFB peer refused the connection')
        security_types = reader.receive(count)
        if 1 not in security_types:
            raise RuntimeError('RFB private socket does not offer None security')
        reader.send(b'\x01')
        result = struct.unpack('!I', reader.receive(4))[0]
        if result != 0:
            reader.text()
            raise RuntimeError('RFB peer rejected the selected security type')

        # Shared ClientInit preserves any independent listener already attached.
        reader.send(b'\x01')
        header = reader.receive(20)
        width, height = struct.unpack_from('!HH', header)
        _geometry(width, height)
        name = reader.text()
        report['server_name'] = name[:256].decode('utf-8', errors='replace')
        report['initial_geometry'] = [width, height]
        report['initial_pixel_format'] = bytes(header[4:20]).hex()

        # Request little-endian B,G,R,padding words and only supported encodings.
        pixel_format = struct.pack('!BBBBHHHBBB3x', 32, 24, 0, 1, 255, 255, 255,
                                   16, 8, 0)
        reader.send(b'\x00\x00\x00\x00' + pixel_format)
        reader.send(struct.pack('!BBHiii', 2, 0, 3, RAW, DESKTOP_SIZE, LAST_RECT))
        pixels = bytearray(width * height * 3)
        covered = bytearray(width * height)
        remaining = width * height
        _request(reader, width, height)

        # Ignore bounded notifications, but never accept a partial framebuffer.
        for message in range(MAX_MESSAGES):
            message_type = reader.receive(1)[0]
            if message_type == 2:
                continue
            if message_type == 3:
                reader.receive(3)
                reader.text()
                continue
            if message_type != 0:
                raise ValueError(f'unsupported RFB server message: {message_type}')
            header = reader.receive(3)
            count = struct.unpack_from('!H', header, 1)[0]
            if count > MAX_RECTANGLES and count != 65535:
                raise ValueError('RFB update rectangle count exceeds its finite limit')
            report['framebuffer_updates'] += 1
            saw_last = False
            for rectangle in range(min(count, MAX_RECTANGLES)):
                header = reader.receive(12)
                x, y, rect_width, rect_height, encoding = struct.unpack('!HHHHi', header)
                if encoding == LAST_RECT:
                    report['last_rectangles'] += 1
                    saw_last = True
                    break
                if encoding == DESKTOP_SIZE:
                    _geometry(rect_width, rect_height)
                    width, height = rect_width, rect_height
                    pixels = bytearray(width * height * 3)
                    covered = bytearray(width * height)
                    remaining = width * height
                    report['desktop_resizes'] += 1
                    continue
                if encoding != RAW:
                    raise ValueError(f'unsupported RFB rectangle encoding: {encoding}')
                if (rect_width == 0 or rect_height == 0 or x + rect_width > width or
                        y + rect_height > height):
                    raise ValueError('RFB RAW rectangle exceeds the active framebuffer')
                report['rectangles'] += 1
                if report['rectangles'] > MAX_RECTANGLES:
                    raise ValueError('RFB capture rectangle count exceeds its finite limit')
                raw_bytes = rect_width * rect_height * 4
                if report['raw_bytes'] + raw_bytes > MAX_RAW_BYTES:
                    raise ValueError('RFB total RAW bytes exceed the finite transfer limit')
                data = reader.receive(raw_bytes)
                report['raw_bytes'] += len(data)

                # Convert only received pixels and track complete spatial coverage.
                row_bytes = rect_width * 4
                for row in range(rect_height):
                    reader.remaining()
                    start = row * row_bytes
                    source = data[start:start + row_bytes]
                    converted = bytearray(rect_width * 3)
                    converted[0::3] = source[2::4]
                    converted[1::3] = source[1::4]
                    converted[2::3] = source[0::4]
                    first = (y + row) * width + x
                    last = first + rect_width
                    pixels[first * 3:last * 3] = converted
                    remaining -= covered[first:last].count(0)
                    covered[first:last] = b'\x01' * rect_width
            if count == 65535 and not saw_last:
                raise ValueError('RFB indefinite update omitted bounded LastRect termination')
            if remaining == 0:
                report.update(width=width, height=height, pixels=width * height,
                              sha256=_save_ppm(output_ppm, width, height, pixels, reader))
                return report
            _request(reader, width, height)
    raise RuntimeError('RFB message limit reached before a complete framebuffer')

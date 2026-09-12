#!/usr/bin/env python3
"""Independent camera-ray/cuboid/nearest-texture oracle for WS014 p005.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

No rasterizer, shader code, recorded GPU image or guest checksum is used to
construct the expected pixels. Only silhouette/face edges within 0.30 pixel,
and texel boundaries within 0.02 texel, are excluded from exact RGB comparison.
"""
import hashlib
import math
from pathlib import Path
import re

WIDTH = 320
HEIGHT = 240
CLEAR = (16, 24, 40)
HALF = (0.75, 0.5, 0.375)
FIXED_TIMES = (0, 1000, 2500)


def read_ppm(path):
    path = Path(path)
    if path.stat().st_size > WIDTH * HEIGHT * 3 + 128:
        raise ValueError('PPM exceeds the bounded 320x240 capture')
    data = path.read_bytes()
    match = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', data)
    if not match or tuple(map(int, match.groups())) != (WIDTH, HEIGHT):
        raise ValueError('expected a 320x240 RGB8 binary PPM')
    pixels = data[match.end():]
    if len(pixels) != WIDTH * HEIGHT * 3:
        raise ValueError('PPM pixel payload length differs from its geometry')
    return pixels


def inverse_rotate(vector, time_ms):
    seconds = time_ms / 1000
    ax, ay = 0.30 + 0.43 * seconds, 0.40 + 0.70 * seconds
    sx, cx, sy, cy = math.sin(ax), math.cos(ax), math.sin(ay), math.cos(ay)
    x, y, z = vector
    # Undo Y and then X, taking a camera-space ray into the stationary box.
    x, z = cy * x - sy * z, sy * x + cy * z
    return x, cx * y + sx * z, -sx * y + cx * z


class Scene:
    def __init__(self, time_ms):
        if not isinstance(time_ms, int) or not 0 <= time_ms <= 3600000:
            raise ValueError('sample time must be an integer in 0..3600000 ms')
        self.time_ms = time_ms
        self.origin = inverse_rotate((0, 0, -3), time_ms)
        self.ray_x = inverse_rotate((1 / 192, 0, 0), time_ms)
        self.ray_y = inverse_rotate((0, -1 / 192, 0), time_ms)
        self.ray_z = inverse_rotate((0, 0, 1), time_ms)

    def hit(self, px, py):
        direction = tuple(self.ray_x[i] * (px - WIDTH / 2) +
                          self.ray_y[i] * (py - HEIGHT / 2) + self.ray_z[i]
                          for i in range(3))
        near, far, face = -math.inf, math.inf, None
        for axis, (origin, step, extent) in enumerate(zip(self.origin, direction, HALF)):
            if abs(step) < 1e-14:
                if abs(origin) > extent:
                    return None
                continue
            first, last = (-extent - origin) / step, (extent - origin) / step
            sign = -1
            if first > last:
                first, last, sign = last, first, 1
            if first > near:
                near, face = first, (axis, sign)
            far = min(far, last)
            if near > far:
                return None
        if near <= 0 or face is None:
            return None
        x, y, z = (self.origin[i] + near * direction[i] for i in range(3))
        axis, sign = face
        if axis == 0:
            u, v = ((0.375 - z) if sign > 0 else (z + 0.375)) / 0.75, 0.5 - y
        elif axis == 1:
            u, v = (x + 0.75) / 1.5, ((z + 0.375) if sign > 0 else (0.375 - z)) / 0.75
        else:
            u, v = ((x + 0.75) if sign > 0 else (0.75 - x)) / 1.5, 0.5 - y
        return face, u, v

    def pixel(self, x, y):
        hit = self.hit(x + 0.5, y + 0.5)
        return CLEAR if hit is None else texture(hit[1], hit[2])


def texture(u, v):
    x = min(63, max(0, math.floor(u * 64)))
    y = min(63, max(0, math.floor(v * 64)))
    return (32 if (x // 8 + y // 8) % 2 == 0 else 224, 32 + 3 * x, 32 + 3 * y)


def reference(time_ms):
    """Produce a mathematical fixture image; never accept it as GPU evidence."""
    scene = Scene(time_ms)
    return bytes(component for y in range(HEIGHT) for x in range(WIDTH)
                 for component in scene.pixel(x, y))


def verify_pixels(pixels, time_ms):
    if len(pixels) != WIDTH * HEIGHT * 3:
        raise ValueError('RGB payload length is not 320x240x3')
    scene = Scene(time_ms)
    checked = excluded = foreground = mismatch = 0
    faces, colors, examples = {}, set(), []
    for y in range(HEIGHT):
        for x in range(WIDTH):
            px, py = x + 0.5, y + 0.5
            hit = scene.hit(px, py)
            face = hit[0] if hit else None
            edge = False
            # Restrict geometric tolerance to a subpixel band around real edges.
            for dx, dy in ((-0.30, 0), (0.30, 0), (0, -0.30), (0, 0.30)):
                nearby = scene.hit(px + dx, py + dy)
                if (nearby[0] if nearby else None) != face:
                    edge = True
                    break
            if hit is not None:
                foreground += 1
                faces[str(face)] = faces.get(str(face), 0) + 1
                texel_edge = any(abs(value * 64 - round(value * 64)) < 0.02
                                 for value in hit[1:])
                edge = edge or texel_edge
                expected = texture(hit[1], hit[2])
            else:
                expected = CLEAR
            if edge:
                excluded += 1
                continue
            checked += 1
            index = (y * WIDTH + x) * 3
            actual = tuple(pixels[index:index + 3])
            if hit:
                colors.add(actual)
            if actual != expected:
                mismatch += 1
                if len(examples) < 12:
                    examples.append({'x': x, 'y': y, 'expected': expected, 'actual': actual})
    if excluded > WIDTH * HEIGHT // 20:
        raise ValueError('oracle excluded more than the maximum 5% boundary budget')
    passed = mismatch == 0 and foreground >= 2500 and len(faces) >= 2 and len(colors) >= 128
    return {'passed': passed, 'time_ms': time_ms, 'width': WIDTH, 'height': HEIGHT,
            'rgb_sha256': hashlib.sha256(pixels).hexdigest(), 'checked_pixels': checked,
            'excluded_boundary_pixels': excluded, 'foreground_pixels': foreground,
            'visible_faces': faces, 'checked_foreground_colors': len(colors),
            'mismatch_pixels': mismatch, 'mismatch_examples': examples,
            'geometry_margin_pixels': 0.30, 'texel_margin': 0.02,
            'maximum_excluded_fraction': 0.05}


def verify_sequence(samples):
    """Enforce fixed and real-time progression independently of a frame's content."""
    if len(samples) != 6:
        raise ValueError('verification requires exactly six frames in one process')
    token = samples[0]['run']
    for index, sample in enumerate(samples):
        mode = 'fixed' if index < 3 else 'live'
        if (sample['run'] != token or sample['mode'] != mode or
                sample['sample'] != index % 3 + 1 or sample['frame'] != index + 1 or
                (sample['width'], sample['height']) != (WIDTH, HEIGHT)):
            raise ValueError('frame identity/order differs from the six-frame session')
        if not re.fullmatch(r'[0-9a-f]{64}', sample['rgb_sha256']):
            raise ValueError('invalid GPU readback RGB SHA256')
        if index < 3 and sample['time_ms'] != FIXED_TIMES[index]:
            raise ValueError('fixed sample time differs from the independent schedule')
    live_times = [sample['time_ms'] for sample in samples[3:]]
    if not 0 <= live_times[0] <= 1000:
        raise ValueError('live clock did not start at its own monotonic epoch')
    if any(b - a < 200 for a, b in zip(live_times, live_times[1:])):
        raise ValueError('monotonic animation did not advance between checkpoints')
    for start in (0, 3):
        if len({sample['rgb_sha256'] for sample in samples[start:start + 3]}) != 3:
            raise ValueError('unchanged/old frames cannot prove rotation')
    return {'frames': 6, 'fixed_times_ms': list(FIXED_TIMES), 'live_times_ms': live_times,
            'same_process': True, 'distinct_frames_per_mode': 3}

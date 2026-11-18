#!/usr/bin/env python3
"""Compile the compiler test's shaders and generate what the kernel test embeds: the SPIR-V, the
per-cell inputs and the bounds every output must fall in."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: python3 src/drivers/gpu/i915/tests/render/compiler-shaders/regenerate.py
#
# Writes <name>.spv next to each GLSL source (read by the host fixtures i915-vk-lower-test.c
# and i915-vk-compile-test.c) and tests/fixtures/compiler-shaders-gen.inc (included by tests/render/compiler.c).
# The geometry shaders and the hand-written assembly are only written as <name>.spv, for the host
# fixtures (i915-vk-spirv-test.c); the kernel test does not embed them.
#
# The target is 64 x 64 pixels of R32G32B32A32_SFLOAT.  A "cell" test draws 16 x 16 cells of 4 x 4 pixels, each a
# quad whose vertices all carry the same attributes, so every pixel of a cell receives them exactly; a "pixel" test
# draws one quad over the whole target whose value is the pixel position (0 .. 64), so v.xy is the pixel centre.
# The expected outputs are computed here in double precision and rounded to float; each is given as an interval
# [lo, hi] of ordered keys (the float bits mapped so that integer order is float order), wide enough for the
# precision Vulkan requires of the function: exact for abs, floor, min, max, clamp, the comparisons and selections
# and for mix (whose x * (1 - a) + y * a is emulated operation by operation), 1 ULP for fract, 2 ULP for
# inversesqrt, 2.5 (3) ULP for division, 3 + 2|x| ULP for exp2, 3 ULP or 2^-21 absolute in [0.5, 2] for log2, pow as
# exp2(y * log2(x)), sqrt as 1 / inversesqrt, normalize with the rounding of its dot product.
import hashlib
import math
import pathlib
import struct
import subprocess

# Source, stage, C array.
SHADERS = (
    ('cells.vert', 'vertex', 'i915_vkc_cells_vert'),
    ('vsmath.vert', 'vertex', 'i915_vkc_vsmath_vert'),
    ('passthrough.frag', 'fragment', 'i915_vkc_passthrough_frag'),
    ('unary.frag', 'fragment', 'i915_vkc_unary_frag'),
    ('exponent.frag', 'fragment', 'i915_vkc_exponent_frag'),
    ('minmax.frag', 'fragment', 'i915_vkc_minmax_frag'),
    ('divide.frag', 'fragment', 'i915_vkc_divide_frag'),
    ('compare.frag', 'fragment', 'i915_vkc_compare_frag'),
    ('branch.frag', 'fragment', 'i915_vkc_branch_frag'),
    ('discard.frag', 'fragment', 'i915_vkc_discard_frag'),
    ('shade.frag', 'fragment', 'i915_vkc_shade_frag'),
)

# The shaders of the geometry stage the host fixtures parse and compile: glxtest's three geometry
# shaders, the varyings test, the ones the parser refuses, a fragment shader reading gl_PrimitiveID, and the vertex
# shader varyings.geom reads (the three-stage pipelines of i915-vk-pipe-test.c).  Source, stage.
GEOMETRY = (
    ('points.geom', 'geometry'),
    ('adjacency.geom', 'geometry'),
    ('layers.geom', 'geometry'),
    ('varyings.geom', 'geometry'),
    ('noemit.geom', 'geometry'),
    ('overflow.geom', 'geometry'),
    ('emitif.geom', 'geometry'),
    ('cut64.geom', 'geometry'),
    ('cut160.geom', 'geometry'),
    ('spill.geom', 'geometry'),
    ('refuse-entry.geom', 'geometry'),
    ('refuse-invocations.geom', 'geometry'),
    ('refuse-invocation-id.geom', 'geometry'),
    ('refuse-viewport.geom', 'geometry'),
    ('refuse-clip-distance.geom', 'geometry'),
    ('primitive-id.frag', 'fragment'),
    ('varyings.vert', 'vertex'),
)

# SPIR-V written by hand, for what no GLSL compiler emits: assembled with spirv-as.
ASSEMBLED = (
    'refuse-length.spvasm',
)

# mview's shaders as the application ships them (userland/tests/mview/shaders/), embedded unchanged.
SHIPPED = (
    ('mview.vert.spv', 'i915_vkc_mview_vert'),
)

CELLS = 256
SIZE = 64
# The clear value every pixel holds before a draw: a discarded pixel keeps it.
CLEAR = -7.0


def run(arguments):
    """Runs one tool and returns what it printed."""
    return subprocess.run(arguments, check=True, timeout=30, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True).stdout.strip()


def f32(x):
    """Rounds a double to the nearest float."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    """The bits of a float."""
    return struct.unpack('<I', struct.pack('<f', x))[0]


def key(b):
    """The ordered key of float bits: integer order is float order (-0 just below +0)."""
    return (b ^ 0xFFFFFFFF) if (b & 0x80000000) else (b | 0x80000000)


def exact(x):
    k = key(bits(f32(x)))
    return (k, k)


def ulps(x, n):
    k = key(bits(f32(x)))
    n = int(math.ceil(n))
    return (k - n, k + n)


def absolute(x, e):
    return (key(bits(f32(x - e))) - 1, key(bits(f32(x + e))) + 1)


def span(lo, hi, n):
    """[lo, hi] widened by n ULP on each side."""
    return (key(bits(f32(lo))) - n, key(bits(f32(hi))) + n)


# ---------------------------------------------------------------------------- inputs of the cell tests

def math_inputs():
    """x in [-8, 8] with exact integers, y in [1/32, 300] (> 0), z in [-10, 10] (never 0), w in [-3, 3]."""
    cells = []
    for i in range(CELLS):
        x = (i - 128) / 16.0
        if i % 16 != 0:
            x += ((i * 37) % 11) * 0.013
        y = 2.0 ** ((i % 40) / 4.0 - 5.0) * (1.0 + ((i * 13) % 7) * 0.07)
        z = ((i * 29) % 81) / 4.0 - 10.0
        if z == 0.0:
            z = 0.375
        w = ((i * 17) % 49) / 8.0 - 3.0
        cells.append([f32(x), f32(y), f32(z), f32(w)])
    return cells


def compare_inputs():
    """Ordered, equal, signed-zero and NaN pairs; when v.y is NaN, v.x >= v.z so ?: never picks it."""
    nan = float('nan')
    pairs = [(1.0, 2.0), (2.0, 1.0), (3.0, 3.0), (-4.0, 7.0), (0.0, -0.0), (-0.0, 0.0), (5.5, -5.5),
             (1e-3, 1e-3), (nan, 1.0), (1.0, nan), (nan, nan), (-2.5, -2.5)]
    zw = [(5.0, 3.0), (3.0, 5.0), (-1.0, -1.0), (0.5, 0.25)]
    cells = []
    for i in range(CELLS):
        x, y = pairs[i % len(pairs)]
        z, w = zw[(i // len(pairs)) % len(zw)]
        if y != y:
            z = -100.0
        cells.append([f32(x), f32(y), f32(z), f32(w)])
    return cells


def vsmath_inputs():
    cells = []
    for i in range(CELLS):
        x = ((i * 7) % 23) / 4.0 - 2.75
        y = ((i * 11) % 19) / 3.0 - 3.0
        z = ((i * 5) % 17) / 2.0 - 4.0
        if x == 0.0 and y == 0.0 and z == 0.0:
            z = 1.0
        w = ((i * 3) % 21) / 10.0 - 0.5
        cells.append([f32(x), f32(y), f32(z), f32(w)])
    return cells


def mview_inputs():
    """Per cell: the normal (x, y, z) and the texture position (u, v)."""
    cells = []
    for i in range(CELLS):
        nx = ((i * 7) % 13) / 6.0 - 1.0
        ny = ((i * 5) % 11) / 5.0 - 1.0
        nz = ((i * 3) % 9) / 4.0 - 1.0
        if nx == 0.0 and ny == 0.0 and nz == 0.0:
            nz = 1.0
        u = (i % 16) / 16.0
        v = (i // 16) / 16.0
        cells.append([f32(nx), f32(ny), f32(nz), f32(u), f32(v)])
    return cells


# ---------------------------------------------------------------------------- references

def log2_bounds(y):
    exact_value = math.log2(y)
    if 0.5 <= y <= 2.0:
        return absolute(exact_value, 2.0 ** -21)
    return ulps(exact_value, 3)


def pow_bounds(y, w):
    """exp2(w * log2(y)): the log2 error times |w|, the rounding of the product, then exp2's 3 + 2|p| ULP."""
    logarithm = math.log2(y)
    if 0.5 <= y <= 2.0:
        log_error = 2.0 ** -21
    else:
        log_error = 3.0 * abs(logarithm) * 2.0 ** -23
    p = w * logarithm
    p_error = abs(w) * log_error + abs(p) * 2.0 ** -24
    return span(2.0 ** (p - p_error), 2.0 ** (p + p_error), int(math.ceil(3 + 2 * abs(p))) + 1)


def mix_exact(x, y, a):
    complement = f32(1.0 - a)
    left = f32(x * complement)
    right = f32(y * a)
    return f32(left + right)


def unary_reference(v):
    x, y = v[0], v[1]
    return [exact(abs(x)), exact(math.floor(x)), ulps(f32(x - math.floor(x)), 1), ulps(math.sqrt(y), 4)]


def exponent_reference(v):
    x, y, z, w = v
    return [ulps(2.0 ** z, 3 + 2 * abs(z)), log2_bounds(y), pow_bounds(y, w), ulps(1.0 / math.sqrt(y), 2)]


def minmax_reference(v):
    x, y, z, w = v
    return [exact(x if x < z else z), exact(x if x >= z else z),
            exact(min(max(x, -0.5), 0.75)), exact(mix_exact(x, z, w))]


def divide_reference(v):
    x, y, z, w = v
    length = math.sqrt(x * x + y * y + z * z)
    return [ulps(x / y, 3), ulps(1.0 / z, 2), ulps(x / length, 8), ulps(z / length, 8)]


def compare_reference(v):
    x, y, z, w = v
    order = float(x < y) + 2.0 * float(x > y) + 4.0 * float(x <= y) + 8.0 * float(x >= y)
    equality = (float(x == y) + 2.0 * float(x != y) + 4.0 * float(x < y and z > w) +
                8.0 * float(x < y or z > w))
    picked = y if x < z else w
    negated = float(not (x < y)) + 2.0 * (w if z < w else z)
    return [exact(order), exact(equality), exact(picked), exact(negated)]


LIGHT = (f32(0.267261), f32(0.534522), f32(0.801784))


def lambert_bounds(n, error):
    """max(dot(n, light), 0) within an absolute error of the dot product."""
    d = n[0] * LIGHT[0] + n[1] * LIGHT[1] + n[2] * LIGHT[2]
    return span(max(d - error, 0.0), max(d + error, 0.0), 1)


def vsmath_reference(v):
    x, y, z, w = v
    length = math.sqrt(x * x + y * y + z * z)
    n = (x / length, y / length, z / length)
    return [ulps(n[0], 8), ulps(n[1], 8), lambert_bounds(n, 2e-6), exact(min(max(w, 0.0), 1.0))]


# mview's push constants: clip columns that place (x, y, 0.5) at itself, normal columns that turn by 90 degrees.
MVIEW_PUSH = [
    1.0, 0.0, 0.0, 0.0,   0.0, 1.0, 0.0, 0.0,   0.0, 0.0, 1.0, 0.0,   0.0, 0.0, 0.0, 1.0,
    0.0, 1.0, 0.0, 0.0,   -1.0, 0.0, 0.0, 0.0,   0.0, 0.0, 1.0, 0.0,   1.0, 1.0, 1.0, 1.0,
]


def mview_reference(c):
    nx, ny, nz, u, v = c
    turned = [nx * MVIEW_PUSH[16 + k] + ny * MVIEW_PUSH[20 + k] + nz * MVIEW_PUSH[24 + k] for k in range(3)]
    length = math.sqrt(sum(t * t for t in turned))
    n = [t / length for t in turned]
    d = sum(n[k] * LIGHT[k] for k in range(3))
    lo = f32(0.35) + f32(0.65) * max(d - 2e-6, 0.0)
    hi = f32(0.35) + f32(0.65) * max(d + 2e-6, 0.0)
    return [exact(u), exact(v), span(lo, hi, 4), exact(1.0)]


# ---------------------------------------------------------------------------- the pixel tests

def branch_color(x, y):
    c = [0.0, 0.0, 0.0, 1.0]
    t = 1.0
    if x < 21.0:
        if y < 11.0:
            c[0] = 1.0
        else:
            c[1] = 1.0
            t = t * 2.0
    elif y < 33.0 and x > 41.0:
        c[2] = 1.0
        t = 3.0
    else:
        c = [0.25, 0.25, 0.25, 0.25]
    s = t if x < y + 0.5 else -t
    return (c[0], c[1], c[2], c[3] + s)


def discard_color(x, y):
    cell = math.floor(x * 0.125) + math.floor(y * 0.125)
    if (cell * 0.5 - math.floor(cell * 0.5)) > 0.25:
        return None
    c = [0.5, 0.25, 1.0, 1.0]
    if y < 32.0:
        if (x * 0.5 - math.floor(x * 0.5)) < 0.5 and x > 16.0:
            return None
        c[0] = 1.0
    return tuple(c)


def classes(color):
    """Per pixel, the index of its colour in a table; class 0 is the clear colour (a discarded pixel)."""
    table = [(CLEAR, CLEAR, CLEAR, CLEAR)]
    index = []
    for py in range(SIZE):
        for px in range(SIZE):
            value = color(px + 0.5, py + 0.5)
            if value is None:
                value = table[0]
            if value not in table:
                table.append(value)
            index.append(table.index(value))
    return table, index


# ---------------------------------------------------------------------------- output

def c_words(lines, symbol, words, kind='uint32_t', per_line=6, fmt='0x{:08x}U'):
    lines.append(f'static const {kind} {symbol}[{len(words)}] = {{')
    for offset in range(0, len(words), per_line):
        lines.append('\t' + ', '.join(fmt.format(word) for word in words[offset:offset + per_line]) + ',')
    lines.append('};')


def main():
    directory = pathlib.Path(__file__).resolve().parent
    fixtures = directory.parent.parent / 'fixtures'
    version = run(['glslc', '--version']).splitlines()[0]
    lines = [
        '/*',
        ' * zedBSD',
        ' * Copyright (C) 2026 Awe Morris',
        ' *',
        ' * SPDX-License-Identifier: Zlib',
        ' */',
        '',
        '/*',
        ' * GENERATED FILE - the SPIR-V, the inputs and the expected output bounds of the compiler test.',
        ' * Do not edit by hand.',
        ' *',
        ' * Generator : src/drivers/gpu/i915/tests/render/compiler-shaders/regenerate.py',
        f' * Compiler  : {version} (--target-env=vulkan1.1 --target-spv=spv1.0 -O0)',
        ' *',
        ' * A bound is a pair of ordered keys [lo, hi]: the float bits b mapped to b | 0x80000000 when',
        ' * the sign is clear and to ~b when it is set, so that integer order is float order.',
        ' */',
    ]
    for source, stage, symbol in SHADERS:
        path = directory / source
        output = directory / (source + '.spv')
        run(['glslc', '--target-env=vulkan1.1', '--target-spv=spv1.0', f'-fshader-stage={stage}',
             '-O0', str(path), '-o', str(output)])
        run(['spirv-val', '--target-env', 'vulkan1.1', str(output)])
        binary = output.read_bytes()
        words = struct.unpack(f'<{len(binary) // 4}I', binary)
        lines.extend(['', f'/* {source} (sha256 {hashlib.sha256(path.read_bytes()).hexdigest()}), {len(words)} words. */'])
        c_words(lines, symbol, words)

    for source, stage in GEOMETRY:
        output = directory / (source + '.spv')
        run(['glslc', '--target-env=vulkan1.1', '--target-spv=spv1.0', f'-fshader-stage={stage}',
             '-O0', str(directory / source), '-o', str(output)])
        run(['spirv-val', '--target-env', 'vulkan1.1', str(output)])

    for source in ASSEMBLED:
        output = directory / (source.rsplit('.', 1)[0] + '.spv')
        run(['spirv-as', '--target-env', 'vulkan1.0', str(directory / source), '-o', str(output)])
        run(['spirv-val', '--target-env', 'vulkan1.1', str(output)])

    shipped = directory.parents[6] / 'userland' / 'tests' / 'mview' / 'shaders'
    for source, symbol in SHIPPED:
        binary = (shipped / source).read_bytes()
        words = struct.unpack(f'<{len(binary) // 4}I', binary)
        lines.extend(['', f'/* userland/tests/mview/shaders/{source} as shipped (sha256 {hashlib.sha256(binary).hexdigest()}), {len(words)} words. */'])
        c_words(lines, symbol, words)

    # The NDC coordinate of cell edge k (0 .. 16): -1 + k / 8.
    lines.extend(['', '/* The NDC coordinate of cell edge k: -1 + k / 8. */'])
    c_words(lines, 'i915_vkc_ndc', [bits(f32(-1.0 + k / 8.0)) for k in range(17)])
    lines.extend(['', '/* The clear value of every component of every pixel. */',
                  f'#define I915_VKC_CLEAR_BITS 0x{bits(CLEAR):08x}U'])
    lines.extend(['', '/* mview\'s push constants: clip columns that keep (x, y, 0.5, 1), normal columns turning by 90 degrees. */'])
    c_words(lines, 'i915_vkc_mview_push', [bits(f32(value)) for value in MVIEW_PUSH])

    tests = (
        ('math', math_inputs(), (('unary', unary_reference), ('exponent', exponent_reference),
                                 ('minmax', minmax_reference), ('divide', divide_reference))),
        ('compare', compare_inputs(), (('compare', compare_reference),)),
        ('vsmath', vsmath_inputs(), (('vsmath', vsmath_reference),)),
        ('mview', mview_inputs(), (('mview', mview_reference),)),
    )
    for name, cells, references in tests:
        width = len(cells[0])
        lines.extend(['', f'/* The attributes of each cell of the {name} inputs, {width} floats to a cell. */'])
        c_words(lines, f'i915_vkc_{name}_attributes', [bits(value) for cell in cells for value in cell],
                per_line=width if width > 4 else 4)
        for test, reference in references:
            bounds = []
            for cell in cells:
                # The fragment shader sees an attribute after the interpolation, value + 0 * b1 + 0 * b2:
                # a -0 arrives as +0 (the vertex stage reads it as it is, and passes the sign on only
                # through arithmetic that keeps it).
                seen = [value + 0.0 for value in cell] if reference is not vsmath_reference else cell
                for lo, hi in reference(seen):
                    assert lo <= hi
                    bounds.extend([lo & 0xFFFFFFFF, hi & 0xFFFFFFFF])
            lines.extend(['', f'/* {test}: [lo, hi] of each of the four outputs of each cell. */'])
            c_words(lines, f'i915_vkc_{test}_bounds', bounds, per_line=8)

    for test, color in (('branch', branch_color), ('discard', discard_color)):
        table, index = classes(color)
        lines.extend(['', f'/* {test}: the colours a pixel can have (class 0: the clear colour), then the class of each pixel. */'])
        c_words(lines, f'i915_vkc_{test}_colors', [bits(f32(value)) for entry in table for value in entry], per_line=4)
        c_words(lines, f'i915_vkc_{test}_classes', index, kind='uint8_t', per_line=32, fmt='{}')
    lines.append('')
    (fixtures / 'compiler-shaders-gen.inc').write_text('\n'.join(lines))


if __name__ == '__main__':
    main()

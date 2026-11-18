#!/usr/bin/env python3
"""Compile the generality test's shaders and generate what the kernel test and the host fixtures embed: the
SPIR-V, the uniform and push-constant blocks, the vertex data and the pixels every step expects."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: python3 src/drivers/gpu/i915/tests/render/generality-shaders/regenerate.py
#
# Writes <name>.spv next to each GLSL source and tests/fixtures/generality-shaders-gen.inc (included by
# tests/render/generality.c, the scenario "vke2", and by the host fixtures i915-vk-lower-test.c and
# i915-vk-compile-test.c).  The file holds arrays and macros only.
#
# The target is 64 x 64 pixels of R8G8B8A8_UNORM, and every fragment shader writes the four bytes of one
# 32-bit word per pixel (red the low byte): an integer result, or the bits of a float.  The expected words
# are computed here: the integers modulo 2^32 with the SPIR-V definitions (OpSDiv rounds toward zero,
# OpSMod takes the divisor's sign), the floats as IEEE-754 single precision operations in the order the
# executor's compiler lowers them (every value is exact or a correctly rounded single), with the inputs
# chosen so that no result depends on the precision of the hardware's reciprocal.
import fractions
import hashlib
import math
import pathlib
import re
import struct
import subprocess

# Source, stage, C array.
SHADERS = (
    ('quad.vert', 'vertex', 'i915_vke2_quad_vert'),
    ('matrix.vert', 'vertex', 'i915_vke2_matrix_vert'),
    ('matrix.frag', 'fragment', 'i915_vke2_matrix_frag'),
    ('int.frag', 'fragment', 'i915_vke2_int_frag'),
    ('float.frag', 'fragment', 'i915_vke2_float_frag'),
    ('loop.frag', 'fragment', 'i915_vke2_loop_frag'),
    ('vary16.vert', 'vertex', 'i915_vke2_vary16_vert'),
    ('vary16.frag', 'fragment', 'i915_vke2_vary16_frag'),
    ('subset.frag', 'fragment', 'i915_vke2_subset_frag'),
    ('vin16.vert', 'vertex', 'i915_vke2_vin16_vert'),
    ('vin16.frag', 'fragment', 'i915_vke2_vin16_frag'),
    ('spill.frag', 'fragment', 'i915_vke2_spill_frag'),
    ('vio16.vert', 'vertex', 'i915_vke2_vio16_vert'),
    ('agg.frag', 'fragment', 'i915_vke2_agg_frag'),
    ('matfn.frag', 'fragment', 'i915_vke2_matfn_frag'),
    ('coord.frag', 'fragment', 'i915_vke2_coord_frag'),
    ('deriv.frag', 'fragment', 'i915_vke2_deriv_frag'),
    ('nopersp.vert', 'vertex', 'i915_vke2_nopersp_vert'),
    ('nopersp.frag', 'fragment', 'i915_vke2_nopersp_frag'),
    ('persp.frag', 'fragment', 'i915_vke2_persp_frag'),
    ('point.vert', 'vertex', 'i915_vke2_point_vert'),
    ('point.frag', 'fragment', 'i915_vke2_point_frag'),
    ('vformat.vert', 'vertex', 'i915_vke2_vformat_vert'),
    ('vformat.frag', 'fragment', 'i915_vke2_vformat_frag'),
    ('edge.frag', 'fragment', 'i915_vke2_edge_frag'),
    ('undef.frag', 'fragment', 'i915_vke2_undef_frag'),
    ('killoop.frag', 'fragment', 'i915_vke2_killoop_frag'),
    ('noinput.vert', 'vertex', 'i915_vke2_noinput_vert'),
    ('noinput.frag', 'fragment', 'i915_vke2_noinput_frag'),
    ('int16.frag', 'fragment', 'i915_vke2_int16_frag'),
)

SIZE = 64


def run(arguments):
    """Runs one tool and returns what it printed."""
    return subprocess.run(arguments, check=True, timeout=60, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True).stdout.strip()


def f32(x):
    """Rounds a double to the nearest float (ties to even)."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    """The bits of a float."""
    return struct.unpack('<I', struct.pack('<f', x))[0]


def u32(v):
    return v & 0xFFFFFFFF


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def floor_f(x):
    """RNDD: a float rounded down; a zero keeps its sign."""
    if x == 0.0:
        return x
    return float(math.floor(x))


def trunc_f(x):
    """RNDZ: a float rounded toward zero; a zero keeps its sign."""
    if x == 0.0:
        return x
    value = float(math.trunc(x))
    return value if value != 0.0 else math.copysign(0.0, x)


def round_even_f(x):
    """RNDE: a float rounded to the nearest integer, ties to even."""
    value = float(round(x))
    return value if value != 0.0 else math.copysign(0.0, x)


# ------------------------------------------------------------------ matrices

# A matrix is a list of columns; M[c][r] is column c, row r.
MAT_M = [[1.0, 0.5, -1.0, 2.0], [0.0, 1.5, 1.0, -0.5], [2.0, -1.0, 0.25, 1.0], [-0.5, 0.0, 1.0, 1.0]]
MAT_R = [[0.5, 1.0, 0.0, -1.0], [1.0, -0.5, 2.0, 0.0], [0.0, 1.0, 1.0, 0.5], [1.5, 0.0, -1.0, 1.0]]
MAT_M3 = [[1.0, -2.0, 0.5], [0.25, 1.0, 3.0], [-1.0, 0.5, 2.0]]
EXTRA = [0.5, -1.25, 2.0, 7.0]
PUSH_P = [[1.0, 0.0, 2.0, -1.0], [0.5, 1.0, 0.0, 1.0], [-2.0, 0.25, 1.0, 0.0], [0.0, 1.0, -1.0, 3.0]]
PUSH_Q = [[2.0, -1.0, 0.0, 1.0], [1.0, 0.5, -2.0, 0.0], [0.0, 3.0, 1.0, -1.0], [-1.5, 0.0, 0.5, 2.0]]

# The matrix step's placement: turn is the transpose of a quarter turn (x, y) -> (-y, x), scale doubles x and y.
QUARTER = [[0.0, 1.0, 0.0, 0.0], [-1.0, 0.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]]
TURN = [[QUARTER[r][c] for r in range(4)] for c in range(4)]
SCALE = [[2.0, 0.0, 0.0, 0.0], [0.0, 2.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]]


def column_major_words(matrix, stride_floats=4):
    """A matrix as std140 / std430 column-major memory: column c at c * stride."""
    words = []
    for column in matrix:
        words.extend(column + [0.0] * (stride_floats - len(column)))
    return words


def row_major_words(matrix):
    """A 4 x 4 matrix as row-major memory: row r at r * 16 bytes."""
    return [matrix[c][r] for r in range(4) for c in range(4)]


def mat_mul(left, right):
    """left * right as the compiler lowers it: each sum starts at the first product and adds the others in order."""
    rows = len(left[0])
    inner = len(left)
    result = []
    for column in right:
        out = []
        for r in range(rows):
            total = None
            for k in range(inner):
                product = f32(left[k][r] * column[k])
                total = product if total is None else f32(total + product)
            out.append(total)
        result.append(out)
    return result


def mat_vec(matrix, vector):
    return mat_mul(matrix, [list(vector)])[0]


def vec_mat(vector, matrix):
    out = []
    for column in matrix:
        total = None
        for k in range(len(vector)):
            product = f32(vector[k] * column[k])
            total = product if total is None else f32(total + product)
        out.append(total)
    return out


def transpose(matrix):
    return [[matrix[c][r] for c in range(len(matrix))] for r in range(len(matrix[0]))]


def dot(a, b):
    total = None
    for k in range(len(a)):
        product = f32(a[k] * b[k])
        total = product if total is None else f32(total + product)
    return total


def matrix_pixel(x, y):
    v = [f32(float(x & 7) - 3.0), f32(float(y & 3) * 0.5), float((x >> 4) & 3), f32(1.0 + float(y >> 3))]
    l = mat_mul(MAT_M, MAT_R)
    l[2] = [f32(l[2][k] + v[k]) for k in range(4)]
    if (y & 8) == 8:
        l = transpose(l)
    a = mat_vec(l, v)
    b = vec_mat(v, PUSH_P)
    c = mat_vec([[f32(value * 0.5) for value in column] for column in PUSH_Q], v)
    o = [[f32(v[r] * a[col]) for r in range(3)] for col in range(4)]
    d = mat_vec(o, [1.0, -2.0, 0.5, 3.0])
    e = mat_vec(MAT_M3, [v[2], v[1], v[0]])
    e = [f32(e[k] + EXTRA[k]) for k in range(3)]
    g = mat_vec(transpose(MAT_M3), v[:3])
    n2 = [[v[0], v[1]], [a[2], a[3]]]
    w2 = mat_vec(n2, [2.0, -1.0])
    if (y & 4) == 0:
        groups = [a, b, c, d + [e[0]]]
    else:
        groups = [[e[1], e[2], g[0], g[1]], [g[2], w2[0], w2[1], l[0][0]],
                  [l[1][1], l[3][2], dot(a, b), o[3][0]],
                  [o[3][1], o[3][2], mat_vec(PUSH_P, v)[0], mat_vec(MAT_R, v)[3]]]
    k = x & 15
    return bits(groups[k >> 2][k & 3])


# ------------------------------------------------------------------ integers

def int_operands(x, y):
    variant = y >> 4
    h = u32(x * 2654435761 + y * 2246822519)
    h ^= h >> 15
    h = u32(h * 668265263)
    h ^= h >> 13
    a = s32(h)
    if variant == 1:
        a = a >> 20
    elif variant == 2:
        a = (a & 255) - 128
    elif variant == 3:
        a = s32(a | 0x80000000)
    k = x & 31
    b = 1 << k if k < 16 else (k - 16) * 37 + 3
    if (x >> 5) & 1:
        b = -b
    if variant == 0 and (x & 3) == 3:
        b = s32(u32(h * 2654435761) | 1)
    if (x & 7) == 5 and a != 0:
        b = a
    return a, b


def sdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def int_pixel(x, y):
    a, b = int_operands(x, y)
    assert b != 0
    assert not (a == -2 ** 31 and b == -1)
    assert a != -2 ** 31 and b != -2 ** 31
    op = y & 15
    ua, ub = u32(a), u32(b)
    if op == 0:
        r = a + b
    elif op == 1:
        r = a - b
    elif op == 2:
        r = a * b
    elif op == 3:
        r = sdiv(a, b)
    elif op == 4:
        r = a % b
    elif op == 5:
        r = ua // ub
    elif op == 6:
        r = ua % ub
    elif op == 7:
        r = s32(a << (x & 31)) ^ (a >> (y & 31)) ^ s32(ua >> ((x + y) & 31))
    elif op == 8:
        r = ((a & b) ^ (a | ~b)) + (a ^ b)
    elif op == 9:
        sign = (a > 0) - (a < 0)
        r = s32(-a) ^ abs(b) ^ (sign * 7) ^ s32(min(a, b) * 3) ^ max(a, b) ^ min(max(a, -1000), 1000 + x)
    elif op == 10:
        low, high = min(ub, 1000), max(ub, 1000)
        r = min(ua, ub) ^ (max(ua, ub) >> 1) ^ min(max(ua, low), high)
    elif op == 11:
        r = (int(a == b) | int(a != b) << 1 | int(a < b) << 2 | int(a <= b) << 3 | int(a > b) << 4 |
             int(a >= b) << 5 | int(ua < ub) << 6 | int(ua <= ub) << 7 | int(ua > ub) << 8 | int(ua >= ub) << 9)
    elif op == 12:
        r = bits(f32(float(ua)))
    elif op == 13:
        r = bits(f32(float(a)))
    elif op == 14:
        f = f32(f32(float(a >> 7)) * f32(0.37))
        r = int(f) * 3 + (int(abs(f)) >> 1)
    else:
        r = (ua >> 3) - (a >> 3) + s32(-a) * (b | 1)
    return u32(r)


# ------------------------------------------------------------------ floats

def float_pixel(x, y):
    op = y & 7
    fx = f32(f32(f32(float(x) * f32(0.53)) + f32(0.11)) - 11.0)
    fy = f32(f32(float(y >> 3) * f32(0.53)) + f32(0.73))
    h = f32(float(x - 32) * 0.5)
    if op in (0, 1):
        divisor = fy if op == 0 else -fy
        quotient = fx / divisor
        fraction = quotient - math.floor(quotient)
        assert 0.005 < fraction < 0.995, (x, y, quotient)
        r = f32(fx - f32(divisor * float(math.floor(quotient))))
    elif op == 2:
        r = round_even_f(h)
    elif op == 3:
        r = f32(round_even_f(h) + trunc_f(f32(h * 0.75)))
    elif op == 4:
        ceiling = -floor_f(-f32(h * 0.75))
        sign = 1.0 if h > 0.0 else (-1.0 if h < 0.0 else h)
        r = f32(ceiling + f32(sign * 4.0))
    elif op == 5:
        step = 0.0 if h < 1.0 else 1.0
        r = f32(step + floor_f(f32(h * f32(0.3))))
    elif op == 6:
        span = 1.0 / 32.0
        t = f32(f32(h + 16.0) * span)
        t = min(max(t, 0.0), 1.0)
        term = f32(2.0 * t)
        term = f32(3.0 - term)
        term = f32(t * term)
        r = f32(t * term)
    else:
        r = f32(f32(fx - float(math.floor(fx))) + abs(fx))
    return bits(r)


# ------------------------------------------------------------------ loops

def loop_pixel(x, y):
    n = x % 17
    acc = 1
    for i in range(n):
        if i == (y & 3):
            continue
        if acc > 3000 + y * 16:
            break
        if (i & 1) == 0:
            acc = s32(acc * 3 + i)
        else:
            acc = s32(acc + (x ^ i))
    s = 0
    for j in range(y % 7):
        k = 0
        while k <= x % 5:
            if ((j + k) & 1) == 0:
                s += j * k + 1
            else:
                s -= k
            k += 1
            if s > 20 + (y >> 3):
                break
    t = x + y + 1
    c = 0
    while True:
        t = 3 * t + 1 if (t & 1) == 1 else t // 2
        c += 1
        if not (t != 1 and c < 40):
            break
    f = 0.0
    m = 0
    while True:
        if m >= (y & 7):
            break
        f = f32(f + f32(float(m) * 0.5))
        m += 1
    return u32(u32(acc) ^ (u32(s & 255) << 16) ^ (c << 24) ^ (int(f32(f * 2.0)) << 8))


# ------------------------------------------------------------------ varyings and attributes

# The seed vary16.vert scales its varyings by, and the data attributes 2 .. 15 of vin16.vert.
SEED = (3.0, -2.0, 5.0, 1.0)
VIN_DATA = [(float(k), float(2 * k - 7), float(1 - k), 3.0) for k in range(16)]


def varying(k):
    return [SEED[c] * (k + 1) + (k, -k, k * 0.5, 16 - k)[c] for c in range(4)]


def twice_weighted(locations):
    """Twice the weighted sum vary16.frag / subset.frag add to the pixel term (exact halves)."""
    total = 0.0
    for k in locations:
        total += sum(varying(k)[c] * (4 * k + c + 1) for c in range(4))
    assert total * 2 == int(total * 2)
    return int(total * 2)


def twice_vin16():
    sums = [sum(VIN_DATA[k][c] * k for k in range(2, 16)) for c in range(4)]
    total = sums[0] * 1 + sums[1] * 3 + sums[2] * 5 + sums[3] * 7
    return int(total * 2)


def vio16_varying(k):
    """Varying k (1 .. 15) of vio16.vert: data attribute k + 1 weighted by k + 1 plus the next one (2 after 15)."""
    if k == 15:
        return [VIN_DATA[2][c] * 16.0 + VIN_DATA[15][c] for c in range(4)]
    following = k + 2 if k + 2 <= 15 else 2
    return [VIN_DATA[k + 1][c] * (k + 1) + VIN_DATA[following][c] for c in range(4)]


def twice_vio16():
    """Twice the weighted sum vary16.frag adds to the pixel term over vio16.vert's varyings (whole numbers)."""
    total = 0.0
    for k in range(1, 16):
        total += sum(vio16_varying(k)[c] * (4 * k + c + 1) for c in range(4))
    assert total == int(total) and abs(total) < (1 << 23)
    return int(total * 2)


# ------------------------------------------------------------------ spilling

def spill_pairs(directory):
    """The products spill.frag sums, in its order, read from its source."""
    source = (directory / 'spill.frag').read_text()
    pairs = [(int(a), int(b)) for a, b in re.findall(r's \+= t(\d+) \* t(\d+);', source)]
    assert len(pairs) == 96
    return pairs


def spill_pixel(x, y, pairs):
    """spill.frag: 96 values of the pixel, the per-pixel loop over a and b, then the sum, in the lowering's order."""
    fx, fy = float(x), float(y)
    t = [f32(f32(fx * float(k + 1)) + fy) for k in range(96)]
    a = t[7]
    b = t[50]
    n = int(f32(fx + fy)) % 5
    for _ in range(n):
        a = f32(f32(a * 0.5) + t[3])
        b = f32(f32(b + f32(t[90] * 0.25)) - a)
    s = 0.0
    for first, second in pairs:
        s = f32(s + f32(t[first] * t[second]))
    s = f32(s + f32(a + b))
    return bits(s)


# ------------------------------------------------------------------ aggregates

AGG_TABLE = (3, -7, 11, 19, -23)


def agg_pixel(x, y):
    """agg.frag: a local array through dynamic indices, a constant array, structures holding an array."""
    values = [0] * 8
    for i in range(8):
        values[i] = s32((x * (i + 1)) ^ (y << i))
    k = (x + y) & 7
    values[k] = s32(values[k] + AGG_TABLE[y % 5])
    first = {'a': x, 'b': [y, x + y], 'c': [f32(float(x) * 0.5), float(y)]}
    second = {'a': first['a'], 'b': list(first['b']), 'c': list(first['c'])}
    second['b'][1] = values[y & 7]
    pairs = [first, second]
    pairs[x & 1]['a'] = s32(pairs[x & 1]['a'] + 100)
    picked = pairs[y & 1]
    r = u32(u32(values[x & 7]) * 31 + u32(values[(x * 3 + y) & 7]))
    r ^= u32(u32(first['a']) + u32(second['a']) * 7 + u32(second['b'][1]) * 13 + u32(picked['b'][0]) +
             int(f32(picked['c'][x & 1] * 2.0)))
    return u32(r + (u32(picked['a']) << 20))


# ------------------------------------------------------------------ matrix functions and halves

def exact_det(columns):
    """The determinant of a square matrix of floats, exactly (Fractions), cofactor expansion."""
    size = len(columns)
    if size == 1:
        return fractions.Fraction(columns[0][0])
    total = fractions.Fraction(0)
    for c in range(size):
        minor = [[columns[k][r] for r in range(1, size)] for k in range(size) if k != c]
        term = fractions.Fraction(columns[c][0]) * exact_det(minor)
        total += term if c % 2 == 0 else -term
    return total


def exact_inverse(columns):
    """The inverse as columns of Fractions: element (row r, column c) is the cofactor of (row c, column r) over det."""
    size = len(columns)
    det = exact_det(columns)
    result = []
    for c in range(size):
        column = []
        for r in range(size):
            minor = [[columns[k][row] for row in range(size) if row != c] for k in range(size) if k != r]
            cofactor = exact_det(minor) * (1 if (r + c) % 2 == 0 else -1)
            column.append(cofactor / det)
        result.append(column)
    return result


def exact_bits(value):
    """The bits of a Fraction that a float holds exactly."""
    as_float = float(value)
    assert fractions.Fraction(f32(as_float)) == value
    return bits(as_float)


def half_bits(value):
    """The IEEE-754 half of a float, rounded to nearest even."""
    return struct.unpack('<H', struct.pack('<e', value))[0]


def half_value(word):
    return struct.unpack('<e', struct.pack('<H', word))[0]


def matfn_pixel(x, y):
    """matfn.frag: determinants, inverses, PackHalf2x16 / UnpackHalf2x16."""
    group = (y >> 3) & 3
    a = float((x & 3) - 1)
    b = float((y & 3) - 2)
    c = float((x >> 3) & 3)
    d = float((y >> 5) + 1)
    m2 = [[2.0, a], [0.0, 1.0]]
    m3 = [[1.0, a, b], [0.0, 2.0, c], [0.0, 0.0, 4.0]]
    m4 = [[1.0, 0.0, 0.0, 0.0], [a, 1.0, 0.0, 0.0], [b, c, 2.0, 0.0], [d, a, b, 1.0]]
    g3 = [[1.0, a, b], [c, d, 1.0], [2.0, a, 0.0]]
    if group == 0:
        total = exact_det(m2) + exact_det(m3) * 4 + exact_det(m4) * 64 + exact_det(g3) * 256
        return exact_bits(total)
    if group == 1:
        return exact_bits(exact_inverse(m4)[x & 3][(x >> 2) & 3])
    if group == 2:
        if (x & 8) == 0:
            return exact_bits(exact_inverse(m3)[x % 3][(x >> 4) % 3])
        return exact_bits(exact_inverse(m2)[x & 1][(x >> 1) & 1])
    if (x & 1) == 0:
        low = f32(f32(a * f32(0.3)) + float(y))
        high = f32(f32(b * f32(1.7)) - float(x))
        return half_bits(low) | (half_bits(high) << 16)
    word = (u32(x * 40503 + y * 977) & 0x83ff83ff) | 0x3c004000
    back_x = half_value(word & 0xffff)
    back_y = half_value(word >> 16)
    return bits(f32(f32(back_x * 4.0) + back_y))


# ------------------------------------------------------------------ pixel position and derivatives

def coord_pixel(x, y):
    """coord.frag: x and y, the centre, z 0, w 1, agreeing with the interpolated coordinate."""
    return x | (y << 8) | (0x1f << 16)


def deriv_pixel(x, y):
    """deriv.frag: 3, 5, 8, then twice the quad's coarse x, each row's fine x and the coarse y derivatives of x y."""
    x0 = x & ~1
    y0 = y & ~1
    return 3 | (5 << 2) | (8 << 5) | ((2 * y0 + 1) << 9) | ((2 * y + 1) << 16) | ((2 * x0 + 1) << 23)


# ------------------------------------------------------------------ interpolation without perspective

# The interpolation step's corners: x, y in normalized device coordinates, their w, the corner's number.
PERSP_CORNERS = ((-1.0, -1.0, 1.0, 2.0), (1.0, -1.0, 3.0, 1.0), (1.0, 1.0, 5.0, 3.0), (-1.0, 1.0, 2.0, 0.0))


def persp_value(x, y):
    """The corners' x + 3 interpolated with perspective at the pixel centre, in its triangle (0 1 2 or 0 2 3)."""
    px = x + 0.5
    py = y + 0.5
    screen = [((cx + 1.0) * 32.0, (cy + 1.0) * 32.0) for cx, cy, _, _ in PERSP_CORNERS]
    triangle = (0, 1, 2) if py <= px else (0, 2, 3)
    (x0, y0), (x1, y1), (x2, y2) = (screen[k] for k in triangle)
    area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    l1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / area
    l2 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / area
    weights = (1.0 - l1 - l2, l1, l2)
    top = sum(weights[i] * (PERSP_CORNERS[k][0] + 3.0) / PERSP_CORNERS[k][2] for i, k in enumerate(triangle))
    bottom = sum(weights[i] / PERSP_CORNERS[k][2] for i, k in enumerate(triangle))
    return top / bottom


def nopersp_pixel(x, y):
    """nopersp.frag: 16 x + 8 (the x without perspective, in 512ths from -1) and the provoking corner's number."""
    return (16 * x + 8) | (int(PERSP_CORNERS[0][3]) << 30)


def persp_pixel(x, y):
    """persp.frag: the bits of x + 3 interpolated with perspective (compared loosely by the kernel test)."""
    return bits(persp_value(x, y))


# ------------------------------------------------------------------ points

# The point step's points: the centre's pixel corner x and y, the size, the number.
POINTS = ((8, 8, 4, 1), (24, 10, 8, 2), (40, 40, 8, 3), (56, 20, 4, 4), (12, 50, 6, 5), (48, 56, 2, 6))


def point_word(x, y):
    """point_pixel() in whole numbers, as the kernel test computes it: 16 s = 8 + 8 (2 x + 1 - 2 cx) / size, rounded."""
    for cx, cy, size, number in POINTS:
        u = 8 * size + 8 * (2 * x + 1 - 2 * cx)
        v = 8 * size + 8 * (2 * y + 1 - 2 * cy)
        if 0 <= u < 16 * size and 0 <= v < 16 * size:
            return ((2 * u + size) // (2 * size)) | (((2 * v + size) // (2 * size)) << 8) | (number << 16) | (1 << 24)
    return 0


def point_pixel(x, y):
    """point.frag: gl_PointCoord in sixteenths and the point's number where a point covers the pixel's centre."""
    for cx, cy, size, number in POINTS:
        s = 0.5 + (x + 0.5 - cx) / size
        t = 0.5 + (y + 0.5 - cy) / size
        if 0.0 <= s < 1.0 and 0.0 <= t < 1.0:
            for value in (s * 16.0, t * 16.0):
                assert abs(value - math.floor(value) - 0.5) > 0.1
            return int(round(s * 16.0)) | (int(round(t * 16.0)) << 8) | (number << 16) | (1 << 24)
    return 0


# ------------------------------------------------------------------ vertex formats

# The format step's vertex: 64 bytes, the position first (written per corner by the kernel test), then the attributes.
VFORMAT_STRIDE = 64
VFORMAT_UNORM8 = (0x12, 0x80, 0xff, 0x00)
VFORMAT_SSCALED16 = (-1234, 30000)
VFORMAT_SINT16 = (-5, 32767)
VFORMAT_UINT8 = (7, 200, 255)
VFORMAT_HALVES = (1.5, -2.25, 1024.0, 0.0999755859375)
VFORMAT_PACKED10 = (1023, 0, 512, 2)
VFORMAT_SNORM16 = -12345
VFORMAT_SSCALED8 = (-100, 127)
VFORMAT_UINT16 = (65535, 1, 4660)


def vformat_attributes():
    """The bytes 16 .. 63 of every vertex: the attributes at the offsets the kernel test's pipeline gives them."""
    data = bytearray(VFORMAT_STRIDE - 16)
    struct.pack_into('<4B', data, 0, *VFORMAT_UNORM8)
    struct.pack_into('<2h', data, 4, *VFORMAT_SSCALED16)
    struct.pack_into('<2h', data, 8, *VFORMAT_SINT16)
    struct.pack_into('<3B', data, 12, *VFORMAT_UINT8)
    struct.pack_into('<4e', data, 16, *VFORMAT_HALVES)
    r, g, b, a = VFORMAT_PACKED10
    struct.pack_into('<I', data, 24, r | (g << 10) | (b << 20) | (a << 30))
    struct.pack_into('<h', data, 28, VFORMAT_SNORM16)
    struct.pack_into('<2b', data, 30, *VFORMAT_SSCALED8)
    struct.pack_into('<3H', data, 32, *VFORMAT_UINT16)
    return list(struct.unpack('<12I', bytes(data)))


def vformat_words():
    """The twelve words vformat.vert makes of the attributes."""
    r, g, b, a = VFORMAT_PACKED10
    return [
        VFORMAT_UNORM8[0] | (VFORMAT_UNORM8[1] << 8) | (VFORMAT_UNORM8[2] << 16) | (VFORMAT_UNORM8[3] << 24),
        (VFORMAT_SSCALED16[0] & 0xffff) | u32(VFORMAT_SSCALED16[1] << 16),
        (VFORMAT_SINT16[0] & 0xffff) | u32(VFORMAT_SINT16[1] << 16),
        VFORMAT_UINT8[0] | (VFORMAT_UINT8[1] << 8) | (VFORMAT_UINT8[2] << 16) | (1 << 24),
        half_bits(VFORMAT_HALVES[0]) | (half_bits(VFORMAT_HALVES[1]) << 16),
        half_bits(VFORMAT_HALVES[2]) | (half_bits(VFORMAT_HALVES[3]) << 16),
        r | (g << 10) | (b << 20) | (a << 30),
        (VFORMAT_SNORM16 & 0xffff) | (7 << 16),
        (VFORMAT_SSCALED8[0] & 0xffff) | u32(VFORMAT_SSCALED8[1] << 16),
        VFORMAT_UINT16[0] | (VFORMAT_UINT16[1] << 16),
        VFORMAT_UINT16[2] | (1 << 16),
        0x600df00d,
    ]


def vformat_pixel(x, y):
    return vformat_words()[x % 12]


# ------------------------------------------------------------------ boundaries

INT_MIN = -2 ** 31
BOUNDARY = (0, 1, -1, 2, -2, 3, -7, 255, 256, 32767, -32768, 2 ** 31 - 1, INT_MIN, INT_MIN + 1,
            1000000007, -1000000007)
EDGE_MARKER = 0x5a5a5a5a

# The boundary steps' data is in the kernel only when the build defines I915_VKE2_BOUNDARY (the test set "vke2",
# platform/amd64/vmunix.mk; no test kernel holds every scenario).  The host fixtures always have it.
BOUNDARY_ONLY = '#if !defined(I915_VKE2_IN_KERNEL) || defined(I915_VKE2_BOUNDARY)'


def smod(a, b):
    """OpSMod: the remainder with the divisor's sign."""
    r = a - b * sdiv(a, b)
    if r != 0 and (r < 0) != (b < 0):
        r += b
    return r


def edge_pixel(x, y):
    op = y & 15
    a = BOUNDARY[x & 15]
    b = BOUNDARY[(y >> 4) * 4 + ((x >> 4) & 3)]
    ua, ub = u32(a), u32(b)
    undefined_signed = b == 0 or (a == INT_MIN and b == -1)
    if op == 0:
        r = EDGE_MARKER if undefined_signed else sdiv(a, b)
    elif op == 1:
        r = EDGE_MARKER if undefined_signed else smod(a, b)
    elif op == 2:
        r = EDGE_MARKER if b == 0 else ua // ub
    elif op == 3:
        r = EDGE_MARKER if b == 0 else ua % ub
    elif op == 4:
        r = a << (b & 31)
    elif op == 5:
        r = a >> (b & 31)
    elif op == 6:
        r = ua >> (b & 31)
    elif op == 7:
        r = a * b
    elif op == 8:
        r = a + b
    elif op == 9:
        r = a - b
    elif op == 10:
        r = -a
    elif op == 11:
        r = s32(min(a, b)) ^ s32(max(a, b) * 3)
    elif op == 12:
        r = min(ua, ub) ^ (max(ua, ub) >> 1)
    elif op == 13:
        r = int(a == b) | int(a < b) << 1 | int(a > b) << 2 | int(ua < ub) << 3 | int(ua > ub) << 4
    elif op == 14:
        r = bits(f32(float(a))) ^ bits(f32(float(ub)))
    else:
        r = u32(int(f32(float(ua >> 1))))
    return u32(r)


# The int16 step's 32-bit values, truncated to 16 bits by the shader (int16.frag).
INT16_BOUNDARY = (0, 1, -1, 0x12348000, 0x00017fff, 0x7fff, -32768, 0x0003fffe, 2,
                  -7, 0x10000, 0xffff, 0x2345ff9c, 100, -100, 0x7ffe0005)
INT16_TABLE = (11, 22, 33, 44)


def s16(v):
    """The signed 16-bit integer of an integer's low 16 bits."""
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def u16(v):
    """The unsigned 16-bit integer of an integer's low 16 bits."""
    return v & 0xffff


def fbits(v):
    """The bits of an integer converted to a float (exact: 16-bit values are)."""
    return bits(f32(float(v)))


def int16_pixel(x, y):
    op = y & 15
    a32 = INT16_BOUNDARY[x & 15]
    b32 = INT16_BOUNDARY[(y >> 4) * 4 + ((x >> 4) & 3)]
    a, b, ua, ub = s16(a32), s16(b32), u16(a32), u16(b32)
    undefined_signed = b == 0 or (a == -32768 and b == -1)
    if op == 0:
        r = EDGE_MARKER if undefined_signed else s16(sdiv(a, b))
    elif op == 1:
        r = EDGE_MARKER if undefined_signed else s16(smod(a, b))
    elif op == 2:
        r = EDGE_MARKER if ub == 0 else ua // ub
    elif op == 3:
        r = EDGE_MARKER if ub == 0 else ua % ub
    elif op == 4:
        r = s16(a >> (ub & 15))
    elif op == 5:
        r = u16(ua >> (ub & 15))
    elif op == 6:
        r = s16(a << (ub & 15))
    elif op == 7:
        r = s16(a * b)
    elif op == 8:
        r = s16(a + b)
    elif op == 9:
        r = s16(a - b)
    elif op == 10:
        r = min(a, b) ^ (max(a, b) << 8)
    elif op == 11:
        r = min(ua, ub) ^ (max(ua, ub) << 8)
    elif op == 12:
        r = (int(a == b) | int(a < b) << 1 | int(a > b) << 2 | int(ua < ub) << 3 | int(ua > ub) << 4 |
             int(ua != u16(b)) << 5 | int(a >= b) << 6 | int(ua <= ub) << 7)
    elif op == 13:
        r = fbits(a) ^ fbits(ub)
    elif op == 14:
        sign = (b > 0) - (b < 0)
        r = s16(abs(a)) ^ (min(max(a, -100), 100) << 16) ^ (sign * 7)
    else:
        index = s16((a32 & 3) | 0x30000)
        packed = u16(a) | (u16(b) << 16)
        r = packed ^ (u16(u32(a32) >> 16) * 3) ^ INT16_TABLE[index]
    return u32(r)


def undef_pixel(x, y):
    """The guard rows' words, and on the odd rows what the host models give (0 for a division SPIR-V leaves
    undefined, a shift by its count's low five bits, as the EU does): the kernel test does not judge those."""
    op = (y >> 1) & 7
    a = s32(u32(x + 1) * 2654435761)
    if (y & 1) == 0:
        return u32(x * 977 + y)
    if op in (0, 1, 2, 3, 4, 5):
        return 0
    if op == 6:
        return u32(a << ((32 + (x & 31)) & 31))
    return u32(a) >> ((33 + (x & 15)) & 31)


def killoop_pixel(x, y):
    acc = 1
    for i in range(x & 15):
        if i == (y & 15):
            return 0
        acc = u32(acc * 3 + i)
    return (acc & 0x00ffffff) | 0x81000000


def noinput_pixel(x, y):
    return x | (y << 8) | 0xa5000000


def c_words(lines, symbol, words, kind='uint32_t', per_line=6, fmt='0x{:08x}U'):
    lines.append(f'static const {kind} {symbol}[{len(words)}] = {{')
    for offset in range(0, len(words), per_line):
        lines.append('\t' + ', '.join(fmt.format(word) for word in words[offset:offset + per_line]) + ',')
    lines.append('};')


def image(function):
    return [function(x, y) for y in range(SIZE) for x in range(SIZE)]


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
        ' * GENERATED FILE - the SPIR-V, the inputs and the expected pixels of the generality test.',
        ' * Do not edit by hand.',
        ' *',
        ' * Generator : src/drivers/gpu/i915/tests/render/generality-shaders/regenerate.py',
        f' * Compiler  : {version} (--target-env=vulkan1.1 --target-spv=spv1.0 -O0)',
        ' *',
        ' * A pixel is RGBA8 with red in the low byte; a float is its IEEE-754 bits.',
        ' */',
    ]
    boundary = ('i915_vke2_edge_frag', 'i915_vke2_undef_frag', 'i915_vke2_killoop_frag', 'i915_vke2_noinput_vert',
                'i915_vke2_noinput_frag', 'i915_vke2_int16_frag')
    for source, stage, symbol in SHADERS:
        path = directory / source
        output = directory / (source + '.spv')
        run(['glslc', '--target-env=vulkan1.1', '--target-spv=spv1.0', f'-fshader-stage={stage}',
             '-O0', str(path), '-o', str(output)])
        run(['spirv-val', '--target-env', 'vulkan1.1', str(output)])
        binary = output.read_bytes()
        words = struct.unpack(f'<{len(binary) // 4}I', binary)
        lines.extend(['', f'/* {source} (sha256 {hashlib.sha256(path.read_bytes()).hexdigest()}), {len(words)} words. */'])
        if symbol in boundary:
            lines.append(BOUNDARY_ONLY)
        c_words(lines, symbol, words)
        if symbol in boundary:
            lines.append('#endif')

    lines.extend(['', '/* The matrix step\'s uniform block (binding 0, std140): m, r (row-major), m3 (MatrixStride 16), extra. */'])
    matrices = column_major_words(MAT_M) + row_major_words(MAT_R) + column_major_words(MAT_M3) + EXTRA
    assert len(matrices) == 48
    c_words(lines, 'i915_vke2_matrices', [bits(v) for v in matrices], per_line=4)
    lines.extend(['', '/* The matrix step\'s placement block (binding 1, std140): turn (row-major), scale. */'])
    placement = row_major_words(TURN) + column_major_words(SCALE)
    c_words(lines, 'i915_vke2_placement', [bits(v) for v in placement], per_line=4)
    lines.extend(['', '/* The matrix step\'s push constants (std430): p (row-major), q. */'])
    push = row_major_words(PUSH_P) + column_major_words(PUSH_Q)
    c_words(lines, 'i915_vke2_push', [bits(v) for v in push], per_line=4)

    # The matrix step's quad: where each corner of the target must land, sent through the inverse chain.
    lines.extend(['', '/*',
                  ' * The matrix step\'s four vertices (x, y of the position before the chain), top left, top right,',
                  ' * bottom right, bottom left: the chain transpose(turn) * scale takes each to its corner of the target.',
                  ' */'])
    chain = mat_mul(transpose(TURN), SCALE)
    before = []
    for fx, fy in ((-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)):
        # The chain is a quarter turn and a doubling: (px, py) -> (-2 py, 2 px), so px = fy / 2, py = -fx / 2.
        px, py = fy / 2.0, -fx / 2.0
        assert mat_vec(chain, [px, py, 0.0, 1.0])[:2] == [fx, fy]
        before.extend([bits(px), bits(py)])
    c_words(lines, 'i915_vke2_matrix_corners', before, per_line=2)

    lines.extend(['', '/* The seed of vary16.vert (location 2), and the data attributes 2 .. 15 of vin16.vert. */'])
    c_words(lines, 'i915_vke2_seed', [bits(v) for v in SEED], per_line=4)
    c_words(lines, 'i915_vke2_vin_data', [bits(v) for k in range(2, 16) for v in VIN_DATA[k]], per_line=4)

    lines.extend(['', '/*',
                  ' * The varying and attribute steps write floor(x) * 1000 + floor(y) * 100000 plus a constant: twice',
                  ' * the constant of each (every sum is a whole number of halves).',
                  ' */',
                  f'#define I915_VKE2_VARY16_TWICE {twice_weighted(range(1, 16))}U',
                  f'#define I915_VKE2_SUBSET_TWICE {twice_weighted((11, 1, 15, 6))}U',
                  f'#define I915_VKE2_VIN16_TWICE {twice_vin16()}U',
                  f'#define I915_VKE2_VIO16_TWICE {twice_vio16()}U'])

    lines.extend(['', '/* Every pixel of the matrix step: the bits of the float result it picks. */'])
    c_words(lines, 'i915_vke2_matrix_expected', image(matrix_pixel), per_line=8)
    lines.extend(['', '/* Every pixel of the integer step: its 32-bit result. */'])
    c_words(lines, 'i915_vke2_int_expected', image(int_pixel), per_line=8)
    lines.extend(['', '/* Every pixel of the float step: the bits of its result. */'])
    c_words(lines, 'i915_vke2_float_expected', image(float_pixel), per_line=8)
    lines.extend(['', '/* Every pixel of the loop step: its 32-bit result. */'])
    c_words(lines, 'i915_vke2_loop_expected', image(loop_pixel), per_line=8)
    lines.extend(['', '/* Every pixel of the spill step: the bits of its sum. */'])
    pairs = spill_pairs(directory)
    c_words(lines, 'i915_vke2_spill_expected', image(lambda x, y: spill_pixel(x, y, pairs)), per_line=8)

    # The steps of the GLES 2 core of the compiler.
    lines.extend(['', '/*',
                  ' * The interpolation step\'s four corners (x, y in normalized device coordinates, 0, w) and their',
                  ' * coordinate attribute (0, 0, the corner\'s number, 0), top left, top right, bottom right, bottom left.',
                  ' */'])
    persp = []
    for cx, cy, cw, number in PERSP_CORNERS:
        persp.extend([bits(cx), bits(cy), 0, bits(cw), 0, 0, bits(float(number)), 0])
    c_words(lines, 'i915_vke2_persp_vertices', persp, per_line=8)
    lines.extend(['', '/* The point step\'s points: the centre (normalized), 0, the size; the coordinate (the number, 0, 0, 0). */'])
    points = []
    for cx, cy, size, number in POINTS:
        points.extend([bits(cx / 32.0 - 1.0), bits(cy / 32.0 - 1.0), 0, bits(float(size)), bits(float(number)), 0, 0, 0])
    c_words(lines, 'i915_vke2_point_vertices', points, per_line=8)
    lines.append(f'#define I915_VKE2_POINT_COUNT {len(POINTS)}U')
    lines.extend(['', '/* The same points as whole numbers: the centre\'s pixel corner x and y, the size, the number. */'])
    c_words(lines, 'i915_vke2_points', [v for point in POINTS for v in point], per_line=4, fmt='{}U')
    lines.extend(['', '/* The twelve words vformat.vert makes of the attributes, which pixel column x shows word x % 12 of. */'])
    c_words(lines, 'i915_vke2_vformat_words', vformat_words(), per_line=4)
    lines.extend(['', '/* The format step\'s attribute bytes 16 .. 63 of every vertex (after its position). */'])
    c_words(lines, 'i915_vke2_vformat_attributes', vformat_attributes(), per_line=4)
    # The coord, deriv, nopersp, point and vformat steps' words are simple enough for the kernel test to make them
    # (i915_vke2_expect_p004()); these functions say the same and check the data they are made from.
    assert all(point_pixel(x, y) == point_word(x, y) for y in range(SIZE) for x in range(SIZE))
    for name, function, what in (('agg', agg_pixel, 'its 32-bit result'),
                                 ('matfn', matfn_pixel, 'the bits of its determinant, inverse element or halves'),
                                 ('persp', persp_pixel, 'the bits of x + 3 with perspective'),
                                 ('edge', edge_pixel, 'its 32-bit result, or the marker of an undefined division'),
                                 ('undef', undef_pixel, 'the guard word on even rows, the host models\' word on odd rows'),
                                 ('killoop', killoop_pixel, 'its accumulator with the top byte 0x81, or zero when discarded'),
                                 ('noinput', noinput_pixel, 'x, y and the mark 0xa5'),
                                 ('int16', int16_pixel, 'its 16-bit result made a 32-bit word, or the marker of an undefined division')):
        lines.extend(['', f'/* Every pixel of the {name} step: {what}. */'])
        if name in ('edge', 'undef', 'killoop', 'noinput', 'int16'):
            lines.append(BOUNDARY_ONLY)
        c_words(lines, f'i915_vke2_{name}_expected', image(function), per_line=8)
        if name in ('edge', 'undef', 'killoop', 'noinput', 'int16'):
            lines.append('#endif')
    lines.append('')
    (fixtures / 'generality-shaders-gen.inc').write_text('\n'.join(lines))


if __name__ == '__main__':
    main()

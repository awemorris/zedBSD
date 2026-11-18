#!/usr/bin/env python3
"""Compile the feature test's shaders and generate what the kernel test embeds: the SPIR-V, the blend
cases, the uniform blocks, the texels and the pixels every step expects."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: python3 src/drivers/gpu/i915/tests/render/feature-shaders/regenerate.py
#
# Writes <name>.spv next to each GLSL source and tests/fixtures/feature-shaders-gen.inc (included by
# tests/render/features.c, the scenario "vke1").
#
# The target is 64 x 64 pixels of R8G8B8A8_UNORM.  The expected pixels are computed here in double
# precision from the Vulkan definitions: the blend equation of VkBlendFactor / VkBlendOp on the source
# clamped to [0, 1] and the destination as it is stored (byte / 255), the result clamped and rounded to a
# byte; the uniform step's position and colour from the std140 blocks; the linear and nearest texture
# filters on 2 x 1 textures clamped to the edge.  The kernel test allows one step of a byte for blending
# and the uniform colour (the hardware's rounding of x.5), and two for linear filtering (its 8-bit
# sub-texel weights).
import hashlib
import math
import pathlib
import struct
import subprocess

# Source, stage, C array.
SHADERS = (
    ('pass.vert', 'vertex', 'i915_vke1_pass_vert'),
    ('color.frag', 'fragment', 'i915_vke1_color_frag'),
    ('ubo.vert', 'vertex', 'i915_vke1_ubo_vert'),
    ('ubo.frag', 'fragment', 'i915_vke1_ubo_frag'),
    ('tex3.frag', 'fragment', 'i915_vke1_tex3_frag'),
    ('texops.frag', 'fragment', 'i915_vke1_texops_frag'),
    ('texkinds.frag', 'fragment', 'i915_vke1_texkinds_frag'),
)

SIZE = 64

# VkBlendFactor, VkBlendOp and VkColorComponentFlagBits values.
ZERO, ONE, SRC_COLOR, ONE_MINUS_SRC_COLOR, DST_COLOR, ONE_MINUS_DST_COLOR = 0, 1, 2, 3, 4, 5
SRC_ALPHA, ONE_MINUS_SRC_ALPHA, DST_ALPHA, ONE_MINUS_DST_ALPHA = 6, 7, 8, 9
CONSTANT_COLOR, ONE_MINUS_CONSTANT_COLOR, CONSTANT_ALPHA, ONE_MINUS_CONSTANT_ALPHA = 10, 11, 12, 13
SRC_ALPHA_SATURATE = 14
ADD, SUBTRACT, REVERSE_SUBTRACT, MIN, MAX = 0, 1, 2, 3, 4
R, G, B, A = 1, 2, 4, 8
ALL = R | G | B | A

FACTOR_NAMES = {ZERO: 'ZERO', ONE: 'ONE', SRC_COLOR: 'SRC_COLOR', ONE_MINUS_SRC_COLOR: 'ONE_MINUS_SRC_COLOR',
                DST_COLOR: 'DST_COLOR', ONE_MINUS_DST_COLOR: 'ONE_MINUS_DST_COLOR', SRC_ALPHA: 'SRC_ALPHA',
                ONE_MINUS_SRC_ALPHA: 'ONE_MINUS_SRC_ALPHA', DST_ALPHA: 'DST_ALPHA',
                ONE_MINUS_DST_ALPHA: 'ONE_MINUS_DST_ALPHA', CONSTANT_COLOR: 'CONSTANT_COLOR',
                ONE_MINUS_CONSTANT_COLOR: 'ONE_MINUS_CONSTANT_COLOR', CONSTANT_ALPHA: 'CONSTANT_ALPHA',
                ONE_MINUS_CONSTANT_ALPHA: 'ONE_MINUS_CONSTANT_ALPHA', SRC_ALPHA_SATURATE: 'SRC_ALPHA_SATURATE'}
OP_NAMES = {ADD: 'ADD', SUBTRACT: 'SUBTRACT', REVERSE_SUBTRACT: 'REVERSE_SUBTRACT', MIN: 'MIN', MAX: 'MAX'}

# The colour the blend step's target is cleared to: every channel a whole byte.
DESTINATION = (0.2, 0.4, 0.6, 0.8)

# The blend constants of the pipelines, and the ones vkCmdSetBlendConstants sets for the dynamic case.
PIPELINE_CONSTANTS = (0.25, 0.5, 0.75, 1.0)
DYNAMIC_CONSTANTS = (0.75, 0.25, 0.5, 0.5)

# The sixteen blend cases, one 16 x 16 cell each (cell k at column k % 4, row k // 4):
# (name, enable, src colour, dst colour, colour op, src alpha, dst alpha, alpha op, write mask, dynamic
# constants, source colour).
BLEND_CASES = (
    ('off', 0, ONE, ZERO, ADD, ONE, ZERO, ADD, ALL, 0, (1.0, 0.5, 0.25, 1.0)),
    ('over', 1, SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ADD, SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ADD, ALL, 0, (1.0, 0.0, 0.0, 0.25)),
    ('add', 1, ONE, ONE, ADD, ONE, ONE, ADD, ALL, 0, (0.6, 0.8, 0.2, 0.4)),
    ('modulate', 1, ZERO, SRC_COLOR, ADD, ZERO, SRC_COLOR, ADD, ALL, 0, (0.5, 1.0, 0.25, 0.5)),
    ('dst-color', 1, DST_COLOR, ZERO, ADD, DST_COLOR, ZERO, ADD, ALL, 0, (0.5, 0.5, 1.0, 1.0)),
    ('inv-dst-color', 1, ONE_MINUS_DST_COLOR, ONE, ADD, ONE_MINUS_DST_COLOR, ONE, ADD, ALL, 0, (0.25, 0.5, 0.75, 0.5)),
    ('dst-alpha', 1, DST_ALPHA, ONE_MINUS_DST_ALPHA, ADD, DST_ALPHA, ONE_MINUS_DST_ALPHA, ADD, ALL, 0, (1.0, 1.0, 0.0, 0.0)),
    ('subtract', 1, ONE, ONE, SUBTRACT, ONE, ONE, SUBTRACT, ALL, 0, (0.5, 0.2, 0.9, 1.0)),
    ('reverse-subtract', 1, ONE, ONE, REVERSE_SUBTRACT, ONE, ONE, REVERSE_SUBTRACT, ALL, 0, (0.1, 0.5, 0.2, 0.3)),
    ('min', 1, SRC_ALPHA, ZERO, MIN, SRC_ALPHA, ZERO, MIN, ALL, 0, (0.1, 0.9, 0.5, 0.9)),
    ('max', 1, SRC_ALPHA, ZERO, MAX, SRC_ALPHA, ZERO, MAX, ALL, 0, (0.1, 0.9, 0.5, 0.9)),
    ('constant', 1, CONSTANT_COLOR, ONE_MINUS_CONSTANT_COLOR, ADD, CONSTANT_COLOR, ONE_MINUS_CONSTANT_COLOR, ADD, ALL, 0, (1.0, 0.0, 1.0, 0.0)),
    ('dynamic-constant', 1, CONSTANT_COLOR, ONE_MINUS_CONSTANT_COLOR, ADD, CONSTANT_COLOR, ONE_MINUS_CONSTANT_COLOR, ADD, ALL, 1, (1.0, 0.0, 1.0, 0.0)),
    ('independent-alpha', 1, SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ADD, ZERO, ONE, ADD, ALL, 0, (0.0, 1.0, 0.0, 0.5)),
    ('mask-rb', 0, ONE, ZERO, ADD, ONE, ZERO, ADD, R | B, 0, (1.0, 1.0, 1.0, 1.0)),
    ('mask-ga-add', 1, ONE, ONE, ADD, ONE, ONE, ADD, G | A, 0, (0.5, 0.5, 0.5, 0.1)),
)

# The uniform step: the byte offset of the second transform and the second material in their buffers.
SECOND_BLOCK = 256

# Transforms: a column-major mat4 (each column 4 floats, std140 MatrixStride 16), then the offset.
TRANSFORMS = (
    ((0.25, 0.0, 0.0, 0.0), (0.0, 0.5, 0.0, 0.0), (0.0, 0.0, 1.0, 0.0), (0.5, -0.5, 0.0, 1.0), (0.25, 0.25, 0.0, 0.0)),
    ((0.25, 0.0, 0.0, 0.0), (0.0, 0.5, 0.0, 0.0), (0.0, 0.0, 1.0, 0.0), (-0.75, 0.25, 0.0, 1.0), (0.25, 0.25, 0.0, 0.0)),
)

# Materials: the colour, scale[0] (never read), scale[1], then the four columns of extra (only column 2 read).
MATERIALS = (
    ((0.5, 0.25, 1.0, 1.0), (9.0, 9.0, 9.0, 9.0), (0.5, 1.0, 0.5, 1.0),
     (7.0, 7.0, 7.0, 7.0), (6.0, 6.0, 6.0, 6.0), (0.25, 0.5, 0.25, 0.0), (5.0, 5.0, 5.0, 5.0)),
    ((1.0, 0.5, 0.25, 0.5), (9.0, 9.0, 9.0, 9.0), (0.25, 0.5, 1.0, 2.0),
     (7.0, 7.0, 7.0, 7.0), (6.0, 6.0, 6.0, 6.0), (0.0, 0.25, 0.5, 0.0), (5.0, 5.0, 5.0, 5.0)),
)

# The three 2 x 1 textures of the texture step (RGBA bytes of texel 0 and 1) and whether each filters linearly.
TEXTURES = (
    (((200, 10, 20, 255), (40, 30, 60, 255)), 1),
    (((10, 100, 10, 255), (10, 180, 10, 255)), 0),
    (((0, 0, 50, 255), (0, 0, 250, 255)), 0),
)

# The textured quad: pixels 16 .. 48 in both directions, the coordinate (0, 0) .. (1, 1).
TEX_FIRST = 16
TEX_SIDE = 32


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


def unorm(x):
    """A value in [0, 1] as the byte an RGBA8 target stores, rounded to nearest."""
    x = min(max(x, 0.0), 1.0)
    return int(math.floor(x * 255.0 + 0.5))


def pixel(channels):
    """Four bytes as the pixel an RGBA8 target holds: red in the low byte."""
    return channels[0] | (channels[1] << 8) | (channels[2] << 16) | (channels[3] << 24)


def factor(code, channel, s, d, c):
    """The value of a VkBlendFactor for one channel (3 is alpha)."""
    if code == ZERO:
        return 0.0
    if code == ONE:
        return 1.0
    if code == SRC_COLOR:
        return s[channel]
    if code == ONE_MINUS_SRC_COLOR:
        return 1.0 - s[channel]
    if code == DST_COLOR:
        return d[channel]
    if code == ONE_MINUS_DST_COLOR:
        return 1.0 - d[channel]
    if code == SRC_ALPHA:
        return s[3]
    if code == ONE_MINUS_SRC_ALPHA:
        return 1.0 - s[3]
    if code == DST_ALPHA:
        return d[3]
    if code == ONE_MINUS_DST_ALPHA:
        return 1.0 - d[3]
    if code == CONSTANT_COLOR:
        return c[channel]
    if code == ONE_MINUS_CONSTANT_COLOR:
        return 1.0 - c[channel]
    if code == CONSTANT_ALPHA:
        return c[3]
    if code == ONE_MINUS_CONSTANT_ALPHA:
        return 1.0 - c[3]
    if code == SRC_ALPHA_SATURATE:
        return 1.0 if channel == 3 else min(s[3], 1.0 - d[3])
    raise ValueError(code)


def blend_expected(case):
    """The pixel a blend case leaves over the destination."""
    _, enable, src_c, dst_c, op_c, src_a, dst_a, op_a, mask, dynamic, color = case
    stored = [unorm(value) for value in DESTINATION]
    d = [byte / 255.0 for byte in stored]
    s = [min(max(f32(value), 0.0), 1.0) for value in color]
    c = DYNAMIC_CONSTANTS if dynamic else PIPELINE_CONSTANTS
    result = []
    for channel in range(4):
        if not mask & (1 << channel):
            result.append(stored[channel])
            continue
        if not enable:
            result.append(unorm(s[channel]))
            continue
        src_f, dst_f, op = (src_c, dst_c, op_c) if channel < 3 else (src_a, dst_a, op_a)
        if op == MIN:
            value = min(s[channel], d[channel])
        elif op == MAX:
            value = max(s[channel], d[channel])
        else:
            a = s[channel] * factor(src_f, channel, s, d, c)
            b = d[channel] * factor(dst_f, channel, s, d, c)
            value = {ADD: a + b, SUBTRACT: a - b, REVERSE_SUBTRACT: b - a}[op]
        result.append(unorm(value))
    return pixel(result)


def ubo_expected(index):
    """The rectangle (left, top, right, bottom) and the pixel of the quad drawn with transform and material index."""
    columns = TRANSFORMS[index][:4]
    offset = TRANSFORMS[index][4]
    corners = []
    for x, y in ((-1.0, -1.0), (1.0, 1.0)):
        position = (x, y, 0.0, 1.0)
        out = [sum(columns[k][row] * position[k] for k in range(4)) + offset[row] for row in range(4)]
        assert out[3] == 1.0
        corners.append(((out[0] + 1.0) * SIZE / 2.0, (out[1] + 1.0) * SIZE / 2.0))
    left, top = corners[0]
    right, bottom = corners[1]
    for edge in (left, top, right, bottom):
        assert edge == int(edge)
    material = MATERIALS[index]
    color = [material[0][k] * material[2][k] + material[5][k] for k in range(4)]
    return int(left), int(top), int(right), int(bottom), pixel([unorm(value) for value in color])


def sample(texels, linear, u):
    """One channel set of a 2 x 1 texture at coordinate u, clamped to the edge (v does not matter)."""
    if not linear:
        index = min(max(int(math.floor(u * 2.0)), 0), 1)
        return list(texels[index])
    t = u * 2.0 - 0.5
    first = int(math.floor(t))
    weight = t - first
    left = texels[min(max(first, 0), 1)]
    right = texels[min(max(first + 1, 0), 1)]
    return [left[k] / 255.0 * (1.0 - weight) + right[k] / 255.0 * weight for k in range(4)]


def tex3_expected():
    """Every pixel of the texture step: black outside the quad, (first.r, second.g, third.b, 1) inside."""
    image = []
    for py in range(SIZE):
        for px in range(SIZE):
            if not (TEX_FIRST <= px < TEX_FIRST + TEX_SIDE and TEX_FIRST <= py < TEX_FIRST + TEX_SIDE):
                image.append(pixel((0, 0, 0, 255)))
                continue
            u = (px - TEX_FIRST + 0.5) / TEX_SIDE
            values = []
            for channel, (texels, linear) in enumerate(TEXTURES):
                sampled = sample(texels, linear, u)
                values.append(unorm(sampled[channel]) if linear else sampled[channel])
            image.append(pixel((values[0], values[1], values[2], 255)))
    return image


# The texture operand step's texture, 8 x 8 texels on level 0 and 4 x 4 on level 1, every texel its own.
MIP_SIDE = 8


def mip_texel(level, i, j):
    """Texel (i, j) of a level: red and green from its place, blue from its level."""
    step = 32 << level
    return (i * step + 8 * (level + 1), j * step + 8 * (level + 1), 64 + 128 * level, 255)


def texops_expected():
    """Every pixel of the texture operand step: the nearest texel of the level and at the offset its band asks."""
    image = []
    for py in range(SIZE):
        for px in range(SIZE):
            band = py >> 4
            level = 0 if band == 0 or (band == 3 and (px & 32) == 0) else 1
            side = MIP_SIDE >> level
            i = px * side // SIZE
            j = py * side // SIZE
            if band == 3 and level == 0:
                i, j = i + 1, j - 1
            elif band == 3:
                i, j = i - 1, j + 1
            i = min(max(i, 0), side - 1)
            j = min(max(j, 0), side - 1)
            image.append(pixel(mip_texel(level, i, j)))
    return image


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
        ' * GENERATED FILE - the SPIR-V, the inputs and the expected pixels of the feature test.',
        ' * Do not edit by hand.',
        ' *',
        ' * Generator : src/drivers/gpu/i915/tests/render/feature-shaders/regenerate.py',
        f' * Compiler  : {version} (--target-env=vulkan1.1 --target-spv=spv1.0 -O0)',
        ' *',
        ' * A pixel is RGBA8 with red in the low byte; a float is its IEEE-754 bits.',
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

    lines.extend(['', '/* The NDC coordinate of step k (0 .. 8): -1 + k / 4, which is pixel 8 * k. */'])
    c_words(lines, 'i915_vke1_ndc', [bits(f32(-1.0 + k / 4.0)) for k in range(9)])

    lines.extend(['', '/* The colour the blend step clears the target to, and the pixel that leaves. */'])
    c_words(lines, 'i915_vke1_destination', [bits(f32(value)) for value in DESTINATION], per_line=4)
    lines.append(f'#define I915_VKE1_DESTINATION_PIXEL 0x{pixel([unorm(v) for v in DESTINATION]):08x}U')

    lines.extend(['', '/* The blend constants of every blend pipeline, and the ones the dynamic case sets. */'])
    c_words(lines, 'i915_vke1_pipeline_constants', [bits(f32(value)) for value in PIPELINE_CONSTANTS], per_line=4)
    c_words(lines, 'i915_vke1_dynamic_constants', [bits(f32(value)) for value in DYNAMIC_CONSTANTS], per_line=4)

    lines.extend(['', '/*',
                  ' * The blend cases, cell k at column k % 4 and row k / 4: the blend of attachment 0, the',
                  ' * source colour and the pixel expected over the destination.',
                  ' */',
                  f'static const struct i915_vke1_blend_case i915_vke1_blend_cases[{len(BLEND_CASES)}] = {{'])
    for case in BLEND_CASES:
        name, enable, src_c, dst_c, op_c, src_a, dst_a, op_a, mask, dynamic, color = case
        lines.append(f'\t/* {name}: {FACTOR_NAMES[src_c]} / {FACTOR_NAMES[dst_c]} / {OP_NAMES[op_c]}, '
                     f'{FACTOR_NAMES[src_a]} / {FACTOR_NAMES[dst_a]} / {OP_NAMES[op_a]}, '
                     f'enable {enable}, mask 0x{mask:x}, dynamic constants {dynamic} */')
        colour = ', '.join(f'0x{bits(f32(value)):08x}U' for value in color)
        lines.append(f'\t{{ "{name}", {enable}U, {src_c}U, {dst_c}U, {op_c}U, {src_a}U, {dst_a}U, {op_a}U, 0x{mask:x}U, '
                     f'{dynamic}, {{ {colour} }}, 0x{blend_expected(case):08x}U }},')
    lines.append('};')

    lines.extend(['', f'/* Where the second transform and the second material sit in their buffers. */',
                  f'#define I915_VKE1_SECOND_BLOCK {SECOND_BLOCK}U'])
    lines.extend(['', '/* The two transform blocks: the mat4 column after column, then the offset (20 floats each). */'])
    c_words(lines, 'i915_vke1_transforms', [bits(f32(v)) for block in TRANSFORMS for vector in block for v in vector],
            per_line=4)
    lines.extend(['', '/* The two material blocks: the colour, scale[0], scale[1], then the columns of extra (28 floats each). */'])
    c_words(lines, 'i915_vke1_materials', [bits(f32(v)) for block in MATERIALS for vector in block for v in vector],
            per_line=4)
    lines.extend(['', '/* The rectangle (left, top, right, bottom) and the pixel of each uniform draw. */',
                  'static const struct i915_vke1_rect i915_vke1_ubo_expected[2] = {'])
    for index in range(2):
        left, top, right, bottom, value = ubo_expected(index)
        lines.append(f'\t{{ {left}U, {top}U, {right}U, {bottom}U, 0x{value:08x}U }},')
    lines.append('};')

    lines.extend(['', '/* The two texels of each texture of the texture step, and whether its sampler filters linearly. */'])
    c_words(lines, 'i915_vke1_texels', [pixel(texel) for texels, _ in TEXTURES for texel in texels], per_line=2)
    c_words(lines, 'i915_vke1_texture_linear', [linear for _, linear in TEXTURES], per_line=3, fmt='{}U')
    lines.extend(['', '/* Every pixel of the texture step. */'])
    c_words(lines, 'i915_vke1_tex3_expected', tex3_expected(), per_line=8)
    lines.extend(['', '/* The texture operand step\'s texels: level 0 row after row, then level 1. */'])
    c_words(lines, 'i915_vke1_mip_level0', [pixel(mip_texel(0, i, j)) for j in range(MIP_SIDE) for i in range(MIP_SIDE)],
            per_line=8)
    c_words(lines, 'i915_vke1_mip_level1',
            [pixel(mip_texel(1, i, j)) for j in range(MIP_SIDE // 2) for i in range(MIP_SIDE // 2)], per_line=4)
    lines.extend(['', '/* Every pixel of the texture operand step. */'])
    c_words(lines, 'i915_vke1_texops_expected', texops_expected(), per_line=8)
    lines.append('')
    (fixtures / 'feature-shaders-gen.inc').write_text('\n'.join(lines))


if __name__ == '__main__':
    main()

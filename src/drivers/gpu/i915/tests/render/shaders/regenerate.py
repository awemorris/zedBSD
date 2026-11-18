#!/usr/bin/env python3
"""Compile the executor test's shaders and generate the words the kernel test embeds."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: python3 src/drivers/gpu/i915/tests/render/shaders/regenerate.py
#
# Writes <name>.spv next to each GLSL source (read by the host fixture
# i915-vk-pipe-test.c) and tests/fixtures/executor-shaders-gen.inc
# (included by tests/render/executor.c).

import hashlib
import pathlib
import struct
import subprocess

SHADERS = (
    ('place.vert', 'vertex', 'i915_vkx_place_vert'),
    ('color.frag', 'fragment', 'i915_vkx_color_frag'),
    ('push.frag', 'fragment', 'i915_vkx_push_frag'),
    ('tex.frag', 'fragment', 'i915_vkx_tex_frag'),
)


def run(arguments):
    """Runs one tool and returns what it printed."""
    return subprocess.run(arguments, check=True, timeout=30, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True).stdout.strip()


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
        ' * GENERATED FILE - the SPIR-V of the executor test shaders.  Do not edit by hand.',
        ' *',
        ' * Generator : src/drivers/gpu/i915/tests/render/shaders/regenerate.py',
        f' * Compiler  : {version} (--target-env=vulkan1.1 --target-spv=spv1.0 -O0)',
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
        lines.extend([
            '',
            f'/* {source} (sha256 {hashlib.sha256(path.read_bytes()).hexdigest()}), {len(words)} words. */',
            f'static const uint32_t {symbol}[{len(words)}] = {{',
        ])
        for offset in range(0, len(words), 6):
            lines.append('\t' + ', '.join(f'0x{word:08x}U' for word in words[offset:offset + 6]) + ',')
        lines.append('};')
    lines.append('')
    (fixtures / 'executor-shaders-gen.inc').write_text('\n'.join(lines))


if __name__ == '__main__':
    main()

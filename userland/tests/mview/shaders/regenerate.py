#!/usr/bin/env python3
"""Compile mview's own shaders; no compiler implementation enters the base."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import hashlib
import json
import os
import pathlib
import stat
import struct
import subprocess
import tempfile

TOOL_TIMEOUT = 30

# Source file, shader stage, and the C array that carries its SPIR-V words.
SHADERS = (
    ('mview.vert', 'vertex', 'mview_vertex_shader'),
    ('mview.frag', 'fragment', 'mview_fragment_shader'),
    ('cutout.frag', 'fragment', 'mview_cutout_shader'),
    ('pixel.vert', 'vertex', 'mview_pixel_vertex_shader'),
    ('pixel.frag', 'fragment', 'mview_pixel_fragment_shader'),
    ('pixel-cutout.frag', 'fragment', 'mview_pixel_cutout_shader'),
)


def run_tool(arguments):
    """Bound every compiler/validator invocation, including version queries."""
    return subprocess.run(arguments, check=True, timeout=TOOL_TIMEOUT,
                          stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True).stdout.strip()


def staged_file(path, data, mode):
    """Prepare one complete sibling so replacement never exposes partial bytes."""
    descriptor, name = tempfile.mkstemp(prefix='.' + path.name + '-', dir=path.parent)
    temporary = pathlib.Path(name)
    try:
        with os.fdopen(descriptor, 'wb') as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        temporary.chmod(mode)
        return temporary
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def publish(outputs):
    """Replace the completed set and restore previous outputs on ordinary failure.

    Each file replacement is atomic. The set is not a filesystem-wide
    transaction: interrupted power or failed rollback can require recovery from
    the explicitly retained sibling backup paths reported in the exception.
    """
    staged, backups, originals = {}, {}, {}
    attempted, keep_backups = [], set()
    try:
        # Complete every write and backup before touching any public artifact.
        for path, data in outputs.items():
            old = path.read_bytes() if path.exists() else None
            originals[path] = old
            mode = stat.S_IMODE(path.stat().st_mode) if old is not None else 0o644
            staged[path] = staged_file(path, data, mode)
            if old is not None:
                backups[path] = staged_file(path, old, mode)
        try:
            for path in outputs:
                current = path.read_bytes() if path.exists() else None
                if current != originals[path]:
                    raise RuntimeError(f'generated output changed during staging: {path}')
                attempted.append(path)
                os.replace(staged[path], path)
        except BaseException as error:
            failures = []
            for path in reversed(attempted):
                try:
                    if path in backups:
                        os.replace(backups[path], path)
                    else:
                        path.unlink(missing_ok=True)
                except OSError as rollback_error:
                    backup = backups.get(path)
                    if backup is not None:
                        keep_backups.add(backup)
                    failures.append(f'{path}: {rollback_error}; backup={backup}')
            if failures:
                raise RuntimeError('shader publication failed; rollback needs recovery: ' +
                                   '; '.join(failures)) from error
            raise
    finally:
        for path in list(staged.values()) + list(backups.values()):
            if path not in keep_backups:
                path.unlink(missing_ok=True)


def generate(directory, compiler, validator):
    # Capture every input before invoking a tool; hashes describe these exact bytes.
    sources = {name: (directory / name).read_bytes() for name, _, _ in SHADERS}
    metadata = {
        'license': 'Zlib',
        'source': 'Original zedBSD GLSL; compiled artifact of the same source',
        'target_env': 'vulkan1.1',
        'target_spv': 'spv1.0',
        'compiler': run_tool([compiler, '--version']),
        'validator': run_tool([validator, '--version']),
        'shaders': {},
    }
    header = [
        '/*',
        ' * zedBSD',
        ' * Copyright (C) 2026 Awe Morris',
        ' *',
        ' * SPDX-License-Identifier: Zlib',
        ' */',
        '',
        '/*',
        ' * Generated from the original GLSL by shaders/regenerate.py.',
        ' */',
        '',
        '#ifndef MVIEW_SHADERS_H',
        '#define MVIEW_SHADERS_H',
        '',
        '#include <stdint.h>',
    ]
    outputs = {}
    with tempfile.TemporaryDirectory(prefix='mview-shaders-') as temporary:
        workspace = pathlib.Path(temporary)
        for name, stage, array in SHADERS:
            source = workspace / name
            source.write_bytes(sources[name])
            output = workspace / f'{name}.spv'
            run_tool([compiler, '--target-env=vulkan1.1', '--target-spv=spv1.0',
                      f'-fshader-stage={stage}', '-O0', str(source), '-o', str(output)])
            run_tool([validator, '--target-env', 'vulkan1.1', str(output)])
            binary = output.read_bytes()
            if len(binary) < 20 or len(binary) % 4 or len(binary) > 6000:
                raise ValueError('shader exceeds the bounded Venus command stream or has no SPIR-V header')
            words = struct.unpack(f'<{len(binary) // 4}I', binary)
            if words[:2] != (0x07230203, 0x00010000):
                raise ValueError('shader is not little-endian SPIR-V 1.0')
            header.extend([
                '',
                f'/* Immutable {stage} shader words compiled from {name}. */',
                f'static const uint32_t {array}[] = {{',
            ])
            for offset in range(0, len(words), 6):
                line = ', '.join(f'0x{word:08x}U' for word in words[offset:offset + 6])
                header.append('\t' + line + ',')
            header.append('};')
            outputs[directory / output.name] = binary
            metadata['shaders'][name] = {
                'stage': stage,
                'array': array,
                'source_sha256': hashlib.sha256(sources[name]).hexdigest(),
                'binary': output.name,
                'binary_sha256': hashlib.sha256(binary).hexdigest(),
                'bytes': len(binary),
            }
    header.extend(['', '#endif', ''])
    outputs[directory.parent / 'shaders.h'] = '\n'.join(header).encode()
    outputs[directory / 'provenance.json'] = (json.dumps(metadata, indent=2) + '\n').encode()
    # Refuse to publish an old snapshot beside newly edited source files.
    for name, snapshot in sources.items():
        if (directory / name).read_bytes() != snapshot:
            raise RuntimeError('GLSL changed during compilation; no outputs were published')
    publish(outputs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--glslc', default='glslc')
    parser.add_argument('--spirv-val', default='spirv-val')
    args = parser.parse_args()
    generate(pathlib.Path(__file__).resolve().parent, args.glslc, args.spirv_val)


if __name__ == '__main__':
    main()

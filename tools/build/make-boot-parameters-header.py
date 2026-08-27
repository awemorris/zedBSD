#!/usr/bin/env python3
"""Generate a C/assembler-preprocessor boot-parameter string header."""

from __future__ import annotations

import argparse
from pathlib import Path


TEXT_MAX = 3071


def quoted(data: bytes) -> str:
    result = ['"']
    for byte in data:
        if byte in (ord('"'), ord('\\')):
            result.append('\\' + chr(byte))
        else:
            result.append(chr(byte))
    result.append('"')
    return ''.join(result)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()

    data = args.input.read_bytes()
    if data.endswith(b'\r\n'):
        data = data[:-2]
    elif data.endswith(b'\n'):
        data = data[:-1]
    if len(data) > TEXT_MAX:
        raise SystemExit(f'boot parameters exceed {TEXT_MAX} bytes')
    for byte in data:
        if byte < 0x20 or byte > 0x7e:
            raise SystemExit('boot parameters must be printable ASCII')

    output = (
        '/* Generated; do not edit. */\n'
        '#ifndef ZEDBSD_IMAGE_BOOT_PARAMETERS_H\n'
        '#define ZEDBSD_IMAGE_BOOT_PARAMETERS_H\n'
        f'#define ZEDBSD_IMAGE_BOOT_PARAMETERS_LENGTH {len(data)}\n'
        f'#define ZEDBSD_IMAGE_BOOT_PARAMETERS_TEXT {quoted(data)}\n'
        '#endif\n'
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + '.tmp')
    temporary.write_text(output, encoding='ascii', newline='\n')
    temporary.replace(args.output)


if __name__ == '__main__':
    main()

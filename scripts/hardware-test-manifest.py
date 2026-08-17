#!/usr/bin/env python3
"""Create or validate reproducible zedBSD hardware-test evidence."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import pathlib
import subprocess
import sys

REQUIRED_CASES = {
    'pc98': ('boot', 'vfs', 'keyboard', 'timer', 'filesystem', 'graphics'),
    'amd64-uefi': ('boot', 'vfs', 'smp', 'timer', 'filesystem', 'dynamic-linker'),
}
VALID_RESULTS = {'PASS', 'FAIL', 'SKIP', 'BLOCKED'}


def digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def git_output(root: pathlib.Path, *arguments: str) -> str:
    return subprocess.check_output(('git', '-C', str(root), *arguments),
                                   text=True).strip()


def create(args) -> None:
    root = pathlib.Path(args.root).resolve()
    image = pathlib.Path(args.image).resolve()
    if not image.is_file():
        raise ValueError(f'image does not exist: {image}')
    manifest = {
        'schema': 1,
        'kind': 'zedbsd-hardware-test-package',
        'machine_class': args.machine_class,
        'revision': git_output(root, 'rev-parse', 'HEAD'),
        'dirty': bool(git_output(root, 'status', '--porcelain')),
        'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'image': {'name': image.name, 'bytes': image.stat().st_size,
                  'sha256': digest(image)},
        'required_cases': list(REQUIRED_CASES[args.machine_class]),
        'cold_boot_count': 3,
    }
    pathlib.Path(args.output).write_text(json.dumps(manifest, indent=2) + '\n',
                                         encoding='utf-8')


def validate(args) -> None:
    manifest = json.loads(pathlib.Path(args.manifest).read_text(encoding='utf-8'))
    result = json.loads(pathlib.Path(args.result).read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or result.get('schema') != 1:
        raise ValueError('unsupported evidence schema')
    if result.get('machine_class') != manifest.get('machine_class'):
        raise ValueError('machine class does not match manifest')
    if result.get('image_sha256') != manifest['image']['sha256']:
        raise ValueError('tested image hash does not match manifest')
    if not result.get('machine_id') or not result.get('operator'):
        raise ValueError('machine_id and operator are required')
    boots = result.get('cold_boots')
    if not isinstance(boots, list) or len(boots) < manifest['cold_boot_count'] or \
            any(entry.get('result') != 'PASS' or not entry.get('evidence')
                for entry in boots):
        raise ValueError('three evidenced cold-boot PASS records are required')
    cases = result.get('cases')
    if not isinstance(cases, dict):
        raise ValueError('cases must be an object')
    for case in manifest['required_cases']:
        record = cases.get(case)
        if not isinstance(record, dict) or record.get('result') not in VALID_RESULTS:
            raise ValueError(f'missing/invalid result for case {case}')
        if not record.get('evidence'):
            raise ValueError(f'case {case} has no evidence reference')
        if record['result'] == 'SKIP' and not record.get('reason'):
            raise ValueError(f'case {case} SKIP requires a reason')
    print('zedBSD hardware evidence: VALID')


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest='command', required=True)
    make = subparsers.add_parser('create')
    make.add_argument('--root', required=True)
    make.add_argument('--image', required=True)
    make.add_argument('--machine-class', choices=sorted(REQUIRED_CASES), required=True)
    make.add_argument('--output', required=True)
    check = subparsers.add_parser('validate')
    check.add_argument('--manifest', required=True)
    check.add_argument('--result', required=True)
    args = parser.parse_args()
    try:
        (create if args.command == 'create' else validate)(args)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f'hardware evidence: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

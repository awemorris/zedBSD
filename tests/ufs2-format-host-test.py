#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
from __future__ import annotations

import importlib.util
import pathlib
import tempfile

from ufs2_format import create

checker_path = (pathlib.Path(__file__).parents[1] / 'tools' / 'build' /
                'check-ufs2-image.py')
spec = importlib.util.spec_from_file_location('_ufs2_checker', checker_path)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def run():
    with tempfile.TemporaryDirectory() as temporary:
        temporary = pathlib.Path(temporary)
        root = temporary / 'root'
        (root / 'bin').mkdir(parents=True)
        (root / 'bin' / 'hello').write_bytes(b'hello\n')
        (root / 'large').write_bytes(bytes(range(251)) * 400)
        image_path = temporary / 'ufs2.img'
        first = create(16 * 1024 * 1024, root, 4)
        second = create(16 * 1024 * 1024, root, 4)
        # Image layout is deterministic, while each filesystem intentionally
        # receives a fresh fs_id just like newfs(8).
        first_layout = bytearray(first)
        second_layout = bytearray(second)
        fsid = slice(65536 + 144, 65536 + 152)
        first_id = bytes(first_layout[fsid])
        second_id = bytes(second_layout[fsid])
        assert first_id != second_id
        # UFS2 records the ID in every cylinder group's backup superblock.
        # Normalize all four copies before comparing the deterministic layout.
        assert first_layout.count(first_id) == 4
        assert second_layout.count(second_id) == 4
        first_layout = first_layout.replace(first_id, b'\0' * 8)
        second_layout = second_layout.replace(second_id, b'\0' * 8)
        assert first_layout == second_layout
        image_path.write_bytes(first)
        checker.check(image_path)
        fs = checker.UFS2(first)
        assert fs.read_file(fs.lookup('/bin/hello')) == b'hello\n'
        assert len(fs.read_file(fs.lookup('/large'))) == 251 * 400
        damaged = bytearray(first)
        damaged[65536 + 1372] ^= 1
        try:
            checker.UFS2(bytes(damaged))
        except ValueError:
            pass
        else:
            raise AssertionError('bad UFS2 magic accepted')
    print('zedBSD UFS2 formatter/checker tests: PASS')


if __name__ == '__main__':
    run()

#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
from __future__ import annotations

import importlib.util
import pathlib
import tempfile

from ufs2_format import create

checker_path = pathlib.Path(__file__).parents[1] / 'scripts' / 'check-ufs2-image.py'
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
        assert first == second
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

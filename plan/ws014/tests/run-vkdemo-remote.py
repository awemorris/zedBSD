#!/usr/bin/env python3
"""Build an isolated vkdemo image and verify fixed and monotonic 3D frames.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import importlib.util
from pathlib import Path
import sys

_spec = importlib.util.spec_from_file_location('venus_remote', Path(__file__).with_name('run-venus-remote.py'))
common = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(common)

if __name__ == '__main__':
    sys.exit(common.main(profile='vkdemo'))

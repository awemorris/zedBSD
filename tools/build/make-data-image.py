#!/usr/bin/env python3
"""Create the persistent writable UFS1 upper image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import os
import tempfile
from pathlib import Path

from overlay_journal_format import JOURNAL_BYTES, empty_active_slot, self_test
from ufs1_format import create


def build(args: argparse.Namespace) -> None:
    self_test()
    if args.size_mib < 16:
        raise SystemExit("data UFS1 image must be at least 16 MiB")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="zedbsd-data-") as work_text:
        root = Path(work_text)
        (root / ".zovl0").write_bytes(empty_active_slot("overlay"))
        (root / ".zovl1").write_bytes(bytes(JOURNAL_BYTES))
        temporary = args.output.with_name(args.output.name + ".tmp")
        try:
            temporary.write_bytes(create(args.size_mib * 1024 * 1024, root))
            os.replace(temporary, args.output)
        finally:
            if temporary.exists():
                temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size-mib", type=int, default=32)
    build(parser.parse_args())


if __name__ == "__main__":
    main()

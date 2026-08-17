#!/usr/bin/env python3
"""Pure host tests for the XEiJ replacement-ROM decoder."""

import importlib.util
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "prepare-x68k-emulator-roms.py"
SPEC = importlib.util.spec_from_file_location("x68k_emulator_roms", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def expect_failure(source: str) -> None:
    try:
        MODULE.decode_xeij_scsiinrom(source)
    except ValueError:
        return
    raise AssertionError("invalid XEiJ source was accepted")


def main() -> None:
    source = (
        'class ROM { public static final byte[] SCSI16IN = '
        '"\\0\\374JSCSIIN\\0A\\377".getBytes(XEiJ.ISO_8859_1); }'
    )
    image = MODULE.decode_xeij_scsiinrom(source)
    assert len(image) == 8192
    assert image[:12] == b"\x00\xfcJSCSIIN\x00A\xff"
    assert image[12:] == b"\xff" * (8192 - 12)
    expect_failure("class ROM {}")
    expect_failure(
        'public static final byte[] SCSI16IN = "bad".getBytes(X);'
    )
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        assert MODULE.checked_output(root / "external", root / "repo") == (
            root / "external"
        ).resolve()
        try:
            MODULE.checked_output(root / "repo" / "roms", root / "repo")
        except SystemExit:
            pass
        else:
            raise AssertionError("repository-contained output was accepted")
    print("X68k emulator replacement-ROM host tests passed")


if __name__ == "__main__":
    main()

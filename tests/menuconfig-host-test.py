#!/usr/bin/env python3
"""Host regression tests for the first-run build menu."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent


def run(*arguments: str, stdout=None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        cwd=REPO,
        check=True,
        text=True,
        stdout=stdout,
    )


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="zedbsd-menuconfig-") as temporary:
        directory = Path(temporary)
        missing = directory / "missing.mk"
        output = directory / "config.mk"

        packages = run(
            "make",
            "--no-print-directory",
            f"ZEDBSD_CONFIG={missing}",
            "list-user-programs",
            stdout=subprocess.PIPE,
        ).stdout
        assert "awk|awk|*|y|base||" in packages

        run(
            "python3",
            "tools/menuconfig.py",
            "--output",
            str(output),
            "--defaults",
        )
        configuration = output.read_text(encoding="utf-8")
        assert "ZEDBSD_PLATFORM := i386\n" in configuration
        assert "ZEDBSD_ARCHITECTURE := i386\n" in configuration
        assert "ZEDBSD_BOARD := pcat\n" in configuration
        assert "ZEDBSD_USER_PROGRAMS := " in configuration

        run(
            "make",
            "--no-print-directory",
            "-n",
            f"ZEDBSD_CONFIG={output}",
            "vmunix",
            stdout=subprocess.DEVNULL,
        )

    print("zedBSD menuconfig host test: PASS")


if __name__ == "__main__":
    main()

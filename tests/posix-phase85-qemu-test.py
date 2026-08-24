#!/usr/bin/env python3
"""Exercise Phase 8.5 terminal packages in an amd64 zedBSD guest."""

import argparse
from pathlib import Path
import subprocess
import tempfile
import time


EXPECTED = (
    "zedBSD-POSIX-PHASE85-QEMU-START",
    "zedBSD-POSIX-PHASE85-TIC-PASS",
    "zedBSD-POSIX-PHASE85-INFOCMP-PASS",
    "zedBSD-POSIX-PHASE85-TPUT-PASS",
    "zedBSD-POSIX-PHASE85-CURSES-PASS",
    "zedBSD-POSIX-PHASE85-QEMU-PASS",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=75)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="zedbsd-posix-phase85-qemu-") as work:
        log = Path(work) / "debugcon.log"
        process = subprocess.Popen((
            args.qemu, "-machine", "pc", "-m", "512", "-smp", "4",
            "-display", "none", "-serial", "none", "-monitor", "none",
            "-no-reboot", "-debugcon", f"file:{log}",
            "-drive", f"file={args.image.resolve()},format=raw,if=ide", "-boot", "c",
        ))
        deadline = time.monotonic() + args.timeout
        output = ""
        while time.monotonic() < deadline:
            output = log.read_text(errors="replace") if log.exists() else ""
            missing = [marker for marker in EXPECTED if marker not in output]
            if not missing:
                process.terminate()
                process.wait(timeout=5)
                break
            if process.poll() is not None:
                raise SystemExit(
                    f"QEMU exited; missing: {', '.join(missing)}\n" + output[-12000:]
                )
            time.sleep(0.05)
        else:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            missing = [marker for marker in EXPECTED if marker not in output]
            raise SystemExit(
                f"QEMU timed out; missing: {', '.join(missing)}\n" + output[-12000:]
            )
    print("zedBSD POSIX Phase 8.5 amd64 QEMU test: PASS")


if __name__ == "__main__":
    main()

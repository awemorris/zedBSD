#!/usr/bin/env python3
"""Exercise Phase 4 parser, traversal, editor, and archive tools."""

import argparse
from pathlib import Path
import subprocess
import tempfile
import time


EXPECTED = (
    "zedBSD-POSIX-PHASE4-QEMU-START",
    "zedBSD-POSIX-PHASE4-BC-PASS",
    "zedBSD-POSIX-PHASE4-ED-PASS",
    "zedBSD-POSIX-PHASE4-FIND-PASS",
    "zedBSD-POSIX-PHASE4-M4-PASS",
    "zedBSD-POSIX-PHASE4-PAX-PASS",
    "zedBSD-POSIX-PHASE4-QEMU-PASS",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()
    if not args.image.is_file():
        raise SystemExit(f"QEMU image is missing: {args.image}")

    with tempfile.TemporaryDirectory(prefix="zedbsd-posix-phase4-qemu-") as work:
        log = Path(work) / "debugcon.log"
        command = (
            args.qemu,
            "-machine", "pc",
            "-m", "512",
            "-smp", "4",
            "-display", "none",
            "-serial", "none",
            "-monitor", "none",
            "-no-reboot",
            "-debugcon", f"file:{log}",
            "-drive", f"file={args.image.resolve()},format=raw,if=ide",
            "-boot", "c",
        )
        process = subprocess.Popen(command)
        deadline = time.monotonic() + args.timeout
        output = ""
        while time.monotonic() < deadline:
            output = log.read_text(errors="replace") if log.exists() else ""
            missing = [marker for marker in EXPECTED if marker not in output]
            if not missing:
                process.terminate()
                process.wait(timeout=5)
                break
            status = process.poll()
            if status is not None:
                raise SystemExit(
                    f"QEMU exited with status {status}; missing: {', '.join(missing)}\n"
                    + output[-10000:]
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
                f"QEMU timed out; missing: {', '.join(missing)}\n" + output[-10000:]
            )
    print("zedBSD POSIX Phase 4 amd64 QEMU test: PASS")


if __name__ == "__main__":
    main()

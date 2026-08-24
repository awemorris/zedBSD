#!/usr/bin/env python3
"""Exercise Phase 2 commands in headless amd64 zedBSD."""

import argparse
from pathlib import Path
import subprocess
import tempfile
import time


EXPECTED = (
    "zedBSD-POSIX-PHASE2-QEMU-START",
    "zedBSD-POSIX-PHASE2-EXPR-PASS",
    "zedBSD-POSIX-PHASE2-CAL-PASS",
    "zedBSD-POSIX-PHASE2-TSORT-PASS",
    "Q2F0Cg==",
    "zedBSD-POSIX-PHASE2-UUENCODE-PASS",
    "zedBSD-POSIX-PHASE2-UUDECODE-PASS",
    "zedBSD-POSIX-PHASE2-NICE-PASS",
    "zedBSD-POSIX-PHASE2-RENICE-PASS",
    "zedBSD-POSIX-PHASE2-HASH-PASS",
    "zedBSD-POSIX-PHASE2-ULIMIT-PASS",
    "zedBSD-POSIX-PHASE2-GETCONF-PASS",
    "zedBSD-POSIX-PHASE2-WRITE-NEGATIVE-PASS",
    "zedBSD-POSIX-PHASE2-QEMU-PASS",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=45)
    args = parser.parse_args()
    if not args.image.is_file():
        raise SystemExit(f"QEMU image is missing: {args.image}")

    with tempfile.TemporaryDirectory(prefix="zedbsd-posix-phase2-qemu-") as work:
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
        missing = list(EXPECTED)
        while time.monotonic() < deadline:
            output = (
                log.read_text(encoding="utf-8", errors="replace")
                if log.exists()
                else ""
            )
            missing = [marker for marker in EXPECTED if marker not in output]
            if not missing:
                process.terminate()
                process.wait(timeout=5)
                break
            status = process.poll()
            if status is not None:
                raise SystemExit(
                    f"QEMU exited with status {status}; missing markers: "
                    + ", ".join(missing)
                    + "\n"
                    + output[-8000:]
                )
            time.sleep(0.05)
        else:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            raise SystemExit(
                "QEMU timed out before all completion markers; missing: "
                + ", ".join(missing)
                + "\n"
                + output[-8000:]
            )
    print("zedBSD POSIX Phase 2 amd64 QEMU test: PASS")


if __name__ == "__main__":
    main()

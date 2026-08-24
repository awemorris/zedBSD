#!/usr/bin/env python3
"""Run the deferred provider commands in headless amd64 zedBSD."""

import argparse
from pathlib import Path
import subprocess
import tempfile
import time


EXPECTED = (
    "zedBSD-POSIX-DEFERRED-QEMU-START",
    "at: job scheduling service is unavailable",
    "batch: batch scheduling service is unavailable",
    "crontab: periodic scheduling service is unavailable",
    "logger: system logging facility is unavailable",
    "mailx: mail provider is not installed",
    "talk: talk rendezvous service is unavailable",
    "lp: no print destination is configured",
    "zedBSD-POSIX-DEFERRED-QEMU-PASS",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=45)
    args = parser.parse_args()
    if not args.image.is_file():
        raise SystemExit(f"QEMU image is missing: {args.image}")

    with tempfile.TemporaryDirectory(prefix="zedbsd-deferred-qemu-") as work:
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
            output = log.read_text(encoding="utf-8", errors="replace") \
                if log.exists() else ""
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
            )
    print("zedBSD deferred command amd64 QEMU test: PASS")


if __name__ == "__main__":
    main()

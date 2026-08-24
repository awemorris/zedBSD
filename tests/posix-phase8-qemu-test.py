#!/usr/bin/env python3
"""Exercise the Phase 8 SCCS suite in an amd64 zedBSD guest."""

import argparse
from pathlib import Path
import subprocess
import tempfile
import time


EXPECTED = (
    "zedBSD-POSIX-PHASE8-QEMU-START",
    "zedBSD-POSIX-PHASE8-ADMIN-VAL-PASS",
    "zedBSD-POSIX-PHASE8-GET-PASS",
    "zedBSD-POSIX-PHASE8-DELTA-PRS-PASS",
    "zedBSD-POSIX-PHASE8-WHAT-PASS",
    "zedBSD-POSIX-PHASE8-EDIT-PASS",
    "zedBSD-POSIX-PHASE8-DISPATCH-PASS",
    "zedBSD-POSIX-PHASE8-RMDEL-GUARD-PASS",
    "zedBSD-POSIX-PHASE8-QEMU-PASS",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=75)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="zedbsd-posix-phase8-qemu-") as work:
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
                raise SystemExit(f"QEMU exited; missing: {', '.join(missing)}\n" + output[-12000:])
            time.sleep(0.05)
        else:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            missing = [marker for marker in EXPECTED if marker not in output]
            raise SystemExit(f"QEMU timed out; missing: {', '.join(missing)}\n" + output[-12000:])
    print("zedBSD POSIX Phase 8 amd64 QEMU test: PASS")


if __name__ == "__main__":
    main()

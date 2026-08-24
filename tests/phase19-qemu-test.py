#!/usr/bin/env python3
"""Exercise Phase 19 service integration in an amd64 zedBSD guest."""

import argparse
from pathlib import Path
import subprocess
import tempfile
import time


EXPECTED = (
    "zedBSD-PHASE19-QEMU-START",
    "zedBSD-PHASE19-SHELL-PASS",
    "zedBSD-PHASE19-SERVICE-LIST-PASS",
    "zedBSD-PHASE19-SERVICE-STATUS-PASS",
    "zedBSD-PHASE19-NETWORK-PASS",
    "zedBSD-PHASE19-NET-CONTROL-PASS",
    "zedBSD-PHASE19-SYSLOG-PASS",
    "zedBSD-PHASE19-DISABLE-PASS",
    "zedBSD-PHASE19-ENABLE-PASS",
    "zedBSD-PHASE19-AT-PASS",
    "zedBSD-PHASE19-CRONTAB-PASS",
    "zedBSD-PHASE19-QEMU-PASS",
    "init: stopping services",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=75)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="zedbsd-phase19-qemu-") as work:
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
            if process.poll() is not None:
                raise SystemExit(
                    f"QEMU exited; missing: {', '.join(missing)}\n" + output[-16000:]
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
                f"QEMU timed out; missing: {', '.join(missing)}\n" + output[-16000:]
            )
    print("zedBSD Phase 19 amd64 QEMU test: PASS")


if __name__ == "__main__":
    main()

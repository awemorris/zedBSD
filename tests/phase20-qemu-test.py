#!/usr/bin/env python3
"""Exercise Phase 20 networking and readiness in an amd64 zedBSD guest."""

import argparse
from pathlib import Path
import subprocess
import tempfile
import time


EXPECTED = (
    "init: started networkd pid",
    "net: configuring lo0",
    "net: configuring ne0",
    "login:",
    "zedBSD-PHASE20-QEMU-START",
    "zedBSD-PHASE20-NETWORKD-READY-PASS",
    "zedBSD-PHASE20-NET-SERVICE-PASS",
    "zedBSD-PHASE20-LOOPBACK-PASS",
    "zedBSD-PHASE20-DHCP-ADDRESS-PASS",
    "zedBSD-PHASE20-DHCP-ROUTE-PASS",
    "zedBSD-PHASE20-DNS-PASS",
    "zedBSD-PHASE20-DHCPC-IDENTITY-PASS",
    "zedBSD-PHASE20-DHCPC-ONESHOT-PASS",
    "zedBSD-PHASE20-NET-CONTROL-PASS",
    "zedBSD-PHASE20-DIRECT-IFCONFIG-PASS",
    "zedBSD-PHASE20-RESTART-PASS",
    "zedBSD-PHASE20-QEMU-PASS",
    "init: stopping services",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=100)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="zedbsd-phase20-qemu-") as work:
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
            "-netdev", "user,id=net0",
            "-device", "ne2k_isa,netdev=net0,iobase=0x300,irq=10",
            "-boot", "c",
        )
        process = subprocess.Popen(command)
        deadline = time.monotonic() + args.timeout
        output = ""
        while time.monotonic() < deadline:
            output = log.read_text(errors="replace") if log.exists() else ""
            missing = [marker for marker in EXPECTED if marker not in output]
            if not missing:
                assert output.index("init: started networkd pid") < output.index(
                    "net: configuring lo0"
                )
                assert output.index("net: configuring ne0") < output.index("login:")
                process.terminate()
                process.wait(timeout=5)
                break
            if process.poll() is not None:
                raise SystemExit(
                    f"QEMU exited; missing: {', '.join(missing)}\n" + output[-20000:]
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
                f"QEMU timed out; missing: {', '.join(missing)}\n" + output[-20000:]
            )
    print("zedBSD Phase 20 amd64 QEMU test: PASS")


if __name__ == "__main__":
    main()

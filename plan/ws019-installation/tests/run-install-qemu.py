#!/usr/bin/env python3
"""Exercise the installer orchestration unchanged, with real console approval."""
import json
from pathlib import Path
import re
import runpy
import struct
import sys
import time
import zlib

HERE = Path(__file__).resolve().parent
BASE = runpy.run_path(str(HERE / "run-transaction-qemu.py"))
F = BASE["fixture"]
REPO = BASE["REPO"]

class PublicInstallGuest(BASE["TransactionGuest"]):
    def source_continue(self, start):
        self.wait("Enter: Continue    Esc: Cancel", start, 120)
        self.proc.stdin.write("sendkey ret\n")
        self.proc.stdin.flush()

    def run_installer(self, command, expected, status=0):
        start = len(self.text())
        self.send(command + "; echo storage-result-$?")
        self.source_continue(start)
        text = self.command_status(start, status)
        assert re.search(expected, text, re.M), text
        return text

    def capture_screen(self, name):
        path = self.output / (name + ".ppm")
        assert not path.exists()
        self.proc.stdin.write("screendump " + json.dumps(str(path)) + "\n")
        self.proc.stdin.flush()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                # QEMU screendump emits binary PPM with a three-line header.
                magic, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
                width, height = map(int, dimensions.split())
                if magic != b"P6" or maximum != b"255" or len(pixels) != width * height * 3:
                    raise ValueError("incomplete QEMU PPM")
                if not (0 < width <= 16384 and 0 < height <= 16384):
                    raise ValueError("invalid QEMU framebuffer dimensions")

                def chunk(kind, data):
                    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

                rows = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
                png = b"\x89PNG\r\n\x1a\n"
                png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                png += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
                path.with_suffix(".png").write_bytes(png)
                return str(path.with_suffix(".png"))
            except (OSError, ValueError):
                if self.proc.poll() is not None:
                    raise RuntimeError("QEMU exited before screenshot completed")
                time.sleep(0.1)
        raise RuntimeError("QEMU framebuffer screenshot did not complete")

    def command_status(self, start, status=0):
        text = self.prompt(start)
        assert re.search(rf"^storage-result-{status}$", text, re.M), text
        return text

    def run(self, command, expected=None, status=0):
        start = len(self.text())
        self.send(command)
        text = self.prompt(start)
        status_start = len(self.text())
        self.send("echo storage-result-$?")
        self.command_status(status_start, status)
        if expected:
            assert re.search(expected, text, re.M), text
        return text


def main():
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019-installation/temp")
    out.mkdir(parents=True, exist_ok=False)
    result = F["prepare_boot"](out, False, False,
        REPO / "build/arch-images/amd64.ufs")
    F["create_nvme"](out / "gpt.img", sectors=10485760)
    BASE["add_discovery_payload"](out / "gpt.img")
    before = F["protected_bytes"](out / "gpt.img")
    payload = BASE["payload_marker"](out / "gpt.img")
    guest = PublicInstallGuest(out, usb_boot=True)
    guest.deadline = time.monotonic() + 3600
    result["cases"] = []
    result["screenshots"] = []
    result["result"] = "FAIL installer orchestration"
    try:
        guest.login()
        guest.run("ls -l /bin/sh")
        guest.run("ls -l /bin/noct")
        guest.run("ls -l /dev/null")
        guest.run("ls -l /dev/zero")
        guest.run("sh -c 'exit 42'", status=42)
        guest.run("cat /dev/null")
        guest.run("dd if=/dev/zero of=/run/conflict-byte bs=1 count=1")
        guest.run("echo discard > /dev/null")
        guest.run("zedinst", "usage: zedinst", status=1)
        guest.run("zedinst nvme0n1 nvme0n1p2 < /dev/null", "interactive terminal", status=1)
        guest.run_installer("zedinst sda sda2", "cannot reload the requested installation destination", status=1)
        result["vars_before"] = F["digest"](out / "vars.fd")
        cancel_before = F["digest"](out / "gpt.img")
        for case in ("cancel", "install", "rerun"):
            start = len(guest.text())
            guest.send("zedinst nvme0n1 nvme0n1p2; echo storage-result-$?")
            guest.source_continue(start)
            text, _ = guest.wait(r"Type exactly: INSTALL nvme0n1 nvme0n1p2|root@[^\s]*:[^\n]*\$ ?$", start, 1100)
            assert "Type exactly: INSTALL nvme0n1 nvme0n1p2" in text, text
            result["screenshots"].append(guest.capture_screen("installer-" + case))
            if case == "cancel":
                guest.proc.stdin.write("sendkey esc\n")
                guest.proc.stdin.flush()
                expected = "Installation cancelled."
            else:
                guest.send("INSTALL nvme0n1 nvme0n1p2")
                expected = "Installation complete."
            guest.wait(re.escape(expected) + r"|root@[^\s]*:[^\n]*\$ ?$", start, 1100)
            text = guest.command_status(start)
            assert expected in text, text
            if case == "cancel":
                assert F["digest"](out / "gpt.img") == cancel_before, "cancel changed destination"
            result["cases"].append(case)
        # Change exactly the ELF magic's first byte, then prove refusal preserves it.
        guest.run("mkdir /run/verify-installed")
        guest.run("mount -t fat nvme0n1p2 /run/verify-installed")
        guest.run("/bin/cp -T --update=none-fail /run/verify-installed/vmunix /run/kernel-backup")
        guest.run("dd if=/run/conflict-byte of=/run/verify-installed/vmunix bs=1 count=1 conv=notrunc")
        corrupted = guest.run("cksum -a sha256 /run/verify-installed/vmunix")
        digest = re.search(r"^([0-9a-f]{64})  /run/verify-installed/vmunix$", corrupted, re.M)
        assert digest, corrupted
        guest.run("umount /run/verify-installed")
        guest.run_installer("zedinst nvme0n1 nvme0n1p2",
                  "installation incomplete", status=1)
        guest.run("mount -t fat nvme0n1p2 /run/verify-installed")
        unchanged = guest.run("cksum -a sha256 /run/verify-installed/vmunix")
        assert digest.group(1) in unchanged, unchanged
        guest.run("/bin/cp -T /run/kernel-backup /run/verify-installed/vmunix")
        guest.run("cmp /run/kernel-backup /run/verify-installed/vmunix")
        guest.run("sync /run/verify-installed/vmunix")
        guest.run("umount /run/verify-installed")
        guest.run("rmdir /run/verify-installed")
        guest.run("rm /run/kernel-backup")
        guest.run("rm /run/conflict-byte")
        result["cases"].append("single-byte conflict preserves destination; original restored")
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["guest_result"] = "PASS cancel install rerun"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        guest.stop()
        result["protected_before"] = before
        result["protected_after"] = F["protected_bytes"](out / "gpt.img")
        result["payload_before"] = payload
        result["payload_after"] = BASE["payload_marker"](out / "gpt.img")
        result["vars_after"] = F["digest"](out / "vars.fd")
        result["production_sha256_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert before == result["protected_after"]
    assert payload == result["payload_after"]
    assert result["vars_before"] == result["vars_after"]
    assert result["production_sha256"] == result["production_sha256_after"]
    result["result"] = "PASS installer cancel install rerun"
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(result["result"])


if __name__ == "__main__":
    main()

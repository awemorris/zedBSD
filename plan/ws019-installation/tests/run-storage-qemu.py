#!/usr/bin/env python3
"""Q076/q077 bounded production QEMU acceptance; only fresh disposable copies."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time
import uuid
import zlib

REPO = Path(__file__).resolve().parents[3]
TYPE = "0fc63daf-8483-4772-8e79-3d69d8477de4"
PART1 = "76190000-1111-4111-8111-111111111111"
PART2 = "76190000-2222-4222-8222-222222222222"
SECTORS = 196608


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def fixture(path, gpt):
    """Independent fixture constructor, not a production table initializer."""
    mbr = bytearray(512)
    mbr[510:512] = b"\x55\xaa"
    struct.pack_into("<I", mbr, 440, 0x76190000 + int(gpt))
    mbr[450] = 0xEE if gpt else 0x83
    struct.pack_into("<II", mbr, 454, 1 if gpt else 2048,
                     SECTORS - 1 if gpt else 131072)
    with path.open("xb") as f:
        f.truncate(SECTORS * 512)
        f.write(mbr)
        if not gpt:
            return
        entries = bytearray(16384)
        entries[:16] = uuid.UUID(TYPE).bytes_le
        entries[16:32] = uuid.UUID(PART1).bytes_le
        struct.pack_into("<QQQ", entries, 32, 2048, 2048 + 131072 - 1, 0)
        entries[56:66] = "q076a".encode("utf-16le")
        for lba, alternate, table in [(1, SECTORS - 1, 2),
                                       (SECTORS - 1, 1, SECTORS - 33)]:
            header = bytearray(512)
            struct.pack_into("<8sIIIIQQQQ16sQIII", header, 0, b"EFI PART",
                             0x10000, 92, 0, 0, lba, alternate, 34,
                             SECTORS - 34, uuid.UUID("76190000-aaaa-4aaa-8aaa-aaaaaaaaaaaa").bytes_le,
                             table, 128, 128, zlib.crc32(entries))
            struct.pack_into("<I", header, 16, zlib.crc32(header[:92]))
            f.seek(lba * 512)
            f.write(header)
            f.seek(table * 512)
            f.write(entries)


class Guest:
    def __init__(self, output):
        self.output = output
        self.log = output / "guest.log"
        self.deadline = time.monotonic() + 600
        self.commands = (output / "commands.log").open("w")
        self.monitor = (output / "qemu.log").open("w")
        args = ["qemu-system-x86_64", "-machine", "pc", "-m", "512", "-smp", "4",
                "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                "-drive", f"if=pflash,format=raw,file={output / 'vars.fd'}",
                "-drive", f"file={output / 'boot.img'},format=raw,if=ide",
                "-drive", f"file={output / 'gpt.img'},format=raw,if=none,id=gpt",
                "-device", "nvme,drive=gpt,serial=q076gpt",
                "-drive", f"file={output / 'mbr.img'},format=raw,if=ide,index=1",
                "-nic", "none", "-display", "none", "-serial", "none",
                "-debugcon", f"file:{self.log}", "-monitor", "stdio"]
        self.commands.write(repr(args) + "\n")
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=self.monitor,
                                     stderr=subprocess.STDOUT, text=True)

    def text(self):
        return self.log.read_text(errors="replace").replace("\r", "") if self.log.exists() else ""

    def wait(self, pattern, start=0, timeout=40):
        end = min(self.deadline, time.monotonic() + timeout)
        while time.monotonic() < end:
            text = self.text()[start:]
            match = re.search(pattern, text, re.M)
            if match:
                return text, match
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU exited before expected output")
            time.sleep(0.1)
        raise RuntimeError(f"timeout waiting for {pattern!r}: {self.text()[start:][-1200:]}")

    def send(self, text):
        self.commands.write(text + "\n")
        self.commands.flush()
        keys = {" ": "spc", "/": "slash", "-": "minus", ".": "dot",
                ":": "shift-semicolon", "$": "shift-4", "?": "shift-slash",
                "_": "shift-minus"}
        for char in text:
            if "a" <= char <= "z" or "0" <= char <= "9":
                key = char
            elif "A" <= char <= "Z":
                key = "shift-" + char.lower()
            else:
                key = keys[char]
            self.proc.stdin.write(f"sendkey {key}\n")
            self.proc.stdin.flush()
            time.sleep(0.015)
        self.proc.stdin.write("sendkey ret\n")
        self.proc.stdin.flush()

    def login(self, start=0):
        self.wait(r"login:\s*$", start, 120)
        at = len(self.text())
        self.send("root")
        self.wait("Password:", at)
        at = len(self.text())
        self.send("")
        self.prompt(at)

    def prompt(self, start):
        return self.wait(r"root@[^\s]*:[^\n]*\$ ?$", start)[0]

    def run(self, command, expected=None, status=0):
        at = len(self.text())
        self.send("storage-exit " + command)
        output = self.prompt(at)
        if expected and not re.search(expected, output, re.M):
            raise AssertionError(f"{command}: missing {expected!r}: {output}")
        if not re.search(rf"^storage-result-{status}$", output, re.M):
            raise AssertionError(f"{command}: wrong exit status: {output}")
        return output

    def edit(self, command, busy=False):
        at = len(self.text())
        self.send("storage-exit " + command)
        _, match = self.wait(r"Type '(WRITE [^']+)' to write:", at)
        self.send(match.group(1))
        output = self.prompt(at)
        expected = "Reboot required" if busy else "kernel partition devices reloaded"
        if expected not in output:
            raise AssertionError(output)
        if not re.search(rf"^storage-result-{3 if busy else 0}$", output, re.M):
            raise AssertionError(output)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.stdin.write("quit\n")
            self.proc.stdin.flush()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.monitor.close()
        self.commands.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("cell", choices=["idle", "busy", "combined"])
    parser.add_argument("output", type=Path)
    parser.add_argument("--mount-protection", action="store_true",
                        help="also run q077 exact-errno mounted namespace probes")
    args = parser.parse_args()
    output = args.output.resolve()
    # Fresh scope, under this Phase's disposable area; refuse existing paths.
    output.relative_to(REPO / "plan/ws019-installation/temp")
    output.mkdir(parents=True, exist_ok=False)
    source = REPO / "build/amd64/hdd-image.img"
    source_hash = digest(source)
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(source), str(output / "boot.img")], check=True)
    # Replace ROOTFS only in the disposable copy with the same production
    # packages plus the test-only waitpid observer. The ESP and configured
    # payload are distinct: resolve the unique existing rootfs, not slot 1.
    with source.open("rb") as boot:
        boot.seek(512)
        header = boot.read(512)
        assert header[:8] == b"EFI PART"
        entries_lba, slots, entry_size = struct.unpack_from("<QII", header, 72)
        assert 0 < slots <= 4096 and 128 <= entry_size <= 4096
        boot.seek(entries_lba * 512)
        entries = boot.read(slots * entry_size)
    payloads = []
    for slot in range(slots):
        entry = entries[slot * entry_size:(slot + 1) * entry_size]
        if entry[:16] == bytes(16):
            continue
        start, end = struct.unpack_from("<QQ", entry, 32)
        assert 34 <= start <= end < source.stat().st_size // 512
        found = subprocess.run(["mdir", "-i", f"{source}@@{start * 512}",
                                "::/rootfs.img"], capture_output=True)
        if found.returncode == 0:
            payloads.append(start)
    assert len(payloads) == 1, "refuse ambiguous boot payload"
    start = payloads[0]
    fixture_root = REPO / "build/arch-images/amd64-ws019-storage.ufs"
    assert fixture_root.is_file(), "build ws019-storage-qemu-fixture first"
    subprocess.run(["mcopy", "-o", "-i", f"{output / 'boot.img'}@@{start * 512}",
                    str(fixture_root), "::/rootfs.img"], check=True)
    copied = subprocess.run(["mtype", "-i", f"{output / 'boot.img'}@@{start * 512}",
                             "::/rootfs.img"], capture_output=True, check=True).stdout
    assert hashlib.sha256(copied).hexdigest() == digest(fixture_root)
    check = ["python3", "tools/build/check-arch-overlay-ufs.py", "--profile", "amd64",
             "--image", str(fixture_root), "--file",
             "/usr/bin/storage-exit=build/amd64/tests/storage-exit"]
    if args.mount_protection:
        check += ["--file", "/usr/bin/mount-protection=build/amd64/tests/mount-protection"]
    subprocess.run(check, cwd=REPO, check=True)
    shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", output / "vars.fd")
    fixture(output / "gpt.img", True)
    fixture(output / "mbr.img", False)
    subprocess.run(["mformat", "-i", f"{output / 'gpt.img'}@@1048576",
                    "-F", "-T", "131072", "-N", "76190000", "::"], check=True)
    before = {name: digest(output / name) for name in ["gpt.img", "mbr.img"]}
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(output / "gpt.img"), str(output / "gpt-before.img")], check=True)
    guest = Guest(output)
    try:
        guest.login()
        mounts = guest.run("mount", r" on / type ")
        devices = guest.run("diskpart list", "nvme0n1")
        guest.run("diskpart show nvme0n1", "1 active partitions")
        assert not re.search(r"^/dev/nvme0n1p\d+ on | on /disk\d+ ", mounts, re.M), mounts
        guest.run("ls /dev/nvme0n1p1")
        guest.run("diskpart reload nvme0n1", "Kernel partition devices reloaded")
        if args.cell in ("idle", "combined"):
            guest.edit(f"diskpart add nvme0n1 2 150000 4096 {TYPE} {PART2} q076b")
            guest.run("ls /dev/nvme0n1p2")
            guest.edit("diskpart delete nvme0n1 2")
            guest.edit("diskpart add sdb 2 150000 4096 83")
            guest.edit("diskpart delete sdb 2")
        if args.cell in ("busy", "combined"):
            # Current public mount API accepts only root-level targets.
            guest.run("mkdir -p /q076")
            guest.run("mount -t fat -r nvme0n1p1 /q076")
            guest.run("mount", r"/dev/nvme0n1p1 on /q076 type fat \(ro")
            if args.mount_protection:
                guest.run("mount-protection mounted /q076", "mount-protection PASS mounted")
            guest.run("diskpart reload nvme0n1", "Disk busy", 1)
            guest.edit(f"diskpart add nvme0n1 2 150000 4096 {TYPE} {PART2} q076b", busy=True)
            guest.run("diskpart show nvme0n1", "2 active partitions")
            guest.run("ls /dev/nvme0n1p2", status=1)
            guest.run("ls /q076")
            guest.run("umount /q076")
            if args.mount_protection:
                guest.run("mount-protection released /q076", "mount-protection PASS released")
                # Existing mount API also permits a virtual name with no
                # underlying directory. It must receive the same protection.
                guest.run("mount -t fat -r nvme0n1p1 /q077-virtual")
                guest.run("mount-protection mounted /q077-virtual", "mount-protection PASS mounted")
                guest.run("umount /q077-virtual")
                guest.run("ls /q077-virtual", status=1)
            guest.run("mount -t fat nvme0n1p1 /q076")
            if args.mount_protection:
                guest.run("mount-protection mounted /q076", "mount-protection PASS mounted")
            guest.run("diskpart reload nvme0n1", "Disk busy", 1)
            # Actual ordinary boot disk (not the auxiliary NVMe namespaces).
            match = re.search(r"^(sd[a-z]+) \d+ \d+ \d+ (?:rw|ro)$", devices, re.M)
            if not match:
                raise AssertionError("missing boot disk identity in diskpart list")
            guest.run(f"diskpart reload {match.group(1)}", "Disk busy", 1)
            at = len(guest.text())
            guest.send("reboot")
            guest.login(at)
            mounts = guest.run("mount", r" on / type overlay ")
            assert not re.search(r"^/dev/nvme0n1p\d+ on | on /disk\d+ ", mounts, re.M), mounts
            guest.run("ls /dev/nvme0n1p2")
            guest.run("diskpart show nvme0n1", "2 active partitions")
        if re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I):
            raise AssertionError("fatal kernel pattern in log")
    finally:
        guest.stop()
    assert digest(source) == source_hash, "production input was changed"
    assert digest(output / "mbr.img") == before["mbr.img"], "MBR edit/delete did not round-trip"
    if args.cell == "idle":
        assert digest(output / "gpt.img") == before["gpt.img"], "GPT edit/delete did not round-trip"
    else:
        # All bytes outside primary/backup headers/arrays must be preserved.
        with (output / "gpt.img").open("rb") as a, (output / "gpt-before.img").open("rb") as b:
            for sector in range(SECTORS):
                left, right = a.read(512), b.read(512)
                if not (1 <= sector < 34 or SECTORS - 33 <= sector < SECTORS):
                    assert left == right, f"non-table sector changed: {sector}"
    (output / "result.txt").write_text(
        f"PASS {args.cell}\nmount_protection={args.mount_protection}\nproduction_sha256={source_hash}\n")
    print(f"PASS {args.cell}: {output}", flush=True)


if __name__ == "__main__":
    main()

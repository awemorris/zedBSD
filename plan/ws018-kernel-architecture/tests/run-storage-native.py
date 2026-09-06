#!/usr/bin/env python3
"""q086 native acceptance on one disposable disk, two grouped boots."""
import hashlib
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("storage_qemu",
    REPO / "plan/ws019-installation/tests/run-storage-qemu.py")
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)

class USBGuest(base.Guest):
    def __init__(self, output):
        self.output = output
        self.log = output / "guest.log"
        self.deadline = time.monotonic() + 600
        self.commands = (output / "commands.log").open("w")
        self.monitor = (output / "qemu.log").open("w")
        args = ["qemu-system-x86_64", "-machine", "q35", "-m", "512", "-smp", "4",
                "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                "-drive", f"if=pflash,format=raw,file={output / 'vars.fd'}",
                "-device", "qemu-xhci,id=xhci",
                "-drive", f"if=none,id=boot,format=raw,file={output / 'boot.img'}",
                "-device", "usb-storage,bus=xhci.0,drive=boot,bootindex=1",
                "-nic", "none", "-display", "none", "-serial", "none",
                "-debugcon", f"file:{self.log}", "-monitor", "stdio"]
        self.commands.write(repr(args) + "\n")
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=self.monitor,
                                     stderr=subprocess.STDOUT, text=True)

def main():
    output = Path(sys.argv[1]).resolve()
    output.relative_to(REPO / "plan/ws018-kernel-architecture/temp")
    output.mkdir(parents=True, exist_ok=False)
    source = REPO / "build/amd64/hdd-image.img"
    original = base.digest(source)
    subprocess.run(["cp", "--reflink=auto", "--sparse=always",
                    str(source), str(output / "boot.img")], check=True)
    with source.open("rb") as stream:
        stream.seek(512)
        header = stream.read(512)
        assert header[:8] == b"EFI PART"
        lba, slots, width = struct.unpack_from("<QII", header, 72)
        assert 0 < slots <= 4096 and 128 <= width <= 4096
        stream.seek(lba * 512)
        entries = stream.read(slots * width)
    candidates = []
    for i in range(slots):
        entry = entries[i * width:(i + 1) * width]
        if entry[:16] == bytes(16):
            continue
        first, last = struct.unpack_from("<QQ", entry, 32)
        assert 34 <= first <= last < source.stat().st_size // 512
        if subprocess.run(["mdir", "-i", f"{source}@@{first * 512}",
                           "::/rootfs.img"], capture_output=True).returncode == 0:
            candidates.append(first)
    assert len(candidates) == 1
    root = REPO / "build/arch-images/amd64-q086.ufs"
    subprocess.run(["mcopy", "-o", "-i",
        f"{output / 'boot.img'}@@{candidates[0] * 512}", str(root), "::/rootfs.img"], check=True)
    shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", output / "vars.fd")
    base.fixture(output / "gpt.img", True)
    base.fixture(output / "mbr.img", False)
    modes = ("bench",) if "--bench" in sys.argv[2:] else ("run", "verify")
    for mode in modes:
        guest = USBGuest(output) if "--usb" in sys.argv[2:] else base.Guest(output)
        try:
            guest.login()
            mounts = guest.run("mount", r" on / type overlay")
            print(mounts, flush=True)
            expected = "BENCH PASS" if mode == "bench" else "S49 PASS" if mode == "verify" else "S48 PASS"
            result = guest.run("storage-native " + mode, expected)
            print(result, flush=True)
        finally:
            guest.stop()
            for name in ("guest.log", "commands.log", "qemu.log"):
                (output / name).rename(output / (mode + "-" + name))
    assert base.digest(source) == original
    (output / "source.sha256").write_text(original + "  build/amd64/hdd-image.img\n")
    print("q086 native grouped execution PASS")

if __name__ == "__main__":
    main()

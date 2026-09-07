#!/usr/bin/env python3
"""Boot a disposable image and record firmware/allocator memory statistics."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("storage_qemu",
    REPO / "plan/ws019-installation/tests/run-storage-qemu.py")
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)

class MemoryGuest(base.Guest):
    def __init__(self, output, firmware, memory):
        self.output = output
        self.log = output / "guest.log"
        self.deadline = time.monotonic() + 240
        self.commands = (output / "commands.log").open("w")
        self.monitor = (output / "qemu.log").open("w")
        args = ["qemu-system-x86_64", "-machine", "q35", "-m", memory,
                "-smp", "4", "-device", "qemu-xhci,id=xhci",
                "-drive", f"file={output / 'boot.img'},format=raw,if=none,id=boot",
                "-device", "usb-storage,bus=xhci.0,drive=boot,bootindex=1",
                "-nic", "none", "-display", "none", "-serial", "none",
                "-debugcon", f"file:{self.log}", "-monitor", "stdio"]
        if firmware == "uefi":
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", output / "vars.fd")
            args += ["-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                     "-drive", f"if=pflash,format=raw,file={output / 'vars.fd'}"]
        self.commands.write(repr(args) + "\n")
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=self.monitor,
                                     stderr=subprocess.STDOUT, text=True)

def main():
    output, firmware, memory = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
    assert firmware in ("bios", "uefi") and int(memory) >= 256
    output.relative_to(REPO / "plan/ws025-io-memory-cache/temp")
    output.mkdir(parents=True, exist_ok=False)
    source = REPO / "build/amd64/hdd-image.img"
    digest = base.digest(source)
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", source, output / "boot.img"], check=True)
    guest = MemoryGuest(output, firmware, memory)
    try:
        if len(sys.argv) > 4:
            expected = sys.argv[4]
            text, _ = guest.wait(expected, timeout=120)
            assert "login:" not in text
            print("PASS: expected boot failure", expected)
            return
        guest.login()
        at = len(guest.text())
        guest.send("sysctl hw.memory.stats")
        text = guest.prompt(at)
        (output / "memory.txt").write_text(text)
        if firmware == "bios":
            assert "E820 v6 complete" in guest.text()
        assert "ranges_valid=1" in text
        print(text)
        print("PASS: boot and memory reporting", firmware, memory)
    finally:
        if guest.proc.poll() is None:
            guest.proc.stdin.write("quit\n")
            guest.proc.stdin.flush()
            try:
                guest.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                guest.proc.kill()
                guest.proc.wait()
        guest.commands.close()
        guest.monitor.close()
        assert base.digest(source) == digest

if __name__ == "__main__":
    main()

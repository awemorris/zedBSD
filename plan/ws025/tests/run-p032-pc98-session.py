#!/usr/bin/env python3
"""Exercise PC-98 login, repeated overlay exec and persistence on image copies."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import subprocess
import time

REPO = Path(__file__).resolve().parents[3]

class Guest:
    def __init__(self, directory, disk, extra_disks=(), memory="64M"):
        self.directory = directory
        directory.mkdir()
        self.deadline = time.monotonic() + 240
        self.log = (directory / "screen.log").open("w")
        self.previous = None
        self.sequence = 0
        monitor = directory / "monitor.sock"
        self.output = (directory / "qemu.log").open("w")
        self.proc = subprocess.Popen([
            str(REPO / "build/qemu-pc98/build/qemu-system-i386"),
            "-M", "pc9821,pegc=off,coregraph=on", "-cpu", "486",
            "-smp", "1", "-m", memory, "-display", "none", "-serial", "none",
            "-no-reboot", "-drive", f"if=ide,format=raw,file={disk}",
            "-debugcon", f"file:{directory}/debugcon.log",
            "-monitor", f"unix:{monitor},server=on,wait=off"] + [argument for extra in extra_disks
                for argument in ("-drive", f"if=ide,format=raw,file={extra}")],
            stdout=self.output, stderr=subprocess.STDOUT)
        self.socket = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.socket.connect(str(monitor))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(.05)
        else:
            self.close()
            raise RuntimeError("monitor did not start")
        self.socket.settimeout(.2)
        self.drain()

    def drain(self):
        try:
            while self.socket.recv(65536):
                pass
        except (socket.timeout, BlockingIOError):
            pass

    def screen(self):
        vram = self.directory / "vram.bin"
        vram.unlink(missing_ok=True)
        self.socket.sendall(f'pmemsave 0xa0000 0x1000 "{vram}"\n'.encode())
        self.drain()
        raw = vram.read_bytes()
        lines = []
        for row in range(25):
            cells = raw[row * 160:row * 160 + 160:2]
            lines.append("".join(chr(c) if 32 <= c < 127 else " " for c in cells).rstrip())
        text = "\n".join(lines)
        if text != self.previous:
            self.log.write(text + "\n---\n")
            self.log.flush()
            self.previous = text
        if re.search(r"kernel panic|panic:|\[INT\]", text):
            raise RuntimeError("guest kernel fault: " + text)
        return text

    def wait(self, pattern, seconds=40):
        until = min(self.deadline, time.monotonic() + seconds)
        while time.monotonic() < until:
            text = self.screen()
            match = re.search(pattern, text, re.M)
            if match:
                return match
            if self.proc.poll() is not None:
                raise RuntimeError("guest exited")
            time.sleep(.2)
        raise RuntimeError("screen timeout: " + pattern + "\n" + text)

    def send(self, text):
        keys = {" ": "spc", "/": "slash", "-": "minus", ".": "dot",
                "$": "shift-4", "?": "shift-slash"}
        for c in text:
            if c.isascii() and (c.islower() or c.isdigit()):
                key = c
            elif "A" <= c <= "Z":
                key = "shift-" + c.lower()
            else:
                key = keys[c]
            self.socket.sendall(f"sendkey {key} 10\n".encode())
            time.sleep(.035)
        self.socket.sendall(b"sendkey ret 10\n")
        time.sleep(.3)
        self.drain()

    def command(self, text, pattern=None):
        self.sequence += 1
        marker = f"P032RESULT{self.sequence}"
        self.send(text)
        result = self.wait(pattern) if pattern else None
        self.send(f"echo {marker} $?")
        self.wait(r"^" + marker + r" 0 *$")
        self.wait(r"root@[^\n]*\$ *$")
        return result

    def halt(self):
        symbols = subprocess.check_output([
            str(REPO / "build/llvm/bin/llvm-nm"),
            str(REPO / "build/pc98/vmunix")], text=True)
        address = int(re.search(r"^([0-9a-f]+) T kern_platform_halt$",
            symbols, re.M).group(1), 16)
        self.send("/sbin/shutdown -h")
        until = time.monotonic() + 30
        while time.monotonic() < until:
            self.socket.sendall(b"info registers\n")
            text = ""
            try:
                while True:
                    data = self.socket.recv(65536)
                    if not data:
                        raise RuntimeError("monitor closed before halt")
                    text += data.decode(errors="replace")
            except socket.timeout:
                pass
            pc = re.search(r"EIP=([0-9A-Fa-f]+)", text)
            if pc and address <= int(pc.group(1), 16) < address + 16 and "HLT=1" in text:
                self.log.write("HALT VERIFIED " + text + "\n")
                self.log.flush()
                return
            time.sleep(.2)
        raise RuntimeError("guest did not reach platform halt: " + self.screen())

    def login(self):
        self.wait(r"login: *$")
        self.send("root")
        self.wait(r"Password:")
        self.send("")
        self.wait(r"root@[^\n]*\$ *$")

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if hasattr(self, "socket"):
            self.socket.close()
        self.output.close()
        self.log.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir()
    image = args.image.resolve()
    before = hashlib.sha256(image.read_bytes()).hexdigest()
    disk = output / "guest.img"
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(image), str(disk)], check=True)
    result = {"image_sha256": before, "boots": []}
    checksum = None
    try:
        for boot in range(2):
            guest = Guest(output / f"boot-{boot}", disk)
            try:
                guest.login()
                guest.command("uname", r"^zedBSD *$")
                guest.command("ls /", r"sbin")
                source_checksum = guest.command("cksum /etc/passwd",
                    r"^(\d+ +\d+) +/etc/passwd *$").group(1)
                if boot == 0:
                    guest.command("cp /etc/passwd /root/p032-state")
                value = guest.command("cksum /root/p032-state",
                    r"^(\d+ +\d+) +/root/p032-state *$").group(1)
                if value != source_checksum:
                    raise RuntimeError("copied content differs from source")
                if checksum is not None and value != checksum:
                    raise RuntimeError("persistent content changed")
                checksum = value
                for _ in range(12):
                    guest.command("/bin/uname", r"^zedBSD *$")
                result["boots"].append({"login": True, "execs": 12, "checksum": value})
                guest.halt()
                result["boots"][-1]["platform_halt"] = True
            finally:
                guest.close()
        result["status"] = "PASS"
    finally:
        result["source_unchanged"] = hashlib.sha256(image.read_bytes()).hexdigest() == before
        (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    if not result["source_unchanged"]:
        raise RuntimeError("source image changed")
    print(json.dumps(result))

if __name__ == "__main__":
    main()

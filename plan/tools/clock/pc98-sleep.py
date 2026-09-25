#!/usr/bin/env python3
"""Times `sleep N` on a PC-98 guest against the host's clock.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

WS040 p005.  PC-98 has no serial console, so the guest is driven with key
events through the QEMU monitor and read from text VRAM.  Each command ends
by printing a marker; the host notes when the marker appears.  `sleep 0` is
timed the same way and taken off, which leaves the time the guest slept.

    pc98-sleep.py QEMU IMAGE [--sleep 5]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time

KEYS = {" ": "spc", "/": "slash", "-": "minus", ".": "dot", ";": "semicolon",
	"$": "shift-4", "?": "shift-slash"}


class Guest:
	"""One PC-98 guest driven through its monitor."""

	def __init__(self, qemu: str, disk: Path, directory: Path) -> None:
		self.directory = directory
		monitor = directory / "monitor.sock"
		self.output = (directory / "qemu.log").open("w")
		self.proc = subprocess.Popen([
			qemu, "-M", "pc9821,pegc=off,coregraph=on", "-cpu", "486",
			"-smp", "1", "-m", "64M", "-display", "none", "-serial", "none",
			"-no-reboot", "-drive", f"if=ide,format=raw,file={disk}",
			"-monitor", f"unix:{monitor},server=on,wait=off"],
			stdout=self.output, stderr=subprocess.STDOUT)
		self.socket = socket.socket(socket.AF_UNIX)
		for _ in range(100):
			try:
				self.socket.connect(str(monitor))
				break
			except (FileNotFoundError, ConnectionRefusedError):
				time.sleep(.05)
		else:
			raise RuntimeError("monitor did not start")
		self.socket.settimeout(.2)
		self.drain()

	def drain(self) -> None:
		try:
			while self.socket.recv(65536):
				pass
		except (socket.timeout, BlockingIOError):
			pass

	def screen(self) -> str:
		"""Reads the 80x25 text VRAM."""
		vram = self.directory / "vram.bin"
		vram.unlink(missing_ok=True)
		self.socket.sendall(f'pmemsave 0xa0000 0x1000 "{vram}"\n'.encode())
		self.drain()
		raw = vram.read_bytes()
		return "\n".join(
			"".join(chr(c) if 32 <= c < 127 else " "
				for c in raw[row * 160:row * 160 + 160:2]).rstrip()
			for row in range(25))

	def wait(self, pattern: str, seconds: float) -> float:
		"""Waits for a line matching the pattern, returning when it was seen."""
		until = time.monotonic() + seconds
		while time.monotonic() < until:
			if re.search(pattern, self.screen(), re.M):
				return time.monotonic()
			if self.proc.poll() is not None:
				raise RuntimeError("guest exited")
			time.sleep(.05)
		raise RuntimeError(f"screen timeout: {pattern}\n{self.screen()}")

	def send(self, text: str) -> float:
		"""Types a line, returning when Enter was sent."""
		for c in text:
			if c.isascii() and (c.islower() or c.isdigit()):
				key = c
			elif "A" <= c <= "Z":
				key = "shift-" + c.lower()
			else:
				key = KEYS[c]
			self.socket.sendall(f"sendkey {key} 10\n".encode())
			time.sleep(.035)
		self.drain()
		sent = time.monotonic()
		self.socket.sendall(b"sendkey ret 10\n")
		return sent

	def close(self) -> None:
		if self.proc.poll() is None:
			self.proc.kill()
			self.proc.wait()
		self.socket.close()
		self.output.close()


def main() -> int:
	parser = argparse.ArgumentParser()
	parser.add_argument("qemu")
	parser.add_argument("image", type=Path)
	parser.add_argument("--sleep", type=int, default=5)
	args = parser.parse_args()
	with tempfile.TemporaryDirectory(prefix="w40-pc98-") as name:
		directory = Path(name)
		disk = directory / "guest.img"
		subprocess.run(["cp", "--sparse=always", str(args.image), str(disk)], check=True)
		guest = Guest(args.qemu, disk, directory)
		try:
			guest.wait(r"login: *$", 300)
			guest.send("root")
			guest.wait(r"Password:", 30)
			guest.send("")
			guest.wait(r"[#$] *$", 60)
			timings = {}
			for label, seconds in (("base", 0), ("sleep", args.sleep)):
				marker = f"WSDONE{label.upper()}"
				sent = guest.send(f"sleep {seconds}; echo {marker}")
				seen = guest.wait(r"^" + marker + r" *$", 30 + 20 * seconds)
				timings[label] = seen - sent
		finally:
			guest.close()
	slept = timings["sleep"] - timings["base"]
	failed = abs(slept - args.sleep) > 0.1 * args.sleep + 0.5
	print(json.dumps({"sleep_host_s": round(slept, 3),
			  "base_s": round(timings["base"], 3),
			  "failed": ["sleep"] if failed else []}, indent=1))
	return 1 if failed else 0


if __name__ == "__main__":
	sys.exit(main())

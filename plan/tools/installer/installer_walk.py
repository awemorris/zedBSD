"""installer_walk: what the QEMU walks of the installer share (p010-screens.py, p011-install.py).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

A Run boots the installer's image from USB with a Venus GPU (the desktop's compositor needs Vulkan;
plan/ws035/tests/zdesktop-guest.sh's set-up), a USB tablet and the caller's disks, and walks the installer as a person
would, through QMP's keyboard and tablet: every place clicked is a control the installer's log names (INSTALLER
LAYOUT and CONTROL, in the session's log read over SSH).  Pictures are QMP screendumps of the Venus head, or its VNC
when screendump has no surface (a GL scanout).
"""
import json
import os
import re
import shlex
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GUEST = ROOT / "plan/tools/guest/guest.py"
SHOT_VNC = ROOT / "plan/ws035/tests/zdesktop-shot.py"
GPT = ROOT / "plan/tools/installer/make-gpt-4k.py"
SESSION_LOG = "/run/user/1000/session.log"
VENUS = Path(os.environ.get("VENUS_RENDERER", "/home/awe/zedBSD-claude1/build/ws035-sq-venus/install"))
ABS_MAX = 0x7FFF
ESP = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
BASIC = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"


def blank(path: Path, gib: float):
	"""An empty disk of a size."""
	with open(path, "wb") as f:
		f.truncate(int(gib * (1 << 30)))


def gpt(path: Path, sector: int, *parts):
	"""A GPT on a disk: parts are (first MiB, MiB, type, name)."""
	per = (1 << 20) // sector
	specs = [f"{a * per},{n * per},{t},{uuid.uuid4()},{name}" for a, n, t, name in parts]
	subprocess.run([sys.executable, str(GPT), "--sector", str(sector), str(path), *specs], check=True)


def fat(path: Path, first_mib: int, mib: int, label: str, files: dict):
	"""A FAT32 in a partition (512-byte sectors), with files {"/DIR/NAME": bytes}."""
	at = f"{path}@@{first_mib << 20}"
	subprocess.run(["mformat", "-i", at, "-F", "-T", str(mib * 2048), "-v", label, "::"], check=True, capture_output=True)
	made = set()
	for name, data in files.items():
		parts = name.strip("/").split("/")
		for depth in range(1, len(parts)):
			directory = "::/" + "/".join(parts[:depth])
			if directory not in made:
				subprocess.run(["mmd", "-i", at, directory], check=True, capture_output=True)
				made.add(directory)
		local = path.parent / ("fat-" + parts[-1])
		local.write_bytes(data)
		subprocess.run(["mcopy", "-i", at, str(local), "::/" + "/".join(parts)], check=True, capture_output=True)


def fat_read(path: Path, first_mib: int, name: str):
	"""A file of a FAT in a partition, or None."""
	got = subprocess.run(["mtype", "-i", f"{path}@@{first_mib << 20}", "::" + name], capture_output=True)
	if got.returncode != 0:
		return None
	return got.stdout


class Failed(Exception):
	pass


class Run:
	def __init__(self, image: str, prefix: str):
		(ROOT / "build/installer-test").mkdir(parents=True, exist_ok=True)
		self.out = Path(tempfile.mkdtemp(prefix=prefix, dir=ROOT / "build/installer-test"))
		(self.out / "shots").mkdir()
		self.env = dict(os.environ)
		self.env["GUEST_RUNTIME"] = str(self.out / "guest")
		self.env["LIBGL_ALWAYS_SOFTWARE"] = "1"
		self.env["MESA_LOADER_DRIVER_OVERRIDE"] = "zink"
		self.env["VK_DRIVER_FILES"] = self.env.get("VK_DRIVER_FILES", "/usr/share/vulkan/icd.d/lvp_icd.json")
		self.env["RENDER_SERVER_EXEC_PATH"] = str(VENUS / "libexec/virgl_render_server")
		self.env["LD_LIBRARY_PATH"] = str(VENUS / "lib/x86_64-linux-gnu")
		self.image = image
		self.shots = 0
		self.width = 1280
		self.height = 800
		self.qmp = None
		self.lines = []
		self.log = open(self.out / "steps.txt", "w")

	def say(self, text: str):
		print(text, flush=True)
		self.log.write(text + "\n")
		self.log.flush()

	def guest(self, *words, timeout=600):
		return subprocess.run([sys.executable, str(GUEST), *words], env=self.env, capture_output=True, text=True,
				      timeout=timeout)

	def start(self, disks: str):
		"""Boots the image's copy from USB with the Venus GPU, the tablet and the QEMU arguments of the disks."""
		hostmem = os.environ.get("VENUS_HOSTMEM", "1G")
		out = self.out
		shutil.copyfile(self.image, out / "boot.img")
		# The stick's flushes are dropped on the host (cache=unsafe): a host fsync of 0.5 to 2 s blocks QEMU's USB
		# and the guest's xHCI commands to the stick time out, and the live system does not come up.
		extra = (f"-set drive.boot.cache=unsafe "
			 f"-object memory-backend-memfd,id=mem,size=8G,share=on -machine memory-backend=mem "
			 f"-device virtio-gpu-gl-pci,id=venus,venus=on,blob=on,hostmem={hostmem},max_outputs=1 "
			 f"-display egl-headless,rendernode=/dev/dri/renderD128 -vnc unix:{out}/guest/vnc.sock,display=venus "
			 f"-device usb-tablet,bus=xhci.0,port=4 {disks}")
		started = self.guest("start", str(out / "boot.img"), "--disk", "usb", "--qemu-extra", extra)
		(out / "start.txt").write_text(started.stdout + started.stderr)
		if started.returncode != 0:
			raise Failed("start " + started.stderr.strip())
		waited = self.guest("wait", "--timeout", "600", timeout=700)
		(out / "wait.txt").write_text(waited.stdout + waited.stderr)
		if waited.returncode != 0:
			raise Failed("the live system did not answer over SSH")
		session = json.loads((out / "guest/session.json").read_text())
		self.qmp_open(session["monitor"])

	def stop(self):
		self.guest("stop", timeout=120)

	# QMP.
	def qmp_open(self, path: str):
		self.qmp = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		self.qmp.connect(path)
		self.qmp_file = self.qmp.makefile("rw")
		self.qmp_file.readline()
		self.qmp_call("qmp_capabilities")

	def qmp_call(self, command: str, arguments=None):
		request = {"execute": command}
		if arguments is not None:
			request["arguments"] = arguments
		self.qmp_file.write(json.dumps(request) + "\n")
		self.qmp_file.flush()
		while True:
			line = self.qmp_file.readline()
			if not line:
				raise Failed("QMP closed")
			reply = json.loads(line)
			if "return" in reply or "error" in reply:
				return reply

	def events(self, events):
		reply = self.qmp_call("input-send-event", {"events": events})
		if "error" in reply:
			raise Failed("input " + json.dumps(reply["error"]))

	def key(self, *names):
		down = [{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": n}}} for n in names]
		up = [{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": n}}} for n in reversed(names)]
		self.events(down)
		time.sleep(0.05)
		self.events(up)
		time.sleep(0.15)

	def type(self, text: str):
		for c in text:
			if c == ".":
				self.key("dot")
			elif c.isdigit() or ("a" <= c <= "z"):
				self.key(c)
			elif "A" <= c <= "Z":
				self.key("shift", c.lower())
			elif c == "-":
				self.key("minus")
			elif c == " ":
				self.key("spc")
			else:
				raise Failed(f"no key for {c!r}")

	def point(self, x: float, y: float):
		ax = int(round(x * ABS_MAX / (self.width - 1)))
		ay = int(round(y * ABS_MAX / (self.height - 1)))
		self.events([{"type": "abs", "data": {"axis": "x", "value": ax}},
			     {"type": "abs", "data": {"axis": "y", "value": ay}}])

	def press(self, x: float, y: float):
		self.point(x, y)
		time.sleep(0.12)
		self.events([{"type": "btn", "data": {"down": True, "button": "left"}}])
		time.sleep(0.08)
		self.events([{"type": "btn", "data": {"down": False, "button": "left"}}])
		time.sleep(0.4)

	def wheel(self, x: float, y: float, down: bool, count: int = 3):
		self.point(x, y)
		for _ in range(count):
			button = "wheel-down" if down else "wheel-up"
			self.events([{"type": "btn", "data": {"down": True, "button": button}}])
			self.events([{"type": "btn", "data": {"down": False, "button": button}}])
			time.sleep(0.1)

	# The session's log.
	def read_log(self):
		got = self.guest("run", "cat", SESSION_LOG, timeout=60)
		self.lines = got.stdout.splitlines()
		return self.lines

	def wait_line(self, pattern: str, timeout: float = 120.0, after: int = 0):
		rx = re.compile(pattern)
		end = time.time() + timeout
		while time.time() < end:
			lines = self.read_log()
			for index in range(after, len(lines)):
				if rx.search(lines[index]):
					return index, lines[index]
			time.sleep(1.0)
		raise Failed(f"no line {pattern!r} in {timeout:.0f} s")

	def origin(self):
		lines = self.read_log()
		start = 0
		for index, line in enumerate(lines):
			if "KWL INSTALLER start" in line:
				start = index
		place = None
		for line in lines[start:]:
			m = re.search(r"KWL WINDOW centred surface=\d+ x=(-?\d+) y=(-?\d+)", line) or \
				re.search(r"KWL MAP client=\d+ surface=\d+ x=(-?\d+) y=(-?\d+)", line)
			if m:
				place = (int(m.group(1)), int(m.group(2)))
		if place is None:
			raise Failed("the installer's window is not in the compositor's log")
		return place

	def controls(self):
		lines = self.read_log()
		last = None
		for index, line in enumerate(lines):
			if "INSTALLER LAYOUT " in line:
				last = index
		if last is None:
			return "", {}
		screen = re.search(r"screen=(\S+)", lines[last]).group(1)
		found = {}
		for line in lines[last + 1:]:
			m = re.search(r"INSTALLER CONTROL name=(\S+) x=(-?\d+) y=(-?\d+) width=(\d+) height=(\d+) enabled=(\d)", line)
			if not m:
				if "INSTALLER LAYOUT " in line:
					break
				continue
			found[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
		return screen, found

	def wait_screen(self, word: str, timeout: float = 120.0):
		end = time.time() + timeout
		while time.time() < end:
			screen, found = self.controls()
			if screen == word and found:
				time.sleep(0.6)
				return found
			time.sleep(1.0)
		raise Failed(f"screen {word} not shown")

	def click(self, name: str, prefix: bool = False):
		_screen, found = self.controls()
		names = [n for n in found if (n.startswith(name) if prefix else n == name)]
		names = [n for n in names if found[n][4] == 1]
		if not names:
			raise Failed(f"no enabled control {name} ({sorted(found)})")
		x, y, w, h, _enabled = found[names[0]]
		ox, oy = self.origin()
		cy = oy + y + h / 2
		if cy > self.height - 80 and name not in ("next", "back", "close", "start-over", "reboot"):
			self.wheel(ox + 300, oy + 300, True, 5)
			time.sleep(0.8)
			_screen, found = self.controls()
			x, y, w, h, _enabled = found[names[0]]
			cy = oy + y + h / 2
		self.press(ox + x + w / 2, cy)
		return names[0]

	def set_field(self, name: str, text: str):
		"""Types a text into a field in place of what it holds: Ctrl+A selects it all and the typing replaces it.
		(Backspace does not erase a space in a field of the live session, so erasing key by key leaves text behind.)"""
		self.click(name)
		time.sleep(0.5)
		self.key("ctrl", "a")
		time.sleep(0.3)
		self.type(text)

	def root(self, command: str, timeout: float = 300):
		"""Runs a shell command as root on the live system over SSH; gives its output."""
		# guest.py joins its words for ssh, so the command goes as one quoted word.
		got = self.guest("run", "sh", "-c", shlex.quote(command), timeout=timeout)
		return got.returncode, got.stdout

	def shot(self, name: str):
		self.shots += 1
		path = self.out / "shots" / f"{self.shots:02d}-{name}.png"
		time.sleep(0.8)
		reply = self.qmp_call("screendump", {"filename": str(path), "device": "venus", "format": "png"})
		how = "screendump"
		if "error" in reply or not path.exists() or path.stat().st_size == 0:
			got = subprocess.run([sys.executable, str(SHOT_VNC), str(path), "--runtime", str(self.out / "guest")],
					     env=self.env, capture_output=True, text=True, timeout=120)
			how = "vnc"
			if got.returncode != 0:
				raise Failed(f"shot {name}: {reply.get('error')} / {got.stderr.strip()}")
		with open(path, "rb") as f:
			head = f.read(24)
		if head[:8] == b"\x89PNG\r\n\x1a\n":
			self.width, self.height = struct.unpack(">II", head[16:24])
		self.say(f"SHOT {path.relative_to(ROOT)} {how} {self.width}x{self.height}")



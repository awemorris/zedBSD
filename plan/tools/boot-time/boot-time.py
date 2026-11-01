#!/usr/bin/env python3
"""Times boots of an amd64 graphical image in QEMU, from the emulator's start to the desktop.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

The guest is the Venus desktop guest (plan/ws035/tests/zdesktop-guest.sh), so
the compositor draws as it does on a machine.  Nothing reads the serial port
or the emulator's console log: the host watches the Venus head through VNC
(as often as a picture can be taken, keeping the pictures that differ from
the one before; VNC can show a new picture up to a few seconds late) and SSH (when it first answers), and afterwards reads the
guest's own records over SSH: init's timeline (/var/log/init.log), the kernel's
boot steps (dmesg's "boot: at" lines), sessiond's log and the session's log.

    plan/tools/boot-time/boot-time.py IMAGE OUTDIR [--runs 3] [--seconds 90]

Each run boots the image afresh (zdesktop-guest.sh copies it over the
runtime's disk) and writes OUTDIR/run-N/.  The last lines printed are one
summary line per run and their means:

    boot-time: run=1 screen=... ssh=... kernel_init=... init_login=... desktop_shown=... network_done=...

screen and ssh are seconds from the emulator's start on the host's clock: the
first picture of the Venus head that is mostly bright, not QEMU's black
"Display output is not active" (the desktop's first frame) and the first SSH answer.  The others are seconds since the kernel started, from the guest's
records: init starting (dmesg), the login service starting (init.log's start
of greeter or getty_console), the desktop shown (sessiond's DESKTOP shown) and
the network's oneshot done (init.log's done networking); init_to_login and
init_to_desktop are the login's start and the desktop shown less init's
start.  The guest's clock and the host's do not always agree in QEMU, so the
guest's times are compared with each other, and the host's with each other.

GUEST_RUNTIME names the guest's runtime directory (default build/boot-time/run).
VENUS_RENDERER is read by zdesktop-guest.sh.  The guest is stopped after each
run.  Files are only written, never removed: each picture has a name of its
own, and the scratch picture is written over.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "plan/ws014/tests"))
sys.path.insert(0, str(ROOT / "plan/ws035/tests"))

from venus_rfb import capture  # noqa: E402
from ppm2png import read_ppm, write_png  # noqa: E402

GUEST = ROOT / "plan/ws035/tests/zdesktop-guest.sh"

# What is read from the guest once the boot has settled, each into a file of its own.
RECORDS = {
	"init.log": "cat /var/log/init.log",
	"boot-steps.txt": "dmesg | grep 'boot: at '",
	"boot-parameters.txt": "dmesg | grep 'boot: parameters'",
	"dmesg.txt": "dmesg",
	"sessiond.log": "cat /var/log/sessiond.log",
	"session.log": "cat /run/user/*/session.log",
	"services.txt": "service list",
	"ifconfig.txt": "ifconfig -a",
}


def guest(runtime: Path, *words: str, timeout: int = 120) -> subprocess.CompletedProcess:
	"""Runs the guest script with the runtime directory of this run."""
	environment = dict(os.environ, GUEST_RUNTIME=str(runtime))
	return subprocess.run([str(GUEST), *words], cwd=ROOT, env=environment,
			      capture_output=True, text=True, timeout=timeout)


def shoot(runtime: Path, scratch: Path) -> tuple[str | None, tuple | None]:
	"""Photographs the Venus head through VNC into the scratch picture; returns a digest of its pixels and the pixels.

	QMP's screendump does not see the compositor's GL scanout (it keeps QEMU's
	"Display output is not active"), so the picture comes from VNC, which QEMU
	updates more slowly while the picture does not change: a new picture can be
	seen up to a few seconds late."""
	try:
		report = capture(str(runtime / "vnc.sock"), str(scratch), 1)
	except (OSError, ValueError, RuntimeError):
		return None, None
	if report is None or not scratch.exists():
		return None, None
	width, height, pixels = read_ppm(str(scratch))
	return hashlib.sha256(pixels).hexdigest()[:16], (width, height, pixels)


def drawn(pixels: bytes) -> bool:
	"""Reports whether a picture is mostly bright, as the desktop's wallpaper is and QEMU's black notice is not."""
	sample = pixels[::997]
	return len(sample) > 0 and sum(sample) / len(sample) > 64


def first_time(text: str, pattern: str) -> float | None:
	"""The first number (seconds) a line of text matching pattern carries in its first group."""
	for line in text.splitlines():
		found = re.search(pattern, line)
		if found:
			return float(found.group(1))
	return None


def last_time(text: str, pattern: str) -> float | None:
	"""The last number (seconds) a line of text matching pattern carries in its first group."""
	value = None
	for line in text.splitlines():
		found = re.search(pattern, line)
		if found:
			value = float(found.group(1))
	return value


def one_run(image: Path, runtime: Path, outdir: Path, seconds: float) -> dict:
	"""Boots the image once, watches it, keeps what it saw, and returns the run's times."""
	frames = outdir / "frames"
	frames.mkdir(parents=True, exist_ok=True)
	scratch = outdir / "scratch.ppm"

	# The emulator starts; every host time below is measured from its start
	# command's return, after the image has been copied to the runtime's disk.
	result = guest(runtime, "start", str(image))
	started = time.monotonic()
	(outdir / "start.txt").write_text(result.stdout + result.stderr)
	if result.returncode != 0:
		return {"error": "start"}

	# The screen as often as it can be taken, and SSH every second until it answers.
	events = []
	last = None
	screen_at = None

	# SSH is tried in a thread of its own, so the pictures keep their pace while a try waits.
	ssh = {"at": None, "stop": False}

	def try_ssh() -> None:
		while not ssh["stop"] and time.monotonic() - started < seconds:
			moment = time.monotonic()
			answer = guest(runtime, "run", "true", timeout=30)
			if answer.returncode == 0:
				ssh["at"] = round(time.monotonic() - started, 2)
				return
			time.sleep(max(0.0, 0.5 - (time.monotonic() - moment)))

	ssh_thread = threading.Thread(target=try_ssh)
	ssh_thread.start()
	while time.monotonic() - started < seconds:
		moment = time.monotonic() - started
		digest, picture = shoot(runtime, scratch)
		if digest is not None and digest != last:
			name = f"{moment:06.2f}.png"
			write_png(str(frames / name), *picture)
			events.append({"t": round(moment, 2), "screen": digest, "picture": name})
			# QEMU's own notice of a head nothing has drawn on yet is dark; the desktop is not.
			if screen_at is None and drawn(picture[2]):
				screen_at = round(moment, 2)
			last = digest
		# Once both are seen, a few more seconds of pictures, then the records.
		ssh_at = ssh["at"]
		if screen_at is not None and ssh_at is not None and moment > max(screen_at, ssh_at) + 8.0:
			break
		time.sleep(0.05)
	ssh["stop"] = True
	ssh_thread.join()
	ssh_at = ssh["at"]
	if ssh_at is not None:
		events.append({"t": ssh_at, "ssh": "answers"})

	# The guest's own records.
	texts = {}
	for name, command in RECORDS.items():
		answer = guest(runtime, "run", command, timeout=60)
		texts[name] = answer.stdout + answer.stderr
		(outdir / name).write_text(texts[name])
	(outdir / "events.json").write_text(json.dumps({"events": events}, indent=1) + "\n")
	guest(runtime, "stop")

	# The times of the run.
	init = texts["init.log"]
	sessiond = texts["sessiond.log"]
	shown_ms = last_time(sessiond, r"DESKTOP shown at_ms=(\d+)")
	times = {
		"screen": screen_at,
		"ssh": ssh_at,
		"kernel_init": None,
		"init_login": first_time(init, r"^(\d+\.\d+) start (?:greeter|getty_console)$"),
		"desktop_shown": shown_ms / 1000.0 if shown_ms is not None else None,
		"network_done": first_time(init, r"^(\d+\.\d+) done networking$"),
		"order": None,
	}
	steps = first_time(texts["boot-steps.txt"], r"boot: at (\d+) ms: init is starting")
	if steps is not None:
		times["kernel_init"] = steps / 1000.0
	for line in init.splitlines():
		if " order " in line:
			times["order"] = line.split()[-1]

	# What init and the desktop took from init's start, on the guest's own clock.
	for key, mark in (("init_to_login", "init_login"), ("init_to_desktop", "desktop_shown")):
		times[key] = None
		if times["kernel_init"] is not None and times[mark] is not None:
			times[key] = round(times[mark] - times["kernel_init"], 3)
	return times


def main() -> int:
	"""Boots the image the asked number of times and prints the times of each run."""
	parser = argparse.ArgumentParser()
	parser.add_argument("image")
	parser.add_argument("outdir")
	parser.add_argument("--runs", type=int, default=3)
	parser.add_argument("--seconds", type=float, default=90.0)
	arguments = parser.parse_args()

	runtime = Path(os.environ.get("GUEST_RUNTIME", str(ROOT / "build/boot-time/run"))).resolve()
	outdir = Path(arguments.outdir).resolve()
	image = Path(arguments.image).resolve()
	results = []
	for run in range(1, arguments.runs + 1):
		times = one_run(image, runtime, outdir / f"run-{run}", arguments.seconds)
		results.append(times)
		(outdir / f"run-{run}" / "times.json").write_text(json.dumps(times, indent=1) + "\n")
		words = " ".join(f"{key}={value}" for key, value in times.items())
		print(f"boot-time: run={run} {words}", flush=True)

	# The means of the numbers every run has.
	means = []
	for key in ("screen", "ssh", "kernel_init", "init_login", "desktop_shown", "network_done",
		    "init_to_login", "init_to_desktop"):
		values = [times.get(key) for times in results]
		if values and all(isinstance(value, float) for value in values):
			means.append(f"{key}={sum(values) / len(values):.2f}")
		else:
			means.append(f"{key}=-")
	print("boot-time: mean " + " ".join(means), flush=True)
	return 0


if __name__ == "__main__":
	sys.exit(main())

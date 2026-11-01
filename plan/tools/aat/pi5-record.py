#!/usr/bin/env python3
"""pi5-record: short videos of the desktop driven with aat, taken by a USB HDMI capture board on this host.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

The board's HDMI output goes into a UVC capture device (/dev/video0); the desktop is driven over SSH with
plan/tools/aat/aat (aat-input on the board, a kernel with CONFIG_INPUT_TEST_INJECT, for the Raspberry Pi 5
plan/tools/aat/config-rpi5-aat.mk).  Each clip is a setup (not recorded), then the capture started, the actions,
and the capture stopped; the capture is kept as MJPEG while recording (no frames lost to the encoder) and turned
into H.264 afterwards, with one PNG still.

    AAT_TARGET=user@10.0.30.7 AAT_INPUT=/tmp/aat-rec/aat-input \\
        python3 -I plan/tools/aat/pi5-record.py OUT [CLIP ...]     (default: every clip, in order)
    python3 -I plan/tools/aat/pi5-record.py --still OUT/now.png   one frame of the capture, nothing driven
    python3 -I plan/tools/aat/pi5-record.py --list

OUT/NN-name.mp4 and OUT/NN-name.png; OUT/record.log says what each clip did.  The board needs aat-input started
(aat start --size WxH) and its session log at AAT_LOG (default /run/user/1000/session.log).  Samples for the
video clip: SAMPLE (default /tmp/aat-rec/sample.mp4 on the board); the model for mview: MVIEW (the program) and
MODEL (its directory) on the board.
"""
from __future__ import annotations

import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
AAT = ROOT / "plan/tools/aat/aat"
DEVICE = os.environ.get("CAPTURE_DEVICE", "/dev/video0")
SIZE = os.environ.get("CAPTURE_SIZE", "1920x1080")
FPS = os.environ.get("CAPTURE_FPS", "60")
INPUT = os.environ.get("AAT_INPUT", "/bin/aat-input")
SAMPLE = os.environ.get("SAMPLE", "/tmp/aat-rec/sample.mp4")
MVIEW = os.environ.get("MVIEW", "/tmp/aat-rec/mview")
MODEL = os.environ.get("MODEL", "/tmp/aat-rec/qs40")
SESSION_USER = os.environ.get("AAT_USER", "user")
# The capture device gives black frames for about a second after it is opened: each clip starts this much earlier,
# and the black part is cut off.
LEAD = float(os.environ.get("CAPTURE_LEAD", "1.8"))

# A floating window's title bar (as in scenarios/aatlib.py): 44 pixels high, its bottom 8 above the body.
TITLE_HEIGHT = 44
TITLE_GAP = 8


class Fail(Exception):
	pass


def aat(*words: str, timeout: float = 60.0, check: bool = True) -> str:
	"""Runs one aat command; returns its output."""
	command = [sys.executable, "-I", str(AAT), *[str(word) for word in words]]
	try:
		result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
	except subprocess.TimeoutExpired:
		raise Fail(f"aat {' '.join(map(str, words))[:60]}: no answer in {timeout:g} s")
	if check and result.returncode != 0:
		raise Fail(f"aat {' '.join(map(str, words))[:60]}: {result.stderr.strip() or result.stdout.strip()}")
	return result.stdout


class Recorder:
	def __init__(self, out: Path):
		self.out = out
		self.marks = 0
		self.x, self.y = 960, 540
		self.log = open(out / "record.log", "a", encoding="utf-8")

	def say(self, text: str) -> None:
		line = f"{time.strftime('%H:%M:%S')} {text}"
		print(line, flush=True)
		self.log.write(line + "\n")
		self.log.flush()

	# The session's log.

	def mark(self) -> str:
		self.marks += 1
		name = f"rec{os.getpid()}-{self.marks}"
		aat("mark", name)
		return name

	def wait(self, regex: str, since: str | None, timeout: float = 15.0) -> str | None:
		words = ["wait-log", regex, "--timeout", str(timeout)] + (["--since", since] if since else [])
		text = aat(*words, timeout=timeout + 30, check=False).strip()
		return text.splitlines()[-1] if text else None

	def lines(self, regex: str, since: str | None) -> list[str]:
		words = ["lines", regex] + (["--since", since] if since else [])
		return aat(*words, check=False).strip().splitlines()

	# The input: many aat-input commands in one SSH round, paced on the board.

	def inputs(self, commands: list[str], timeout: float = 60.0) -> None:
		# As a script file: a long sequence does not fit in one command line on the target.
		local = Path(os.environ.get("CAPTURE_RAW", str(self.out / "raw"))) / "input.sh"
		local.parent.mkdir(parents=True, exist_ok=True)
		local.write_text(f"I={shlex.quote(INPUT)}\n" + "".join(f"$I {command} > /dev/null\n" for command in commands))
		aat("put", str(local), "/tmp/aat-rec/input.sh")
		aat("run", "--root", "--timeout", str(timeout), "sh /tmp/aat-rec/input.sh", timeout=timeout + 30)

	def glide_steps(self, x: float, y: float, ms: int = 450) -> list[str]:
		"""The pointer moved smoothly (ease in and out) to x, y over ms milliseconds."""
		steps = max(2, ms // 16)
		commands = []
		for step in range(1, steps + 1):
			t = step / steps
			t = t * t * (3 - 2 * t)
			commands.append(f"move-to {round(self.x + (x - self.x) * t)} {round(self.y + (y - self.y) * t)}")
			commands.append("sleep 16")
		self.x, self.y = x, y
		return commands

	def glide(self, x: float, y: float, ms: int = 450) -> None:
		self.inputs(self.glide_steps(x, y, ms))

	def click(self, x: float, y: float, ms: int = 450) -> None:
		self.inputs(self.glide_steps(x, y, ms) + ["sleep 80", "click left"])

	def drag_path(self, points: list[tuple[float, float]], ms_each: int = 500) -> None:
		"""The left button held along a path of points (the first is where it goes down)."""
		commands = self.glide_steps(*points[0], ms=ms_each) + ["sleep 120", "down left", "sleep 120"]
		for point in points[1:]:
			commands += self.glide_steps(*point, ms=ms_each)
		commands += ["sleep 120", "up left"]
		self.inputs(commands, timeout=120)

	def key(self, *chords: str) -> None:
		aat("key", *chords)

	def type(self, text: str) -> None:
		aat("type", text)

	def wheel(self, notches: int, times: int, pause_ms: int = 140) -> None:
		commands = []
		for _ in range(times):
			commands += [f"wheel {notches}", f"sleep {pause_ms}"]
		self.inputs(commands)

	# Programs and windows.

	def as_user(self, command: str) -> None:
		inner = ("cd; export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0; "
			f"nohup {command} >> /run/user/1000/session.log 2>&1 < /dev/null &")
		aat("run", "--root", f"su {shlex.quote(SESSION_USER)} -c {shlex.quote(inner)}")

	def stop_programs(self, *names: str) -> None:
		"""Ends the named programs (by their command's name), never the desktop's own Files."""
		script = ("ps -A -o pid,comm,args | while read pid comm args; do case \" $* \" in *\" ${comm##*/} \"*) "
			"case \"$args\" in *--desktop*) ;; *) kill $pid ;; esac ;; esac; done")
		aat("run", "--root", f"set -- {' '.join(names)}; {script}", check=False)
		time.sleep(1.0)

	def home_open(self) -> tuple[str, dict[str, tuple[int, int]]]:
		since = self.mark()
		self.key("super")
		if not self.wait(r"KWL HOME opened ", since, 10):
			raise Fail("App Home did not open")
		icons = {}
		for line in self.lines(r'KWL HOME icon name=', since):
			match = re.search(r'name="([^"]*)" x=(-?\d+) y=(-?\d+)', line)
			if match:
				icons[match.group(1)] = (int(match.group(2)), int(match.group(3)))
		return since, icons

	def launch(self, name: str, pause_ms: int = 400) -> dict:
		"""App Home opened, the pointer to the app's icon and a click; returns its window."""
		since, icons = self.home_open()
		if name not in icons:
			raise Fail(f"App Home shows no {name} (it shows {', '.join(sorted(icons))})")
		time.sleep(pause_ms / 1000)
		started = self.mark()
		self.click(*icons[name], ms=600)
		if not self.wait(rf"KWL HOME launch name={re.escape(name)} pid=\d+", started, 10):
			raise Fail(f"App Home did not start {name}")
		window = self.window_after(started)
		if window is None:
			raise Fail(f"{name}: no window")
		return window

	def window_after(self, since: str | None, timeout: float = 15.0) -> dict | None:
		"""The first window mapped after the mark, with its size once the compositor's lines give it (aat windows)."""
		end = time.time() + timeout
		mapped = None
		while time.time() < end:
			if mapped is None:
				for line in self.lines(r"KWL MAP client=\d+ surface=\d+ ", since):
					match = re.search(r"client=(\d+) surface=(\d+)", line)
					mapped = (int(match.group(1)), int(match.group(2)))
					break
			if mapped is not None:
				for window in self.windows():
					if (window.get("client"), window.get("surface")) == mapped:
						if window.get("width"):
							return window
						# A program started outside App Home: its place only (no line gives its size).
						if time.time() > end - timeout + 4:
							window.setdefault("width", 1120)
							window.setdefault("height", 720)
							return window
			time.sleep(0.3)
		return None

	def windows(self) -> list[dict]:
		import json
		text = aat("windows", "--json", check=False)
		try:
			return json.loads(text)
		except ValueError:
			return []

	# The capture.

	def capture(self, name: str, seconds: float, action, still_at: float = 0.7) -> None:
		raw_dir = Path(os.environ.get("CAPTURE_RAW", str(self.out / "raw")))
		raw_dir.mkdir(parents=True, exist_ok=True)
		raw = raw_dir / f"{name}.mkv"
		mp4 = self.out / f"{name}.mp4"
		png = self.out / f"{name}.png"
		ffmpeg = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "v4l2",
			"-input_format", "mjpeg", "-video_size", SIZE, "-framerate", FPS, "-i", DEVICE, "-t", f"{seconds + LEAD:.1f}",
			"-c:v", "copy", str(raw)], stdin=subprocess.DEVNULL)
		time.sleep(LEAD + 0.2)
		started = time.time()
		error = None
		try:
			action()
		except Exception as failure:
			error = Fail(f"{type(failure).__name__}: {failure}")
		self.say(f"{name}: actions took {time.time() - started:.1f} s")
		try:
			ffmpeg.wait(timeout=seconds + 20)
		except subprocess.TimeoutExpired:
			ffmpeg.terminate()
			ffmpeg.wait(timeout=10)
		subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", f"{LEAD:.1f}", "-i", str(raw), "-c:v", "libx264",
			"-preset", "medium", "-crf", "24", "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(mp4)],
			check=False, timeout=600)
		subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", f"{seconds * still_at:.2f}",
			"-i", str(mp4), "-frames:v", "1", str(png)], check=False, timeout=60)
		size = mp4.stat().st_size if mp4.exists() else 0
		self.say(f"{name}: {mp4.name} {size / 1e6:.1f} MB, still {png.name}")
		if error:
			raise error


def still(path: Path) -> None:
	path.parent.mkdir(parents=True, exist_ok=True)
	subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "v4l2", "-input_format", "mjpeg",
		"-video_size", SIZE, "-framerate", FPS, "-i", DEVICE, "-ss", f"{LEAD:.1f}", "-frames:v", "1", str(path)],
		check=True, timeout=30)


# The clips: (name, seconds, setup, action).

def clean(r: Recorder) -> None:
	r.stop_programs("files", "settings", "terminal", "videoplayer", "mview", "monitor", "browser", "photos")
	r.inputs(r.glide_steps(1500, 900, 200))


def files_home(r: Recorder) -> None:
	time.sleep(0.6)
	r.state["files"] = r.launch("Files")
	time.sleep(1.5)
	r.glide(1200, 700, 600)


def window_drag(r: Recorder) -> None:
	w = r.state.get("files") or r.window_after(None)
	x0, y0 = w["x"] + 160, w["y"] - TITLE_GAP - TITLE_HEIGHT // 2
	r.drag_path([(x0, y0), (x0 - 380, y0 + 140), (x0 - 120, y0 + 330), (x0 + 260, y0 + 120), (x0, y0)], ms_each=650)
	time.sleep(0.3)


def setup_scroll(r: Recorder) -> None:
	clean(r)
	since = r.mark()
	r.as_user("/bin/files /bin")
	r.state["files"] = r.window_after(since)
	time.sleep(2.0)


def files_scroll(r: Recorder) -> None:
	w = r.state["files"]
	r.glide(w["x"] + w["width"] // 2, w["y"] + w["height"] // 2, 500)
	r.wheel(-1, 24, 90)
	time.sleep(0.6)
	r.wheel(1, 24, 90)


def setup_switch(r: Recorder) -> None:
	clean(r)
	r.state["files"] = r.launch("Files")
	time.sleep(1.0)
	r.state["settings"] = r.launch("Settings")
	time.sleep(1.0)
	r.state["terminal"] = r.launch("Terminal")
	time.sleep(1.5)


def switch_windows(r: Recorder) -> None:
	"""Alt held, Tab pressed (the switcher), then the windows clicked to the front."""
	r.inputs(["key-down leftalt", "sleep 200", "key tab", "sleep 900", "key tab", "sleep 900", "key-up leftalt"])
	time.sleep(1.2)
	r.inputs(["key-down leftalt", "sleep 200", "key tab", "sleep 1000", "key-up leftalt"])
	time.sleep(1.2)
	# The Terminal's part that the window in front leaves visible, then a command typed there.
	w = r.state.get("terminal")
	if w:
		r.click(w["x"] + w["width"] - 120, w["y"] + w["height"] - 120, ms=550)
		time.sleep(0.5)
		r.type("uname -a")
		r.key("enter")
		time.sleep(1.0)


def setup_settings(r: Recorder) -> None:
	"""Settings alone on the screen; its sidebar's rows (Network, Appearance, Display, Storage) and its home button,
	as places in its window (the sidebar of Settings' main page at 1920x1080)."""
	r.stop_programs("files", "terminal", "videoplayer", "mview")
	w = r.state.get("settings")
	if not w or not any(window.get("surface") == w["surface"] and window.get("client") == w["client"]
			for window in r.windows()):
		clean(r)
		w = r.state["settings"] = r.launch("Settings")
		time.sleep(1.0)
	r.state["settings_points"] = [(w["x"] + dx, w["y"] + dy) for dx, dy in
		((90, 220), (76, 364), (79, 449), (79, 171), (206, -30))]


def settings_pages(r: Recorder) -> None:
	for x, y in r.state.get("settings_points", []):
		r.click(x, y, ms=500)
		time.sleep(1.1)


def lock_unlock(r: Recorder) -> None:
	"""Super+L locks; a swipe up shows the card, the password (AAT_USER_PASSWORD, default the user's name) is typed
	and the arrow button unlocks.  A user with a PIN gets the keypad instead (not driven here)."""
	time.sleep(0.4)
	r.key("super+l")
	time.sleep(2.6)
	r.drag_path([(960, 960), (960, 760), (960, 520)], ms_each=260)
	time.sleep(0.9)
	for letter in os.environ.get("AAT_USER_PASSWORD", SESSION_USER):
		r.type(letter)
		time.sleep(0.18)
	time.sleep(0.4)
	r.click(1103, 651, ms=500)
	time.sleep(2.0)


def setup_video(r: Recorder) -> None:
	clean(r)
	since = r.mark()
	r.as_user(f"/bin/videoplayer {shlex.quote(SAMPLE)}")
	r.state["video"] = r.window_after(since)
	time.sleep(1.0)


def video_play(r: Recorder) -> None:
	w = r.state.get("video")
	if w:
		r.glide(w["x"] + w["width"] // 2, w["y"] + w["height"] // 2, 600)
		r.inputs(["sleep 100", "click left"])
	time.sleep(6.0)


def setup_mview(r: Recorder) -> None:
	clean(r)
	since = r.mark()
	r.as_user(f"{shlex.quote(MVIEW)} --model={shlex.quote(MODEL)} --windowed --size=1100x700 --token=rec")
	r.state["mview"] = r.window_after(since)
	time.sleep(2.0)


def mview_orbit(r: Recorder) -> None:
	w = r.state.get("mview")
	if not w:
		raise Fail("no mview window")
	cx, cy = w["x"] + w["width"] // 2, w["y"] + w["height"] // 2
	r.drag_path([(cx - 200, cy), (cx + 250, cy + 30), (cx - 100, cy - 80), (cx + 150, cy + 60)], ms_each=900)
	r.glide(cx, cy, 300)
	r.wheel(1, 6, 90)
	r.wheel(-1, 6, 90)


CLIPS = [
	("01-home-files", 8, clean, files_home),
	("02-window-drag", 8, None, window_drag),
	("03-files-scroll", 8, setup_scroll, files_scroll),
	("04-switch-windows", 9, setup_switch, switch_windows),
	("05-settings-pages", 9, setup_settings, settings_pages),
	("06-lock-unlock", 10, clean, lock_unlock),
	("07-video-player", 9, setup_video, video_play),
	("08-mview", 9, setup_mview, mview_orbit),
]


def main() -> int:
	arguments = sys.argv[1:]
	if arguments[:1] == ["--list"]:
		for name, seconds, _setup, _action in CLIPS:
			print(name, seconds)
		return 0
	if arguments[:1] == ["--still"]:
		still(Path(arguments[1]))
		return 0
	if not arguments:
		print(__doc__, file=sys.stderr)
		return 2
	out = Path(arguments[0])
	out.mkdir(parents=True, exist_ok=True)
	wanted = arguments[1:]
	r = Recorder(out)
	r.state = {}
	state_file = out / "state.json"
	import json
	if state_file.exists():
		r.state = json.loads(state_file.read_text())
	status = 0
	for name, seconds, setup, action in CLIPS:
		if wanted and name not in wanted and name.split("-", 1)[1] not in wanted:
			continue
		try:
			if setup:
				setup(r)
			r.capture(name, seconds, lambda: action(r))
		except Fail as failure:
			r.say(f"{name}: FAIL {failure}")
			status = 1
		state_file.write_text(json.dumps(r.state, indent=1))
	return status


if __name__ == "__main__":
	sys.exit(main())

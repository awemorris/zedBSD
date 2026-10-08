"""aatlib: what the AAT's automatic helpers share (WS173 p004, plan/tests.md).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

The scenarios are the documents under tests/scenarios/; an agent reads one
and carries it out with plan/tools/aat/aat.  A helper here is the same
scenario written as code (optional, by the same id): it does the steps the
document lists, as a person would (Super opens App Home, a name is typed,
the icon the log names is clicked; every place clicked comes from a log
line, never a fixed pixel), and records each step: the action, what was
seen, the screenshots, and in the end the scenario's verdict, pass, fail
or needs-person (a look at a screenshot, or a person's hands).  The
records go to OUTDIR/records/ID.md and OUTDIR/verdicts.tsv, which
run-aat.sh sums up.

    import aatlib
    run = aatlib.Run.from_command_line("os")

    @run.define("os.boot.session-up")
    def session_up(item):
        line = run.wait(r"KWL READY .* role=normal", None, 120)
        item.step("wait for the desktop", line)
        item.check(line, "no KWL READY")
        run.shot(item, "desktop")
        item.passed(line)

    sys.exit(run.go(before=..., after=...))
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import time
import traceback
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
AAT = HERE.parent / "aat"
SCENARIOS = ROOT / "tests/scenarios"

# The session's user and its runtime directory (the AAT image logs kei in at boot).
USER = os.environ.get("AAT_USER", "kei")
PASSWORD = os.environ.get("AAT_USER_PASSWORD", "kei")

# Where the samples and the files the items write go on the target.
SAMPLES = "/tmp/aat-samples"
WORK = "/tmp/aat-work"

# A floating window's title bar: 44 pixels high, its bottom 8 above the body (kwl.h, shell.c floating_title); the
# buttons from the right edge (shell.c button_centre).
TITLE_HEIGHT = 44
TITLE_GAP = 8
BUTTON_FROM_RIGHT = 26
BUTTON_SPACING = 34

# A window started outside App Home (a program run with a file) has only its place in the compositor's lines (KWL
# MAP x= y=), not its size: a point this far inside its body's top-left corner is still in the body of any window an
# application opens (none is smaller than 320x200).
UNSIZED_INSET_X = 120
UNSIZED_INSET_Y = 90

# The applications App Home lists (userland/desktop/wayland/apps.conf): name, what to type, the program's name
# (for ps), and the line that says it is ready (None: the window's map is enough).
APPS = {
	"Files": ("files", "files", r"ZFILES READY "),
	"Notes": ("notes", "notes", r"NOTES (START|OPENED) "),
	"Settings": ("settings", "settings", r"ZSETTINGS READY "),
	"Terminal": ("terminal", "terminal", r"ZTERM START "),
	"PDF Viewer": ("pdf", "pdfviewer", None),
	"Image Viewer": ("image", "imageview", None),
	"Video Player": ("video", "videoplayer", r"VIDEOPLAYER READY "),
	"Music": ("music", "music", r"MUSIC READY "),
	"Photos": ("photos", "photos", r"PHOTOS READY "),
	"Phone": ("phone", "phone", r"PHONE READY "),
	"Calendar": ("calendar", "calendar", r"CALENDAR READY "),
	"Mail": ("mail", "mailer", r"MAIL READY "),
	"Text Editor": ("text", "textedit", None),
	"System Monitor": ("monitor", "monitor", r"ZMON READY "),
	"Browser": ("browser", "browser", None),
}

# The programs an item may leave running, stopped before and after each scenario.
PROGRAMS = ("files", "notes", "settings", "terminal", "pdfviewer", "imageview", "videoplayer", "music", "photos",
	"phone", "calendar", "mailer", "textedit", "monitor", "browser", "emacs")


def stop_programs_command(session_log: str) -> str:
	"""The shell command that ends the PROGRAMS running on the target, all but the desktop program (files --desktop).
	zedBSD's ps shows a process's argv[0] alone, so the desktop program is known by the pid of the compositor's latest
	"KWL DESKTOP start pid=N" line in the session's log (T1-481: the old test of ps's "--desktop" never matched and the
	desktop's icons went away after the first scenario that stopped Files)."""
	pattern = "|".join(PROGRAMS)
	log = shlex.quote(session_log)
	return (
		f"desk=$(grep 'KWL DESKTOP start pid=' {log} 2>/dev/null | tail -1 | sed -n 's/.*pid=\\([0-9]*\\).*/\\1/p'); "
		"for p in $(ps -A -o pid,args | awk -v desk=\"$desk\" '{n = $2; sub(/.*\\//, \"\", n); "
		f"if (n ~ /^({pattern})$/ && $1 != desk && !(n == \"files\" && $3 == \"--desktop\")) print $1}}'); "
		"do kill $p 2>/dev/null; done"
	)


class ItemEnded(Exception):
	"""Ends a scenario's run early with its verdict set."""


def scenario_header(ident: str) -> dict[str, str]:
	"""A scenario's header (title, machine, human, ...) from its document under tests/scenarios/."""
	path = SCENARIOS.joinpath(*ident.split(".")).with_suffix(".md")
	try:
		text = path.read_text(encoding="utf-8")
	except OSError:
		return {}
	match = re.match(r"---\n(.*?)\n---\n", text, re.S)
	fields = {}
	for line in (match.group(1).splitlines() if match else []):
		key, _, value = line.partition(":")
		fields[key.strip()] = value.strip()
	return fields


class Item:
	"""One scenario's run: its id and title, the steps done (action, what was seen, the screenshots), the verdict
	and its reason."""

	def __init__(self, run: "Run", ident: str, title: str):
		self.run = run
		self.ident = ident
		self.title = title
		self.verdict: str | None = None
		self.note = ""
		self.pictures: list[str] = []
		self.steps: list[dict] = []
		self.started = time.monotonic()

	def step(self, action: str, seen: str = "") -> None:
		"""Records a step: what was done and what was seen (a log line, a value); later shots belong to it."""
		self.steps.append({"action": action, "seen": " ".join(str(seen or "").split())[:300], "pictures": []})

	def _end(self, verdict: str, note: str) -> None:
		self.verdict = verdict
		self.note = " ".join(str(note).split())
		raise ItemEnded()

	def passed(self, note: str = "") -> None:
		"""Every check of the scenario holds."""
		self._end("pass", note)

	def failed(self, note: str) -> None:
		"""A check does not hold (say which and what was seen)."""
		self._end("fail", note)

	def person(self, note: str) -> None:
		"""What a machine can check holds; a person looks at the screenshots or does a step by hand."""
		self._end("needs-person", note)

	def check(self, condition, note: str):
		"""Fails the scenario with note unless condition holds; returns condition."""
		if not condition:
			self.failed(note)
		return condition

	def record(self) -> str:
		"""The scenario's record (Markdown): each step's action, what was seen and its screenshots, the verdict."""
		lines = [f"# {self.ident}: {self.title}", "", f"verdict: **{self.verdict}**" + (f" — {self.note}" if self.note else ""), ""]
		for number, step in enumerate(self.steps, start=1):
			lines.append(f"{number}. {step['action']}")
			if step["seen"]:
				lines.append(f"   - seen: `{step['seen']}`")
			for picture in step["pictures"]:
				lines.append(f"   - screenshot: [{picture}](../{picture})")
		return "\n".join(lines) + "\n"


@dataclass
class Window:
	"""A window as the compositor's lines place it: its body (the client's surface) on the screen.  A width and
	height of 0 mean the lines gave its place but not its size (a window started outside App Home)."""
	client: int
	surface: int
	x: int
	y: int
	width: int
	height: int
	docked: int = 0

	def sized(self) -> bool:
		"""Whether the lines gave the window's size."""
		return self.width > 0 and self.height > 0

	def title_point(self) -> tuple[int, int]:
		"""A point of the floating title bar left of its controls (the tests' wx + 60)."""
		return self.x + 60, self.y - TITLE_GAP - TITLE_HEIGHT // 2

	def button_point(self, button: int) -> tuple[int, int]:
		"""A floating title bar's button: 0 close, 1 maximize, 2 minimize."""
		return self.x + self.width - BUTTON_FROM_RIGHT - button * BUTTON_SPACING, self.y - TITLE_GAP - TITLE_HEIGHT // 2

	def middle(self) -> tuple[int, int]:
		"""The middle of the body (of a window without a known size, a point surely inside its body)."""
		if not self.sized():
			return self.x + UNSIZED_INSET_X, self.y + UNSIZED_INSET_Y
		return self.x + self.width // 2, self.y + self.height // 2


class Run:
	"""A scenario's run: the target, the output directory and the items' verdicts."""

	def __init__(self, scenario: str, target: list[str], outdir: Path, only: str | None, log: str | None):
		self.scenario = scenario
		self.target = target
		self.outdir = outdir
		self.pictures = outdir / "png"
		self.only = re.compile(only) if only else None
		self.log = log
		self.marks = 0
		self.qemu = "--qemu" in target
		self._ready: dict | None = None
		self.defined: list = []
		self.listing = False

	@classmethod
	def from_command_line(cls, scenario: str) -> "Run":
		"""Reads a helper's command line: --outdir OUTDIR [--only REGEX] [--log PATH] -- TARGET-OPTIONS (--list:
		the ids it has)."""
		if "--list" in sys.argv:
			run = cls(scenario, ["--local"], Path(os.devnull).parent, None, None)
			run.listing = True
			return run
		parser = argparse.ArgumentParser(prog=scenario)
		parser.add_argument("--outdir", required=True)
		parser.add_argument("--only", help="the items whose ID matches")
		parser.add_argument("--log", help="the session's log (default aat's)")
		parser.add_argument("target", nargs=argparse.REMAINDER, help="-- --qemu | -- --target 5330 ...")
		arguments = parser.parse_args()
		target = [word for word in arguments.target if word != "--"]
		if not target:
			parser.error("no target options after --")
		return cls(scenario, target, Path(arguments.outdir), arguments.only, arguments.log)

	# The tool.

	def aat(self, *words: str, timeout: float = 120.0, check: bool = True) -> subprocess.CompletedProcess:
		"""Runs one aat command against the target; with check, a failure ends the item."""
		command = [sys.executable, str(AAT), *self.target, *[str(word) for word in words]]
		try:
			result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
		except subprocess.TimeoutExpired:
			raise RuntimeError(f"aat {' '.join(map(str, words))[:80]}: no answer in {timeout:g} s")
		if check and result.returncode != 0:
			raise RuntimeError(f"aat {' '.join(map(str, words))[:80]}: {result.stderr.strip() or result.stdout.strip()}")
		return result

	def log_options(self, log: str | None = None) -> list[str]:
		"""The --log option for the session's log (or another log)."""
		path = log or self.log
		return ["--log", path] if path else []

	def sh(self, script: str, root: bool = True, timeout: float = 60.0) -> tuple[int, str]:
		"""Runs a shell script on the target (as root by default); returns its status and output."""
		words = ["run"] + (["--root"] if root else []) + ["--timeout", str(timeout), script]
		result = self.aat(*words, timeout=timeout + 30, check=False)
		return result.returncode, result.stdout

	def as_user(self, command: str, wait: bool = False, timeout: float = 60.0) -> tuple[int, str]:
		"""Runs a command as the session's user in its environment (its Wayland socket): in the background, with
		its output appended to the session's log, unless wait (then its output is returned)."""
		ready = self.ready()
		socket = ready.get("socket", f"/run/user/1000/wayland-0")
		runtime, display = os.path.dirname(socket), os.path.basename(socket)
		environment = f"cd; export XDG_RUNTIME_DIR={shlex.quote(runtime)} WAYLAND_DISPLAY={shlex.quote(display)}; "
		if wait:
			inner = environment + command
		else:
			inner = environment + f"nohup {command} >> {shlex.quote(self.session_log())} 2>&1 </dev/null &"
		return self.sh(f"su {shlex.quote(USER)} -c {shlex.quote(inner)}", timeout=timeout)

	def session_log(self) -> str:
		"""The session's log on the target."""
		return self.log or os.environ.get("AAT_LOG", "/run/user/1000/session.log")

	# The logs.

	def mark(self, log: str | None = None) -> str:
		"""Marks the log's length now; returns the mark's name."""
		self.marks += 1
		name = f"{self.scenario}-{os.getpid()}-{self.marks}"
		self.aat("mark", name, *self.log_options(log))
		return name

	def wait(self, regex: str, since: str | None, timeout: float = 20.0, log: str | None = None) -> str | None:
		"""Waits for a line of the log (after a mark) to match; returns it, or None."""
		words = ["wait-log", regex, "--timeout", str(timeout), *self.log_options(log)]
		if since:
			words += ["--since", since]
		result = self.aat(*words, timeout=timeout + 60, check=False)
		if result.returncode != 0:
			return None
		return result.stdout.strip().splitlines()[-1] if result.stdout.strip() else None

	def lines(self, regex: str, since: str | None, log: str | None = None) -> list[str]:
		"""The lines of the log (after a mark) that match."""
		words = ["lines", regex, *self.log_options(log)]
		if since:
			words += ["--since", since]
		result = self.aat(*words, check=False)
		return result.stdout.splitlines() if result.returncode == 0 else []

	def ready(self) -> dict:
		"""The compositor's KWL READY line of the session (socket, width, height), read once."""
		if self._ready is None:
			found = self.lines(r"KWL READY socket=", None)
			self._ready = {}
			if found:
				self._ready = dict(re.findall(r"(\w+)=(\S+)", found[-1]))
		return self._ready

	def screen(self) -> tuple[int, int]:
		"""The screen's size (from KWL READY; the 5330's panel without one)."""
		ready = self.ready()
		return int(ready.get("width", 1920)), int(ready.get("height", 1200))

	# The input.

	def click(self, x: float, y: float, *more: str) -> None:
		"""Clicks at a point of the screen."""
		self.aat("click", str(int(x)), str(int(y)), *more)

	def key(self, *chords: str) -> None:
		"""Presses chords one after another."""
		self.aat("key", *chords)

	def type(self, text: str) -> None:
		"""Types ASCII."""
		self.aat("type", text)

	def drag(self, x1: float, y1: float, x2: float, y2: float, steps: int = 20) -> None:
		"""Drags with the left button."""
		self.aat("drag", str(int(x1)), str(int(y1)), str(int(x2)), str(int(y2)), "--steps", str(steps))

	def press_path(self, points: list[tuple[float, float]], pause: float = 0.03) -> None:
		"""Presses the left button at the first point, moves through the others and lets go at the last."""
		first = points[0]
		self.aat("move", str(int(first[0])), str(int(first[1])))
		self.aat("down")
		for x, y in points[1:]:
			time.sleep(pause)
			self.aat("move", str(int(x)), str(int(y)))
		self.aat("up")

	# The pictures.

	def shot(self, item: Item, name: str) -> str:
		"""Takes the screen as OUTDIR/png/ITEM-NAME.png and gives it to the item."""
		self.pictures.mkdir(parents=True, exist_ok=True)
		path = self.pictures / f"{item.ident}-{name}.png"
		self.aat("shot", str(path), timeout=90)
		relative = str(path.relative_to(self.outdir))
		item.pictures.append(relative)
		if not item.steps:
			item.step(f"screenshot {name}")
		item.steps[-1]["pictures"].append(relative)
		return relative

	# The windows.

	def windows(self, since: str | None = None) -> list[dict]:
		"""The windows the compositor's lines place."""
		words = ["windows", "--json", *self.log_options()]
		if since:
			words += ["--since", since]
		return json.loads(self.aat(*words).stdout or "[]")

	def window(self, client: int, surface: int, since: str | None = None, unsized: bool = False) -> Window | None:
		"""A window by its client and surface, when its place and size are known (with unsized, its place is
		enough: the size is then 0 by 0)."""
		for window in self.windows(since):
			if window["client"] != client or window["surface"] != surface or "x" not in window:
				continue
			if "width" in window:
				return Window(client, surface, window["x"], window["y"], window["width"], window["height"],
					window.get("docked", 0))
			if unsized:
				return Window(client, surface, window["x"], window["y"], 0, 0, window.get("docked", 0))
		return None

	def mapped_after(self, since: str, timeout: float = 15.0, size_wait: float = 10.0) -> Window | None:
		"""The first window mapped after a mark, once its place and size are logged.  A window whose size no line
		gives within size_wait (one started outside App Home has only KWL MAP) comes back with its place only."""
		line = self.wait(r"KWL MAP client=\d+ surface=\d+", since, timeout)
		if line is None:
			return None
		match = re.search(r"client=(\d+) surface=(\d+)", line)
		client, surface = int(match.group(1)), int(match.group(2))
		deadline = time.monotonic() + size_wait
		while time.monotonic() < deadline:
			window = self.window(client, surface, since)
			if window is not None:
				return window
			time.sleep(0.5)
		return self.window(client, surface, since, unsized=True)

	def home_open(self, item: Item) -> str:
		"""Opens App Home with the Windows key (Super pressed alone); returns the mark before it."""
		since = self.mark()
		self.key("super")
		line = self.wait(r"KWL HOME open via=", since, 10)
		item.step("Windows key pressed and let go", line)
		item.check(line, "App Home did not open (no KWL HOME open)")
		time.sleep(0.4)
		return since

	def launch(self, item: Item, name: str, timeout: float = 20.0) -> Window:
		"""Starts an application from App Home: its name typed, its icon clicked; returns its first window."""
		query, _, ready = APPS[name]
		since = self.home_open(item)
		self.type(query)
		item.check(self.wait(rf'KWL HOME search query="{re.escape(query)}"', since, 10), f"Home did not search for {query}")
		icons = self.lines(rf'KWL HOME icon name="{re.escape(name)}" x=-?\d+ y=-?\d+', since)
		item.step(f"typed {query!r} in App Home", icons[-1] if icons else "no icon")
		item.check(icons, f"Home shows no icon for {name}")
		x, y = (int(value) for value in re.search(r"x=(-?\d+) y=(-?\d+)", icons[-1]).groups())
		started = self.mark()
		self.click(x, y)
		launched = self.wait(rf"KWL HOME launch name={re.escape(name)} pid=\d+", started, 10)
		window = self.mapped_after(started, timeout) if launched else None
		item.step(f"clicked the {name} icon at {x},{y}", f"{launched or 'no launch'}; window {window}")
		item.check(launched, f"Home did not start {name}")
		item.check(window, f"{name}: no window mapped within {timeout:g} s")
		if ready:
			item.check(self.wait(ready, started, 15), f"{name}: no line {ready!r}")
		failed = self.lines(r"(FAILED|ERROR)", started)
		item.check(not failed, f"{name}: {failed[0] if failed else ''}")
		time.sleep(1.0)
		return window

	def open_as_user(self, item: Item, command: str, ready: str | None = None, timeout: float = 20.0) -> Window:
		"""Starts a program as the session's user (a file to open, a page) and returns its first window."""
		since = self.mark()
		self.as_user(command)
		window = self.mapped_after(since, timeout, size_wait=2.0)
		item.step(f"as {USER}: {command}", f"window {window}")
		item.check(window, f"{command}: no window mapped within {timeout:g} s")
		if ready:
			item.check(self.wait(ready, since, 15), f"{command}: no line {ready!r}")
		time.sleep(1.0)
		return window

	def close(self, item: Item, window: Window) -> None:
		"""Closes a window with its title bar's close button (floating), and waits for it to go: its unmap, or its
		client gone (an application that ends at its close request leaves without a null image, so the compositor
		says only KWL CLIENT gone)."""
		since = self.mark()
		current = self.window(window.client, window.surface) or window
		if current.docked:
			buttons = self.lines(rf"KWL GLASS dock surface={current.surface} ", None)
			match = re.search(r"buttons=(-?\d+),", buttons[-1]) if buttons else None
			item.check(match, "a docked window without its buttons' line")
			self.click(int(match.group(1)), TITLE_HEIGHT // 2)
		else:
			item.check(current.sized(), f"the window {window.client}:{window.surface} has no known size for its buttons")
			self.click(*current.button_point(0))
		gone = rf"KWL UNMAP client={window.client} surface={window.surface}\b|KWL CLIENT gone client={window.client}\b"
		line = self.wait(gone, since, 10)
		asked = self.lines(rf"KWL GLASS close surface={window.surface} client={window.client}\b", since)
		item.step("clicked the close button", "; ".join(filter(None, [asked[-1] if asked else "no KWL GLASS close", line])))
		if not asked:
			item.check(line, f"the press did not reach the close button of {window.client}:{window.surface}")
		item.check(line, f"the window {window.client}:{window.surface} was asked to close but did not")

	def back_to_windowed(self) -> None:
		"""Brings the session's layout mode back to windowed when a scenario left it docked (ws142-p008: docking a
		window docks every window switched to or opened after, until one is brought back), so that the next scenario
		starts with floating windows: the restore button in the system bar of the docked window in front."""
		modes = self.lines(r"KWL LAYOUT mode=\w+", None)
		if not modes or "mode=docked" not in modes[-1]:
			return
		docks = self.lines(r"KWL GLASS dock surface=\d+ .*buttons=-?\d+,-?\d+,", None)
		if not docks:
			return
		match = re.search(r"buttons=(-?\d+),(-?\d+),", docks[-1])
		since = self.mark()
		self.click(int(match.group(2)), TITLE_HEIGHT // 2)
		self.wait(r"KWL LAYOUT mode=windowed", since, 5)

	# Settings.

	def settings(self, item: Item, page: str) -> tuple[Window, str]:
		"""Opens Settings from App Home and moves it to a page (the word of settings' command line, handed to the
		Settings that runs, ws089-p016); returns its window and the mark before the page."""
		window = self.launch(item, "Settings")
		since = self.mark()
		self.as_user(f"/bin/settings {shlex.quote(page)}")
		line = self.wait(rf"ZSETTINGS LAYOUT page={re.escape(page)} controls=", since, 15)
		item.step(f"moved Settings to the {page} page", line)
		item.check(line, f"Settings did not show the {page} page")
		time.sleep(0.5)
		return window, since

	def controls(self, since: str, page: str) -> dict[int, tuple[int, int, int, int]]:
		"""The controls of a Settings page as its last layout logged them: index -> x, y, width, height in the
		window (ZSETTINGS LAYOUT page=P, then its CONTROL lines)."""
		found: dict[int, tuple[int, int, int, int]] = {}
		inside = False
		for line in self.lines(r"ZSETTINGS (LAYOUT|CONTROL) ", since):
			if "ZSETTINGS LAYOUT " in line:
				inside = f"page={page} " in line
				if inside:
					found = {}
				continue
			match = re.search(r"CONTROL index=(-?\d+) x=(-?\d+) y=(-?\d+) width=(\d+) height=(\d+)", line)
			if inside and match:
				index, x, y, width, height = (int(value) for value in match.groups())
				found[index] = (x, y, width, height)
		return found

	def click_control(self, item: Item, window: Window, controls: dict, index: int, what: str, **options) -> tuple[int, int]:
		"""Clicks the middle of a Settings control (its place in the window plus the window's)."""
		item.check(index in controls, f"no control {index} ({what}) on the page")
		x, y, width, height = controls[index]
		point = (window.x + x + width // 2, window.y + y + height // 2)
		self.click(*point, *options.get("more", ()))
		return point

	def stop_programs(self) -> None:
		"""Ends the applications the items may have left (not the compositor and its daemons, nor the desktop program
		"files --desktop": the compositor starts that again at most four times a minute, so ending it before and after
		every scenario took the desktop's icons away for the rest of the run, T1-339)."""
		# Never on this host (--local, the runner's own test): the names are common ones.
		if "--local" in self.target:
			return
		self.sh(stop_programs_command(self.session_log()) + "; sleep 1; true")

	# The session's log of each scenario.

	def log_start(self) -> str | None:
		"""Marks the session's log where a scenario starts (None when it cannot be marked)."""
		try:
			return self.mark()
		except RuntimeError:
			return None

	def save_log(self, ident: str, start: str | None) -> None:
		"""Keeps the session's log of a scenario (from its start mark) as OUTDIR/logs/ID.log, so that a failure can be
		understood without running it again (T1-202c had only the verdicts)."""
		if start is None:
			return
		try:
			lines = self.lines(r".", start)
		except RuntimeError:
			return
		logs = self.outdir / "logs"
		logs.mkdir(parents=True, exist_ok=True)
		(logs / f"{ident}.log").write_text("\n".join(lines) + "\n", encoding="utf-8")

	# The items.

	def define(self, ident: str):
		"""Registers the helper of a scenario (a function of the Item), by the scenario's id."""
		def register(function):
			self.defined.append((ident, function))
			return function
		return register

	def go(self, before=None, after=None) -> int:
		"""Runs the helpers registered (those --only picks), each one's verdict and record written; before and after
		run around every one (the applications ended, a setting put back).  Returns 0."""
		if self.listing:
			for ident, _ in self.defined:
				print(ident)
			return 0
		records = self.outdir / "records"
		records.mkdir(parents=True, exist_ok=True)
		for ident, function in self.defined:
			if self.only and not self.only.search(ident):
				continue
			title = scenario_header(ident).get("title", ident)
			item = Item(self, ident, title)
			print(f"AAT {ident} ... {title}", flush=True)
			start = self.log_start()
			try:
				if before:
					before(item)
				function(item)
				if item.verdict is None:
					item.verdict, item.note = "fail", "the helper set no verdict"
			except ItemEnded:
				pass
			except Exception as error:
				item.verdict = "fail"
				item.note = " ".join(str(error).split()) or type(error).__name__
				with open(self.outdir / "errors.txt", "a", encoding="utf-8") as errors:
					errors.write(f"== {ident}\n{traceback.format_exc()}\n")
			if after:
				try:
					after(item)
				except Exception:
					with open(self.outdir / "errors.txt", "a", encoding="utf-8") as errors:
						errors.write(f"== {ident} (after)\n{traceback.format_exc()}\n")
			seconds = time.monotonic() - item.started
			self.save_log(ident, start)
			(records / f"{ident}.md").write_text(item.record(), encoding="utf-8")
			line = "\t".join([ident, item.verdict, title, ",".join(item.pictures), item.note, f"{seconds:.0f}", self.scenario])
			with open(self.outdir / "verdicts.tsv", "a", encoding="utf-8") as verdicts:
				verdicts.write(line + "\n")
			print(f"AAT {ident} {item.verdict} {item.note}", flush=True)
		return 0


def number(line: str | None, name: str) -> int | None:
	"""A field NAME=N of a log line, as an integer."""
	if not line:
		return None
	match = re.search(rf"\b{re.escape(name)}=(-?\d+)", line)
	return int(match.group(1)) if match else None


def field(line: str | None, name: str) -> str | None:
	"""A field NAME=VALUE of a log line (to the next space)."""
	if not line:
		return None
	match = re.search(rf"\b{re.escape(name)}=(\S*)", line)
	return match.group(1) if match else None

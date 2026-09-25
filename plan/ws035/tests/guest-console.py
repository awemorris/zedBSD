#!/usr/bin/env python3
"""Type at a booted guest's console and photograph what it answers.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

plan/tools/boot-test.py watches a guest boot and stops at the login prompt.
This carries that on: it waits for a line to appear on the screen, sends
keystrokes with the emulator's own key injection, and reads the screen back
with the same font matching.  No serial port and no console log are read --
the screen is the answer, the way a person at the machine would see it.

    guest-console.py --monitor SOCK --screenshot OUT.png \\
        --expect 'login: ' --send 'root' \\
        --expect '[#$] ' --send 'which ls' ...

--expect waits for a regular expression to match a line.  --send types one
line and presses Return.  They may be repeated and are applied in order.
The exit status is zero when every --expect matched in turn.
"""
from __future__ import annotations

import argparse
import importlib.util
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# The key names the emulator knows, for the characters a shell line uses.
KEYS = {
	" ": "spc", "-": "minus", "=": "equal", "[": "bracket_left",
	"]": "bracket_right", "\\": "backslash", ";": "semicolon",
	"'": "apostrophe", "`": "grave_accent", ",": "comma", ".": "dot",
	"/": "slash",
}
SHIFTED = {
	"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
	"*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal",
	"{": "bracket_left", "}": "bracket_right", "|": "backslash",
	":": "semicolon", '"': "apostrophe", "~": "grave_accent",
	"<": "comma", ">": "dot", "?": "slash",
}


def load_boot_test():
	"""Loads plan/tools/boot-test.py for its screen reader."""
	path = ROOT / "plan/tools/boot-test.py"
	spec = importlib.util.spec_from_file_location("boot_test", path)
	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)
	return module


def key_for(character: str) -> tuple[bool, str]:
	"""Returns whether shift is held, and the emulator key name, for one character.

	Whether a character is shifted is decided here and carried as its own
	value.  Reading it back out of the key name would be wrong for the
	keys whose names hold a hyphen, and a chord built from a misread name
	leaves shift down: the rest of the line then arrives in capitals.
	"""
	if character.isdigit() or ("a" <= character <= "z"):
		return False, character
	if "A" <= character <= "Z":
		return True, character.lower()
	if character in SHIFTED:
		return True, SHIFTED[character]
	if character in KEYS:
		return False, KEYS[character]
	raise SystemExit(f"guest-console: cannot type {character!r}")


def send_line(monitor, text: str) -> None:
	"""Types one line and presses Return."""
	for character in text:
		shifted, name = key_for(character)
		keys = [{"type": "qcode", "data": "shift"}] if shifted else []
		keys.append({"type": "qcode", "data": name})

		# The key is held briefly and then released, rather than for
		# the emulator's own tenth of a second: a press still down
		# when the next one arrives drops characters out of the
		# middle of a command.
		monitor.command("send-key", keys=keys, **{"hold-time": 20})
		time.sleep(0.12)
	monitor.command("send-key",
			keys=[{"type": "qcode", "data": "ret"}],
			**{"hold-time": 20})


def wait_quiet(boot_test, monitor, frame: Path, font, timeout: float) -> None:
	"""Waits until the screen stops changing.

	A guest still writing to its console eats keystrokes: the line editor
	sees them, but the daemon writing over the same screen leaves the
	command broken.  Typing starts once two readings in a row agree.
	"""
	deadline = time.monotonic() + timeout
	previous = None
	while time.monotonic() < deadline:
		monitor.screendump(frame)
		current = boot_test.read_text(frame, font)
		if current == previous:
			return
		previous = current
		time.sleep(2.0)


def wait_for(boot_test, monitor, frame: Path, pattern: str,
	     font, timeout: float) -> list[str]:
	"""Waits until one line of the screen matches, returning the screen."""
	expression = re.compile(pattern)
	deadline = time.monotonic() + timeout
	rows: list[str] = []
	while time.monotonic() < deadline:
		monitor.screendump(frame)
		rows = boot_test.read_text(frame, font)
		if any(expression.search(line) for line in rows):
			return rows
		time.sleep(1.0)
	print(f"guest-console: never saw {pattern!r}", file=sys.stderr)
	for line in rows:
		if line:
			print(f"  screen| {line}", file=sys.stderr)
	raise SystemExit(1)


def main() -> int:
	"""Runs the console conversation."""
	parser = argparse.ArgumentParser()
	parser.add_argument("--monitor", required=True)
	parser.add_argument("--screenshot", required=True)
	parser.add_argument("--timeout", type=float, default=180.0)
	parser.add_argument("--expect", action="append", default=[])
	parser.add_argument("--send", action="append", default=[])
	parser.add_argument("--script", action="append", default=[],
			    help="expect:PATTERN or send:TEXT, applied in order")
	arguments = parser.parse_args()

	boot_test = load_boot_test()
	font = boot_test.load_font(
		ROOT / "src/drivers/generic/vgafont.c"
		if (ROOT / "src/drivers/generic/vgafont.c").exists()
		else ROOT / "src/drivers/platform/pcat/graphics/vgafont.c")
	screenshot = Path(arguments.screenshot)
	frame = screenshot.with_suffix(".ppm")
	monitor = boot_test.Monitor(arguments.monitor, timeout=30.0)

	steps = list(arguments.script)
	for pattern, text in zip(arguments.expect, arguments.send):
		steps.append("expect:" + pattern)
		steps.append("send:" + text)

	rows: list[str] = []
	for step in steps:
		kind, _, value = step.partition(":")
		if kind == "expect":
			rows = wait_for(boot_test, monitor, frame, value, font,
					arguments.timeout)
		elif kind == "send":
			wait_quiet(boot_test, monitor, frame, font, 60.0)
			send_line(monitor, value)
			time.sleep(1.0)
		else:
			raise SystemExit(f"guest-console: unknown step {step!r}")

	# Keeps the last screen whether or not anything was expected after it.
	monitor.screendump(frame)
	rows = boot_test.read_text(frame, font)
	boot_test.write_png(frame, screenshot)
	frame.unlink(missing_ok=True)
	screenshot.with_suffix(".txt").write_text(
		"\n".join(rows) + "\n", encoding="utf-8")
	print(f"guest-console: {screenshot}")
	return 0


if __name__ == "__main__":
	sys.exit(main())

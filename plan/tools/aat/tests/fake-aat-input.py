#!/usr/bin/env python3
"""The self-test's stand-in for P1's aat-input (userland/tests/aat-input): the same command line and answers.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

"start --width W --height H" makes the server (a state file in
FAKE_AAT_DIR, and a child in the background that keeps the descriptors
it was started with, as the real server does) and prints
"AAT-INPUT ready ..."; "stop" ends it; every
other command is checked as aat-input checks it, written to
FAKE_AAT_DIR/record, and answered "ok" or "error WHY" (status 1).
"""
import os
import sys
import time

DIRECTORY = os.environ["FAKE_AAT_DIR"]
STATE = os.path.join(DIRECTORY, "server")
KEYS = {"esc", "enter", "tab", "space", "backspace", "delete", "up", "down", "left", "right", "home", "end",
	"ctrl", "alt", "shift", "super", "meta", "leftctrl", "leftalt", "leftshift", "f5", "t", "a", "c", "v"}
BUTTONS = {"left", "right", "middle"}


def numbers(words, count):
	"""Tells whether words are count integers."""
	try:
		return len(words) == count and all(int(word) is not None for word in words)
	except ValueError:
		return False


def inside(words, width, height):
	"""Tells whether two words are a point of the area."""
	return numbers(words, 2) and 0 <= int(words[0]) < width and 0 <= int(words[1]) < height


def valid(words, width, height):
	"""Tells whether a command is one the server takes."""
	verb, rest = words[0], words[1:]
	if verb == "move-to":
		return inside(rest, width, height)
	if verb in ("move",):
		return numbers(rest, 2)
	if verb in ("click", "double-click"):
		if rest and rest[0] in BUTTONS:
			rest = rest[1:]
		return not rest or inside(rest, width, height)
	if verb in ("down", "up"):
		return len(rest) == 1 and rest[0] in BUTTONS
	if verb == "drag":
		return len(rest) in (4, 5) and inside(rest[0:2], width, height) and inside(rest[2:4], width, height)
	if verb in ("tap", "double-tap"):
		return inside(rest, width, height)
	if verb == "touch-drag":
		return len(rest) in (4, 5) and inside(rest[0:2], width, height) and inside(rest[2:4], width, height)
	if verb in ("touch-down", "touch-move"):
		return len(rest) == 3 and numbers(rest[0:1], 1) and 0 <= int(rest[0]) < 10 and inside(rest[1:3], width, height)
	if verb == "touch-up":
		return numbers(rest, 1) and 0 <= int(rest[0]) < 10
	if verb in ("wheel", "hwheel", "sleep"):
		return numbers(rest, 1)
	if verb == "key":
		return len(rest) == 1 and all(name in KEYS for name in rest[0].split("+"))
	if verb in ("key-down", "key-up"):
		return len(rest) == 1 and rest[0] in KEYS
	if verb == "type":
		return bool(rest)
	return False


def main():
	"""Runs one command line."""
	words = sys.argv[1:]
	if not words:
		return 2
	if words[0] == "start":
		width, height = int(words[words.index("--width") + 1]), int(words[words.index("--height") + 1])
		with open(STATE, "w", encoding="utf-8") as state:
			state.write(f"{width} {height}\n")
		# Like the real server, a child stays in the background holding the descriptors it was started with,
		# until stop (or two minutes): a caller that waits for the end of its output waits for that (T1-200).
		sys.stdout.flush()
		child = os.fork()
		if child == 0:
			os.setsid()
			for _ in range(600):
				if not os.path.exists(STATE):
					break
				time.sleep(0.2)
			os._exit(0)
		print(f"AAT-INPUT ready width={width} height={height} pid={child}")
		return 0
	if not os.path.exists(STATE):
		print("error no-server (aat-input start first)")
		return 1
	if words[0] == "stop":
		os.unlink(STATE)
		print("ok")
		return 0
	with open(STATE, encoding="utf-8") as state:
		width, height = (int(value) for value in state.read().split())
	if not valid(words, width, height):
		print(f"error bad {words[0]}")
		return 1
	with open(os.path.join(DIRECTORY, "record"), "a", encoding="utf-8") as record:
		record.write(" ".join(words) + "\n")
	print("ok")
	return 0


if __name__ == "__main__":
	sys.exit(main())

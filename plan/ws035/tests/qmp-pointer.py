#!/usr/bin/env python3
"""Drives the guest's pointer (usb-tablet) through one QMP connection.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

Each step is one word and its numbers, run in order:
    move X Y      the pointer to output pixel (X, Y)
    down / up     the left button
    wheel-down / wheel-up   one notch of the wheel
    sleep MS      a pause
Pixels are output pixels of an output of --width x --height; zdesktop takes the
tablet value v to pixel floor(v * (size - 1) / 32767), so v is rounded up.
One connection keeps the steps close together (a double click is two
presses within zdesktop's 400 ms).

    qmp-pointer.py SOCKET [--width 1280 --height 800] move 10 20 down sleep 60 up
"""
import argparse
import json
import socket
import sys
import time


def send(stream, command, arguments):
	"""Sends one command and waits for its reply (events may come first)."""
	stream.write(json.dumps({"execute": command, "arguments": arguments}) + "\n")
	stream.flush()
	while True:
		reply = json.loads(stream.readline())
		if "return" in reply:
			return
		if "error" in reply:
			raise RuntimeError(reply["error"])


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("socket")
	parser.add_argument("--width", type=int, default=1280)
	parser.add_argument("--height", type=int, default=800)
	parser.add_argument("steps", nargs=argparse.REMAINDER)
	arguments = parser.parse_args()
	connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	connection.connect(arguments.socket)
	stream = connection.makefile("rw")
	stream.readline()
	send(stream, "qmp_capabilities", {})
	steps = arguments.steps
	index = 0
	while index < len(steps):
		word = steps[index]
		if word == "move":
			x = int(steps[index + 1])
			y = int(steps[index + 2])
			vx = (x * 32767 + arguments.width - 2) // (arguments.width - 1)
			vy = (y * 32767 + arguments.height - 2) // (arguments.height - 1)
			send(stream, "input-send-event", {"events": [
				{"type": "abs", "data": {"axis": "x", "value": vx}},
				{"type": "abs", "data": {"axis": "y", "value": vy}}]})
			index += 3
		elif word in ("down", "up"):
			send(stream, "input-send-event", {"events": [
				{"type": "btn", "data": {"down": word == "down", "button": "left"}}]})
			index += 1
		elif word in ("wheel-down", "wheel-up"):
			for down in (True, False):
				send(stream, "input-send-event", {"events": [
					{"type": "btn", "data": {"down": down, "button": word}}]})
			index += 1
		elif word == "sleep":
			time.sleep(int(steps[index + 1]) / 1000.0)
			index += 2
		else:
			print("qmp-pointer: unknown step %s" % word, file=sys.stderr)
			return 2
	return 0


if __name__ == "__main__":
	sys.exit(main())

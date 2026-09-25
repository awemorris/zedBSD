#!/usr/bin/env python3
"""Talk to a guest over its serial console.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

A guest built with CONFIG_PCAT_SERIAL_MIRROR=y puts its console on the first
serial port and reads what is typed there.  This is the way to drive one
without a keyboard: a character sent on the line arrives whole, where a key
event injected into the emulator is lost out of the middle of a command once
the guest is busy.

    serial.py --socket S login                       log in as root
    serial.py --socket S run 'net show'              run one command
    serial.py --socket S expect 'login: '            wait for a line

`run` prints what the command printed and exits with what the command
exited with, so that a caller can treat it as the command itself.
"""
from __future__ import annotations

import argparse
import re
import socket
import sys
import time

# What the shell shows when it is waiting, and what login asks.
PROMPT = re.compile(r"[#$] $|[#$] \Z")
MARKER = "ZEDBSD-SERIAL-STATUS"


class Console:
	"""One connection to the guest's serial line."""

	def __init__(self, path: str, timeout: float) -> None:
		self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		self.socket.connect(path)
		self.socket.settimeout(0.5)
		self.timeout = timeout
		self.pending = ""

	def read_until(self, pattern: re.Pattern, timeout: float | None = None) -> str:
		"""Reads until the output matches, returning everything read."""
		deadline = time.monotonic() + (timeout or self.timeout)
		while time.monotonic() < deadline:
			if pattern.search(self.pending):
				return self.pending
			try:
				chunk = self.socket.recv(4096)
			except socket.timeout:
				continue
			if not chunk:
				break
			self.pending += chunk.decode("utf-8", "replace")
		raise SystemExit(
			f"serial: never saw {pattern.pattern!r}\n"
			f"--- last output ---\n{self.pending[-2000:]}")

	def send(self, text: str) -> None:
		"""Types one line.

		A terminal sends a carriage return for the return key, which is
		what the console turns back into a newline.
		"""
		self.socket.sendall(text.encode() + b"\r")

	def forget(self) -> None:
		"""Drops what has been read, so the next wait starts clean."""
		self.pending = ""


def login(console: Console, user: str) -> None:
	"""Logs in, or does nothing if a shell is already there."""
	console.send("")
	try:
		console.read_until(re.compile(r"login: |[#$] "), timeout=20.0)
	except SystemExit:
		raise

	# A shell that is already waiting needs no login.
	if PROMPT.search(console.pending):
		console.forget()
		return
	console.forget()
	console.send(user)
	console.read_until(re.compile(r"[Pp]assword"), timeout=30.0)
	console.forget()

	# The account has no password; the empty line is the answer to the ask.
	console.send("")
	console.read_until(PROMPT, timeout=60.0)
	console.forget()


def run(console: Console, command: str) -> int:
	"""Runs one command and reports what it printed and returned.

	The status is carried back in a line of its own rather than read from
	the prompt, because the prompt does not show it and the console is
	shared with whatever else the system is writing.
	"""
	console.forget()

	# A command that ends in & is already terminated, and "&;" is not a
	# command list the shell accepts; the marker is then its own command.
	# Its status is that of starting the background job, which is zero.
	separator = " " if command.rstrip().endswith("&") else "; "
	console.send(f"{command}{separator}echo {MARKER}=$?")
	pattern = re.compile(re.escape(MARKER) + r"=(\d+)")
	output = console.read_until(pattern)
	match = pattern.search(output)
	assert match is not None

	# What the command printed is between the echo of the line and the
	# marker; the echo is dropped so that a caller sees only the answer.
	body = output[:match.start()]
	newline = body.find("\n")
	if newline >= 0:
		body = body[newline + 1:]
	sys.stdout.write(body)
	sys.stdout.flush()
	return int(match.group(1))


def main() -> int:
	"""Runs one console command."""
	parser = argparse.ArgumentParser(prog="serial")
	parser.add_argument("--socket", required=True)
	parser.add_argument("--timeout", type=float, default=120.0)
	parser.add_argument("--user", default="root")
	commands = parser.add_subparsers(dest="command", required=True)
	commands.add_parser("login")
	run_parser = commands.add_parser("run")
	run_parser.add_argument("line")
	expect_parser = commands.add_parser("expect")
	expect_parser.add_argument("pattern")
	arguments = parser.parse_args()

	console = Console(arguments.socket, arguments.timeout)
	if arguments.command == "login":
		login(console, arguments.user)
		print("serial: logged in")
		return 0
	if arguments.command == "expect":
		console.read_until(re.compile(arguments.pattern))
		return 0
	login(console, arguments.user)
	return run(console, arguments.line)


if __name__ == "__main__":
	sys.exit(main())

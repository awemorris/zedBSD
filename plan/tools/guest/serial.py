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
import codecs
import os
import re
import secrets
import socket
import sys
import time

# What the shell shows when it is waiting, and what login asks.
PROMPT = re.compile(r"[#$] $|[#$] \Z")
MARKER = "ZEDBSD-SERIAL-STATUS"

# The shell variable that keeps a command's status until its line is read.
STATUS_VARIABLE = "__zedbsd_serial_status"

# The kernel writes its records on the same console, a character at a time
# between the shell's own: a record can land in the middle of the status
# line, which then no longer matches.  A broken line is still there with its
# letters in order and others between them; when the line has been silent
# this long after one, the status is asked for again, at most this often.
# A kernel record is at most 512 bytes, which bounds what lands inside one.
QUIET_SECONDS = 2.0
ASK_LIMIT = 5
BROKEN_SPAN = 1024


class Console:
	"""One connection to the guest's serial line."""

	def __init__(self, path: str, timeout: float) -> None:
		self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		self.socket.connect(path)
		self.socket.settimeout(0.5)
		self.timeout = timeout
		self.pending = ""
		# A character the guest writes one byte at a time can arrive in separate reads.
		self.decoder = codecs.getincrementaldecoder("utf-8")("replace")

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
			self.pending += self.decoder.decode(chunk)
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

	def read_status(self, pattern: re.Pattern, marker: str, again: str) -> tuple[str, int | None]:
		"""Reads until the status line matches, asking again for a broken one.

		Returns everything read and where the first broken status line
		started (None when the first one was whole).  A broken line is the
		marker's letters in order, apart from the copy in the echo of the
		typed line; the shell has run the command by the time it prints one,
		so the line typed again goes to the shell.
		"""
		deadline = time.monotonic() + self.timeout
		silent_since = time.monotonic()
		broken = None
		asked = 0
		while time.monotonic() < deadline:
			if pattern.search(self.pending):
				return self.pending, broken
			try:
				chunk = self.socket.recv(4096)
			except socket.timeout:
				# A silent line that holds a broken status line asks for the status again.
				if time.monotonic() - silent_since >= QUIET_SECONDS and asked < ASK_LIMIT:
					echo = scattered(self.pending, marker, 0)
					copy = None
					if echo is not None:
						copy = scattered(self.pending, marker, echo[1])
					if copy is not None:
						if broken is None:
							broken = copy[0]
						self.send(again)
						asked += 1
						silent_since = time.monotonic()
				continue
			if not chunk:
				break
			self.pending += self.decoder.decode(chunk)
			silent_since = time.monotonic()
		raise SystemExit(
			f"serial: never saw {pattern.pattern!r}\n"
			f"--- last output ---\n{self.pending[-2000:]}")


def scattered(text: str, word: str, start: int) -> tuple[int, int] | None:
	"""Finds word in text from start, its letters in order with others between.

	Returns where the first such copy starts and where it ends, or None.  A
	copy spans at most BROKEN_SPAN characters more than the word.
	"""
	first = text.find(word[0], start)
	while first >= 0:
		at = first
		for letter in word:
			at = text.find(letter, at)
			if at < 0:
				return None
			if at - first >= len(word) + BROKEN_SPAN:
				break
			at += 1
		else:
			return first, at
		first = text.find(word[0], first + 1)
	return None


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

	# The password is the account's name (root is root, user is user, and kei is kei in the older images), or
	# ZEDBSD_GUEST_PASSWORD.
	console.send(os.environ.get("ZEDBSD_GUEST_PASSWORD", user))
	console.read_until(PROMPT, timeout=60.0)
	console.forget()


def run(console: Console, command: str) -> int:
	"""Runs one command and reports what it printed and returned.

	The status is carried back in a line of its own rather than read from
	the prompt, because the prompt does not show it and the console is
	shared with whatever else the system is writing.  The shell keeps the
	status in a variable, so that a status line broken up by a kernel record
	can be asked for again (Console.read_status).
	"""
	console.forget()

	# A command that ends in & is already terminated, and "&;" is not a
	# command list the shell accepts; the status is then its own command.
	# Its status is that of starting the background job, which is zero.
	# The marker carries a number of this run's own, so that a status line
	# a slow guest prints after an earlier run gave up is not taken for this one.
	marker = f"{MARKER}-{secrets.randbelow(1000000000):09d}"
	separator = " " if command.rstrip().endswith("&") else "; "
	again = f"echo {marker}=${STATUS_VARIABLE}"
	console.send(f"{command}{separator}{STATUS_VARIABLE}=$?; {again}")
	pattern = re.compile(re.escape(marker) + r"=(\d+)\r*\n")
	output, broken = console.read_status(pattern, marker + "=", again)
	match = pattern.search(output)
	assert match is not None

	# What the command printed is between the echo of the line and the
	# marker (or the broken marker); the echo is dropped so that a caller
	# sees only the answer.
	end = match.start()
	if broken is not None:
		end = broken
	body = output[:end]
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

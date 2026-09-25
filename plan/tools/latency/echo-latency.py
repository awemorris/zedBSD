#!/usr/bin/env python3
"""Times how soon the shell echoes a key typed on the serial console while a
busy loop runs beside it (WS041 p003).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

The shell reads the terminal in raw mode and echoes each key itself, so the
echo waits for the shell process to run.  A busy shell loop is started in the
background first.  Each key is sent alone and the time until it comes back is
the echo latency.

    echo-latency.py SOCKET [COUNT] [--quiet] [--c-spin] [--rawecho]
      --quiet    no busy loop; --c-spin  a C busy loop instead of a shell loop
      --rawecho  a raw-mode C echo program instead of the shell's line editor
"""
import socket
import statistics
import sys
import time

sys.path.insert(0, "plan/tools/guest")
import serial  # noqa: E402

console = serial.Console(sys.argv[1], 30.0)
count = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 100
serial.login(console, "root")
busy = "--quiet" not in sys.argv
if busy:
	console.send("rawecho spin &" if "--c-spin" in sys.argv else
		"sh -c 'while :; do :; done' &")
time.sleep(1.0)
raw = "--rawecho" in sys.argv
if raw:
	console.send("rawecho")
time.sleep(1.0)
console.forget()
console.socket.settimeout(0.001)
samples = []
for index in range(count):
	key = b"abcdefghij"[index % 10:index % 10 + 1]
	try:
		while console.socket.recv(4096):
			pass
	except (socket.timeout, BlockingIOError):
		pass
	start = time.monotonic()
	console.socket.sendall(key)
	seen = b""
	while key not in seen and time.monotonic() - start < 2.0:
		try:
			seen += console.socket.recv(4096)
		except (socket.timeout, BlockingIOError):
			pass
	samples.append((time.monotonic() - start) * 1000.0)
	if index % 20 == 19:
		console.socket.sendall(b"\x15")
		time.sleep(0.05)
console.socket.settimeout(0.5)
if raw:
	console.socket.sendall(b"q")
	time.sleep(0.2)
console.socket.sendall(b"\x15")
if busy:
	console.send("kill %1")
samples.sort()
print("ECHO n=%d median=%.2fms p90=%.2fms max=%.2fms" % (
	count, statistics.median(samples), samples[int(count * 0.9) - 1], samples[-1]))

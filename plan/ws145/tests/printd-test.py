#!/usr/bin/env python3
"""Drives keiland-printd as libkeiland-backend does (ws145-p002), against mock-printers.py.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    printd-test.py PRINTD FOLDER

The daemon gets a socket on descriptor 3 and a runtime directory of its own; the test sends jobs (the documents'
descriptors beside the lines) to the mock IPP printer and LPD queue, a NAME, a document that is not a PDF, a CANCEL of a
job not known, and BYE, and checks the daemon's lines, the documents the printers got (SHA-256), and that the spool is
gone at the end.  Prints "PASS name" or "FAIL name ..." and exits with 1 when one failed.
"""
import hashlib
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

PRINTD = sys.argv[1]
FOLDER = Path(sys.argv[2]).resolve()
failures = 0


def check(name, ok, detail=""):
	global failures
	print(("PASS " if ok else "FAIL ") + name + ("" if ok else ": " + detail), flush=True)
	if not ok:
		failures += 1


def main():
	runtime = FOLDER / "runtime"
	runtime.mkdir(parents=True, exist_ok=True)
	os.chmod(runtime, 0o700)
	printers = FOLDER / "printers"
	mock = subprocess.Popen([sys.executable, str(Path(__file__).parent / "mock-printers.py"), str(printers)], stdout=subprocess.PIPE,
		text=True)
	ipp_port, lpd_port = (int(word) for word in mock.stdout.readline().split()[1:])
	ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
	env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime))
	# Descriptor 3 is made in the child (close_fds would close it again); the daemon closes the others itself.
	daemon = subprocess.Popen([PRINTD], close_fds=False, env=env, preexec_fn=lambda: os.dup2(theirs.fileno(), 3))
	theirs.close()
	lines = []
	buffer = b""

	def read_until(predicate, seconds=30):
		nonlocal buffer
		ours.settimeout(0.5)
		end = time.time() + seconds
		while time.time() < end:
			for line in lines:
				if predicate(line):
					return line
			try:
				data = ours.recv(4096)
			except socket.timeout:
				continue
			if not data:
				break
			buffer += data
			while b"\n" in buffer:
				line, buffer = buffer.split(b"\n", 1)
				lines.append(line.decode())
		return None

	sent = 0

	def send(line, fd=None):
		nonlocal sent
		sent += 1
		if fd is None:
			ours.sendall(line.encode() + b"\n")
		else:
			socket.send_fds(ours, [line.encode() + b"\n"], [fd])

	spool = read_until(lambda line: line.startswith("SPOOL "))
	check("spool", spool is not None and spool.split(" ", 1)[1].startswith(str(runtime)), str(spool))

	# A PDF to the IPP printer at a wrong path: the daemon finds /ipp/print.
	document = b"%PDF-1.4\n" + os.urandom(300000) + b"\n%%EOF\n"
	(FOLDER / "doc.pdf").write_bytes(document)
	with open(FOLDER / "doc.pdf", "rb") as file:
		send(f"JOB 1 ipp 127.0.0.1 {ipp_port} /wrong Report one", file.fileno())
	check("ipp-accepted", read_until(lambda line: line == "ACCEPTED 1") is not None, str(lines))
	check("ipp-path", read_until(lambda line: line == "PATH 1 /ipp/print") is not None, str(lines))
	check("ipp-waiting", read_until(lambda line: line == "STATE 1 waiting") is not None, str(lines))
	check("ipp-done", read_until(lambda line: line == "STATE 1 done", 20) is not None, str(lines))
	got = (printers / "ipp-1.pdf").read_bytes() if (printers / "ipp-1.pdf").exists() else b""
	check("ipp-document", hashlib.sha256(got).hexdigest() == hashlib.sha256(document).hexdigest(), f"{len(got)} bytes")
	check("ipp-name", (printers / "ipp-1.name").read_bytes() == b"Report one", "job-name")
	transfer = (printers / "ipp-1.transfer").read_text() if (printers / "ipp-1.transfer").exists() else ""
	check("ipp-chunked", transfer == "chunked", f"the Print-Job's body came by {transfer!r} (ws177-p032, BUG-271)")

	# The same to the LPD queue.
	with open(FOLDER / "doc.pdf", "rb") as file:
		send(f"JOB 2 lpd 127.0.0.1 {lpd_port} lp Report two", file.fileno())
	check("lpd-accepted", read_until(lambda line: line == "ACCEPTED 2") is not None, str(lines))
	check("lpd-done", read_until(lambda line: line == "STATE 2 done") is not None, str(lines))
	got = (printers / "lpd-1.data").read_bytes() if (printers / "lpd-1.data").exists() else b""
	check("lpd-document", hashlib.sha256(got).hexdigest() == hashlib.sha256(document).hexdigest(), f"{len(got)} bytes")
	control = (printers / "lpd-1.control").read_bytes().decode() if (printers / "lpd-1.control").exists() else ""
	check("lpd-control", "JReport two\n" in control and "ldfA" in control, control)

	# A name, a document that is not a PDF, a job not known.
	send(f"NAME 7 127.0.0.1 {ipp_port}")
	check("named", read_until(lambda line: line == "NAMED 7 /ipp/print Mock Printer") is not None, str(lines))
	(FOLDER / "doc.txt").write_bytes(b"plain text\n")
	with open(FOLDER / "doc.txt", "rb") as file:
		send(f"JOB 3 ipp 127.0.0.1 {ipp_port} /ipp/print Text", file.fileno())
	check("not-pdf", read_until(lambda line: line == "REJECTED 3 format") is not None, str(lines))
	send("CANCEL 99")
	check("cancel-unknown", read_until(lambda line: line == "STATE 99 cancelled unknown") is not None, str(lines))

	# BYE with the count: the daemon ends and its spool goes.
	time.sleep(1.0)
	send(f"BYE {sent}")
	try:
		status = daemon.wait(timeout=10)
	except subprocess.TimeoutExpired:
		daemon.kill()
		status = None
	left = list((runtime / "keiland-print").iterdir()) if (runtime / "keiland-print").exists() else []
	check("bye", status == 0 and not left, f"status {status}, left {left}")
	mock.terminate()
	print("printd-test: " + ("PASS" if failures == 0 else f"FAIL {failures}"))
	return 1 if failures else 0


if __name__ == "__main__":
	sys.exit(main())

#!/usr/bin/env python3
"""ws177-p022: keiland-printd's turns, bounds, waits and protocol ends, against mock-printers-q.py.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    host-printd-q.py PRINTD PRINTD-PLAIN RESOLVER FOLDER

PRINTD is built with the sanitizers, PRINTD-PLAIN without (the slow resolver RESOLVER is preloaded into it).  Prints
"PASS name" or "FAIL name ..." and exits 1 when one failed.
"""
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

PRINTD, PLAIN, RESOLVER = sys.argv[1], sys.argv[2], sys.argv[3]
FOLDER = Path(sys.argv[4]).resolve()
failures = 0


def check(name, ok, detail=""):
	global failures
	print(("PASS " if ok else "FAIL ") + name + ("" if ok else ": " + detail), flush=True)
	if not ok:
		failures += 1


class Daemon:
	"""A keiland-printd on a socket pair, as the backend runs it."""

	def __init__(self, program, name, preload=None):
		runtime = FOLDER / f"runtime-{name}"
		runtime.mkdir(parents=True, exist_ok=True)
		os.chmod(runtime, 0o700)
		self.ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
		env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), ASAN_OPTIONS="detect_leaks=0")
		if preload:
			env["LD_PRELOAD"] = preload
		self.process = subprocess.Popen([program], close_fds=False, env=env, preexec_fn=lambda: os.dup2(theirs.fileno(), 3))
		theirs.close()
		self.lines = []
		self.buffer = b""
		self.sent = 0

	def read_until(self, predicate, seconds=20):
		self.ours.settimeout(0.2)
		end = time.time() + seconds
		while time.time() < end:
			for line in self.lines:
				if predicate(line):
					return line
			try:
				data = self.ours.recv(4096)
			except socket.timeout:
				continue
			if not data:
				break
			self.buffer += data
			while b"\n" in self.buffer:
				line, self.buffer = self.buffer.split(b"\n", 1)
				self.lines.append(line.decode())
		for line in self.lines:
			if predicate(line):
				return line
		return None

	def when(self, line, seconds=20):
		"""The time a line came (or None)."""
		start = time.time()
		found = self.read_until(lambda text: text == line, seconds)
		return None if found is None else time.time() - start

	def send(self, line, fd=None):
		self.sent += 1
		if fd is None:
			self.ours.sendall(line.encode() + b"\n")
		else:
			socket.send_fds(self.ours, [line.encode() + b"\n"], [fd])

	def job(self, number, protocol, port, path, document, host="127.0.0.1"):
		with open(document, "rb") as file:
			self.send(f"JOB {number} {protocol} {host} {port} {path} Job {number}", file.fileno())

	def end(self):
		self.ours.close()
		try:
			return self.process.wait(timeout=15)
		except subprocess.TimeoutExpired:
			self.process.kill()
			return None


def mode(index, text):
	(FOLDER / "printers" / f"mode-{index}").write_text(text + "\n")


def main():
	printers = FOLDER / "printers"
	mock = subprocess.Popen([sys.executable, str(Path(__file__).parent / "mock-printers-q.py"), str(printers), "6"],
		stdout=subprocess.PIPE, text=True)
	ports = [int(word) for word in mock.stdout.readline().split()[1:]]
	ipp, lpd = ports[:6], ports[6]
	document = FOLDER / "doc.pdf"
	document.write_bytes(b"%PDF-1.4\n" + os.urandom(20000) + b"\n%%EOF\n")

	# 1. Two jobs for one slow printer: one after the other; six for six slow printers: four at once.
	daemon = Daemon(PRINTD, "turns")
	daemon.read_until(lambda line: line.startswith("SPOOL "))
	for index in range(6):
		mode(index, "slow")
	daemon.job(1, "ipp", ipp[0], "/ipp/print", document)
	daemon.job(2, "ipp", ipp[0], "/ipp/print", document)
	ok = daemon.read_until(lambda line: line == "STATE 2 done", 30) is not None
	most = (printers / "concurrency").read_text().split() if (printers / "concurrency").exists() else ["?", "?"]
	check("one-printer-one-at-a-time", ok and most[1] == "1", f"{most} {daemon.lines}")
	for number in range(3, 9):
		daemon.job(number, "ipp", ipp[number - 3], "/ipp/print", document)
	ok = all(daemon.read_until(lambda line, n=number: line == f"STATE {n} done", 40) for number in range(3, 9))
	most = (printers / "concurrency").read_text().split()
	check("four-at-once", ok and most[0] == "4", f"{most} {[line for line in daemon.lines if line.startswith('STATE')]}")

	# 2. A waiting job is cancelled at once and never reaches the printer.
	before = len(list(printers.glob("ipp-0-*.pdf")))
	daemon.job(9, "ipp", ipp[0], "/ipp/print", document)
	daemon.job(10, "ipp", ipp[0], "/ipp/print", document)
	daemon.read_until(lambda line: line == "ACCEPTED 10")
	daemon.send("CANCEL 10")
	took = daemon.when("STATE 10 cancelled", 3)
	daemon.read_until(lambda line: line == "STATE 9 done", 20)
	time.sleep(1.0)
	after = len(list(printers.glob("ipp-0-*.pdf")))
	check("cancel-waiting", took is not None and after == before + 1, f"took {took}, documents {before} -> {after}")

	# 3. A busy printer's wait (30 s) ends within a few seconds of a cancel.
	mode(1, "busy")
	daemon.job(11, "ipp", ipp[1], "/ipp/print", document)
	time.sleep(2.0)
	daemon.send("CANCEL 11")
	took = daemon.when("STATE 11 cancelled", 5)
	check("cancel-busy-wait", took is not None, str(daemon.lines[-5:]))

	# 4. An answer of every attribute (100 Continue, chunked, 400 long values): understood, the job prints.
	mode(2, "big")
	daemon.job(12, "ipp", ipp[2], "/ipp/print", document)
	check("big-answer", daemon.read_until(lambda line: line == "STATE 12 done", 20) is not None,
		str([line for line in daemon.lines if " 12 " in line]))

	# 5. A header past 16 KiB: refused as protocol.
	mode(3, "header")
	daemon.job(13, "ipp", ipp[3], "/ipp/print", document)
	check("header-bound", daemon.read_until(lambda line: line.startswith("STATE 13 failed"), 20) == "STATE 13 failed protocol",
		str([line for line in daemon.lines if " 13 " in line]))

	# 6. LPD: a cancel while the data file's answer is awaited sends "abort job"; one after the control file is unconfirmed.
	daemon.job(14, "lpd", lpd, "lp", document)
	daemon.read_until(lambda line: line == "STATE 14 sending")
	time.sleep(0.5)
	daemon.send("CANCEL 14")
	daemon.read_until(lambda line: line.startswith("STATE 14 ") and "sending" not in line, 10)
	time.sleep(0.5)
	logs = sorted(printers.glob("lpd-*.log"))
	text = logs[-1].read_text() if logs else ""
	check("lpd-abort", "STATE 14 cancelled" in daemon.lines and "abort" in text, f"{text!r} {daemon.lines[-3:]}")
	daemon.job(15, "lpd", lpd, "lp", document)
	end = time.time() + 15
	while time.time() < end:
		logs = sorted(printers.glob("lpd-*.log"))
		if logs and "control-received" in logs[-1].read_text():
			break
		time.sleep(0.1)
	daemon.send("CANCEL 15")
	check("lpd-after-control", daemon.read_until(lambda line: line.startswith("STATE 15 ") and "sending" not in line, 15) ==
		"STATE 15 failed unconfirmed", str(daemon.lines[-3:]))
	# 6b. A printer that lists application/octet-stream and not PDF (BUG-271): the PDF goes as octet-stream; one that
	# lists neither fails format.
	mode(4, "octet")
	daemon.job(16, "ipp", ipp[4], "/ipp/print", document)
	done = daemon.read_until(lambda line: line.startswith("STATE 16 ") and line.split()[2] in ("done", "failed"), 20)
	formats = [path.read_bytes() for path in printers.glob("ipp-4-*.format")]
	check("octet-stream", done == "STATE 16 done" and b"application/octet-stream" in formats, f"{done} {formats}")
	mode(5, "raster")
	daemon.job(17, "ipp", ipp[5], "/ipp/print", document)
	check("no-pdf-no-octet", daemon.read_until(lambda line: line.startswith("STATE 17 failed"), 20) == "STATE 17 failed format",
		str([line for line in daemon.lines if " 17 " in line]))
	pdf_formats = [path.read_bytes() for path in printers.glob("ipp-0-*.format")]
	check("pdf-stays-pdf", pdf_formats and all(value == b"application/pdf" for value in pdf_formats), str(pdf_formats))
	status = daemon.end()
	check("end-of-socket", status == 0, f"status {status}")

	# 7. The protocol broken: a word that is no command, a JOB without its document; the daemon ends.
	daemon = Daemon(PRINTD, "broken-word")
	daemon.read_until(lambda line: line.startswith("SPOOL "))
	daemon.send("PRINT 1")
	try:
		status = daemon.process.wait(timeout=10)
	except subprocess.TimeoutExpired:
		status = None
	check("unknown-command-ends", status is not None, f"status {status}")
	daemon.end()
	daemon = Daemon(PRINTD, "broken-job")
	daemon.read_until(lambda line: line.startswith("SPOOL "))
	daemon.send(f"JOB 1 ipp 127.0.0.1 {ipp[0]} /ipp/print No document")
	try:
		status = daemon.process.wait(timeout=10)
	except subprocess.TimeoutExpired:
		status = None
	check("job-without-document-ends", status is not None, f"status {status}")
	daemon.end()

	# 8. A name the resolver does not answer: failed timeout after about ten seconds, not thirty.
	daemon = Daemon(PLAIN, "resolver", preload=RESOLVER)
	daemon.read_until(lambda line: line.startswith("SPOOL "))
	start = time.time()
	daemon.job(1, "ipp", ipp[4], "/ipp/print", document, host="slow.test")
	line = daemon.read_until(lambda text: text.startswith("STATE 1 failed"), 25)
	took = time.time() - start
	check("lookup-bound", line == "STATE 1 failed timeout" and 8.0 <= took <= 15.0, f"{line} after {took:.1f} s")
	daemon.end()

	mock.terminate()
	print("host-printd-q: " + ("PASS" if failures == 0 else f"FAIL {failures}"))
	return 1 if failures else 0


if __name__ == "__main__":
	sys.exit(main())

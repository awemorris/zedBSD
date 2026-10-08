#!/usr/bin/env python3
"""Mock printers for keiland-printd's tests (ws145-p002): an IPP printer over HTTP and an LPD queue.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    mock-printers.py FOLDER [--bind ADDRESS]     (prints "PORTS <ipp> <lpd>", then serves until killed)

The IPP printer answers only at /ipp/print (other paths are 404, so that the daemon looks for the path), names itself
"Mock Printer", takes application/pdf, keeps each Print-Job's document as FOLDER/ipp-<n>.pdf and its job-name in
FOLDER/ipp-<n>.name (and how its body came, "chunked" or "length", in FOLDER/ipp-<n>.transfer), and reports a job processing at the first Get-Job-Attributes and completed after.  The LPD queue
keeps each data file as FOLDER/lpd-<n>.data and each control file as FOLDER/lpd-<n>.control.
"""
import socketserver
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

FOLDER = Path(sys.argv[1])
BIND = sys.argv[sys.argv.index("--bind") + 1] if "--bind" in sys.argv else "127.0.0.1"
LOCK = threading.Lock()
COUNTS = {"ipp": 0, "lpd": 0, "watch": {}}


def attribute(tag, name, value):
	data = value if isinstance(value, bytes) else value.encode()
	return bytes([tag]) + struct.pack(">H", len(name)) + name.encode() + struct.pack(">H", len(data)) + data


def parse(body):
	"""The operation, the request-id, the attributes by name (lists of values) and the document after them."""
	operation, request = struct.unpack(">HI", body[2:8])
	offset, name, attributes = 8, None, {}
	while offset < len(body):
		tag = body[offset]
		offset += 1
		if tag == 0x03:
			break
		if tag <= 0x0F:
			continue
		length = struct.unpack(">H", body[offset:offset + 2])[0]
		offset += 2
		if length:
			name = body[offset:offset + length].decode()
		offset += length
		length = struct.unpack(">H", body[offset:offset + 2])[0]
		offset += 2
		attributes.setdefault(name, []).append(body[offset:offset + length])
		offset += length
	return operation, request, attributes, body[offset:]


class Ipp(BaseHTTPRequestHandler):
	def log_message(self, *args):
		pass

	def read_body(self):
		"""The request's body, sent with a Content-Length or in chunks (ws177-p032: printd sends a document so)."""
		if "chunked" not in self.headers.get("Transfer-Encoding", "").lower():
			return self.rfile.read(int(self.headers.get("Content-Length", "0"))), "length"
		body = b""
		while True:
			size = int(self.rfile.readline().split(b";")[0].strip(), 16)
			if size == 0:
				while self.rfile.readline() not in (b"\r\n", b"\n", b""):
					pass
				return body, "chunked"
			body += self.rfile.read(size)
			self.rfile.readline()

	def do_POST(self):
		body, transfer = self.read_body()
		if self.path != "/ipp/print":
			self.send_response(404)
			self.send_header("Content-Length", "0")
			self.end_headers()
			return
		operation, request, attributes, document = parse(body)
		groups = b"\x01" + attribute(0x47, "attributes-charset", "utf-8") + attribute(0x48, "attributes-natural-language", "en")
		if operation == 0x000B:
			groups += b"\x04" + attribute(0x41, "printer-info", "Mock Printer") + attribute(0x41, "printer-make-and-model", "Mock 1")
			groups += attribute(0x49, "document-format-supported", "image/urf") + attribute(0x49, "", "application/pdf")
			groups += attribute(0x23, "printer-state", struct.pack(">I", 3))
		elif operation == 0x0002:
			with LOCK:
				COUNTS["ipp"] += 1
				number = COUNTS["ipp"]
			(FOLDER / f"ipp-{number}.pdf").write_bytes(document)
			(FOLDER / f"ipp-{number}.name").write_bytes(attributes.get("job-name", [b""])[0])
			(FOLDER / f"ipp-{number}.user").write_bytes(attributes.get("requesting-user-name", [b""])[0])
			(FOLDER / f"ipp-{number}.transfer").write_text(transfer)
			groups += b"\x02" + attribute(0x21, "job-id", struct.pack(">I", 100 + number)) + attribute(0x23, "job-state", struct.pack(">I", 3))
		elif operation == 0x0009:
			job = struct.unpack(">I", attributes["job-id"][0])[0]
			with LOCK:
				seen = COUNTS["watch"].get(job, 0)
				COUNTS["watch"][job] = seen + 1
			groups += b"\x02" + attribute(0x23, "job-state", struct.pack(">I", 5 if seen == 0 else 9))
		answer = struct.pack(">BBHI", 2, 0, 0, request) + groups + b"\x03"
		self.send_response(200)
		self.send_header("Content-Type", "application/ipp")
		self.send_header("Content-Length", str(len(answer)))
		self.end_headers()
		self.wfile.write(answer)


class Lpd(socketserver.StreamRequestHandler):
	def handle(self):
		line = self.rfile.readline()
		if not line.startswith(b"\x02"):
			return
		self.wfile.write(b"\0")
		with LOCK:
			COUNTS["lpd"] += 1
			number = COUNTS["lpd"]
		while True:
			line = self.rfile.readline()
			if not line:
				return
			kind = "data" if line[0] == 3 else "control"
			length = int(line[1:].split(b" ")[0])
			self.wfile.write(b"\0")
			data = self.rfile.read(length + 1)
			(FOLDER / f"lpd-{number}.{kind}").write_bytes(data[:length])
			self.wfile.write(b"\0")


class Threading(socketserver.ThreadingMixIn, HTTPServer):
	daemon_threads = True


class LpdServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
	daemon_threads = True
	allow_reuse_address = True


def main():
	FOLDER.mkdir(parents=True, exist_ok=True)
	ipp = Threading((BIND, 0), Ipp)
	lpd = LpdServer((BIND, 0), Lpd)
	threading.Thread(target=lpd.serve_forever, daemon=True).start()
	print(f"PORTS {ipp.server_address[1]} {lpd.server_address[1]}", flush=True)
	ipp.serve_forever()


if __name__ == "__main__":
	main()

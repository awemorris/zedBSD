#!/usr/bin/env python3
"""Mock printers for ws177-p022 (keiland-printd's bounds, turns and cancels).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    mock-printers-q.py FOLDER COUNT     (prints "PORTS <ipp>... <lpd>", then serves until killed)

COUNT IPP printers on ports of their own and one LPD queue.  Each IPP printer answers at /ipp/print, raw HTTP so that
the test can shape the answers; what it does is set by a file FOLDER/mode-<index> (read at each request):

    normal      as a printer does: Get-Printer-Attributes, Print-Job (a job-id), Get-Job-Attributes (completed)
    slow        Print-Job answered after 2 s (the turns are measured meanwhile)
    busy        Print-Job answered server-error-busy
    big         Get-Printer-Attributes after "100 Continue", chunked, with 400 attributes of 2000-byte values after
                the ones that matter (printer-info, document-format-supported)
    header      Get-Printer-Attributes with a 20 KiB header
    octet       lists application/octet-stream, image/urf and image/pwg-raster, not PDF (BUG-271's Brother); a
                Print-Job whose document-format is not application/octet-stream is refused (0x040A)
    raster      lists image/urf and image/pwg-raster only

FOLDER/ipp-<index>-<n>.pdf keeps each document and ipp-<index>-<n>.format its document-format; FOLDER/concurrency holds "most-at-once-in-all most-at-once-one-printer".
The LPD queue delays the answer to the data file by 2 s and records the subcommands it got in FOLDER/lpd-<n>.log
(an "abort" line for \\001).
"""
import socket
import socketserver
import struct
import sys
import threading
import time
from pathlib import Path

FOLDER = Path(sys.argv[1])
COUNT = int(sys.argv[2])
LOCK = threading.Lock()
STATE = {"now": 0, "most": 0, "per": {}, "per_most": 0, "jobs": {}, "lpd": 0}


def attribute(tag, name, value):
	data = value if isinstance(value, bytes) else value.encode()
	return bytes([tag]) + struct.pack(">H", len(name)) + name.encode() + struct.pack(">H", len(data)) + data


def parse(body):
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


def record(index, step):
	"""Counts the Print-Jobs being answered at once, in all and for one printer."""
	with LOCK:
		STATE["now"] += step
		STATE["per"][index] = STATE["per"].get(index, 0) + step
		STATE["most"] = max(STATE["most"], STATE["now"])
		STATE["per_most"] = max(STATE["per_most"], STATE["per"][index])
		(FOLDER / "concurrency").write_text(f"{STATE['most']} {STATE['per_most']}\n")


def make_ipp(index):
	class Ipp(socketserver.StreamRequestHandler):
		def handle(self):
			mode_file = FOLDER / f"mode-{index}"
			mode = mode_file.read_text().strip() if mode_file.exists() else "normal"
			line = self.rfile.readline()
			headers = {}
			while True:
				header = self.rfile.readline()
				if header in (b"\r\n", b"\n", b""):
					break
				key, _, value = header.decode().partition(":")
				headers[key.strip().lower()] = value.strip()
			body = self.rfile.read(int(headers.get("content-length", "0")))
			if not line.split()[1] == b"/ipp/print":
				self.wfile.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
				return
			operation, request, attributes, document = parse(body)
			groups = b"\x01" + attribute(0x47, "attributes-charset", "utf-8") + attribute(0x48, "attributes-natural-language", "en")
			status = 0
			chunked = False
			if operation == 0x000B and mode in ("octet", "raster"):
				groups += b"\x04" + attribute(0x41, "printer-info", f"Mock {index}")
				if mode == "octet":
					groups += attribute(0x49, "document-format-supported", "application/octet-stream") + attribute(0x49, "", "image/urf")
				else:
					groups += attribute(0x49, "document-format-supported", "image/urf")
				groups += attribute(0x49, "", "image/pwg-raster") + attribute(0x23, "printer-state", struct.pack(">I", 3))
			elif operation == 0x0002 and mode == "octet" and attributes.get("document-format", [b""])[0] != b"application/octet-stream":
				status = 0x040A
			elif operation == 0x000B:
				groups += b"\x04" + attribute(0x41, "printer-info", f"Mock {index}") + attribute(0x49, "document-format-supported", "application/pdf")
				groups += attribute(0x23, "printer-state", struct.pack(">I", 3))
				if mode == "big":
					for number in range(400):
						groups += attribute(0x41, f"x-junk-{number}", b"j" * 2000)
					chunked = True
			elif operation == 0x0002:
				if mode == "busy":
					status = 0x0507
				else:
					record(index, 1)
					if mode == "slow":
						time.sleep(2.0)
					with LOCK:
						STATE["jobs"][index] = STATE["jobs"].get(index, 0) + 1
						number = STATE["jobs"][index]
					(FOLDER / f"ipp-{index}-{number}.pdf").write_bytes(document)
					(FOLDER / f"ipp-{index}-{number}.format").write_bytes(attributes.get("document-format", [b""])[0])
					record(index, -1)
					groups += b"\x02" + attribute(0x21, "job-id", struct.pack(">I", 100 + number)) + attribute(0x23, "job-state", struct.pack(">I", 3))
			elif operation == 0x0009:
				groups += b"\x02" + attribute(0x23, "job-state", struct.pack(">I", 9))
			elif operation == 0x0008:
				pass
			answer = struct.pack(">BBHI", 2, 0, status, request) + groups + b"\x03"
			if mode == "header" and operation == 0x000B:
				self.wfile.write(b"HTTP/1.1 200 OK\r\n" + b"".join(b"X-Pad-%d: %s\r\n" % (n, b"p" * 900) for n in range(22)) +
					b"Content-Length: %d\r\n\r\n" % len(answer) + answer)
				return
			if chunked:
				out = b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\nTransfer-Encoding: chunked\r\n\r\n"
				for start in range(0, len(answer), 7000):
					piece = answer[start:start + 7000]
					out += b"%x\r\n" % len(piece) + piece + b"\r\n"
				out += b"0\r\n\r\n"
				self.wfile.write(out)
				return
			self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\nContent-Length: %d\r\n\r\n" % len(answer) + answer)
	return Ipp


class Lpd(socketserver.StreamRequestHandler):
	def handle(self):
		with LOCK:
			STATE["lpd"] += 1
			number = STATE["lpd"]
		log = FOLDER / f"lpd-{number}.log"
		line = self.rfile.readline()
		if not line.startswith(b"\x02"):
			return
		self.wfile.write(b"\0")
		with open(log, "a") as out:
			while True:
				line = self.rfile.readline()
				if not line:
					out.write("closed\n")
					return
				if line[0] == 1:
					out.write("abort\n")
					return
				kind = "data" if line[0] == 3 else "control"
				out.write(kind + "\n")
				out.flush()
				length = int(line[1:].split(b" ")[0])
				self.wfile.write(b"\0")
				data = self.rfile.read(length + 1)
				if len(data) < length + 1:
					out.write("closed\n")
					return
				out.write(kind + "-received\n")
				out.flush()
				time.sleep(2.0)
				self.wfile.write(b"\0")


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
	daemon_threads = True
	allow_reuse_address = True


def main():
	FOLDER.mkdir(parents=True, exist_ok=True)
	servers = [Server(("127.0.0.1", 0), make_ipp(index)) for index in range(COUNT)]
	lpd = Server(("127.0.0.1", 0), Lpd)
	for server in servers + [lpd]:
		threading.Thread(target=server.serve_forever, daemon=True).start()
	print("PORTS " + " ".join(str(server.server_address[1]) for server in servers) + f" {lpd.server_address[1]}", flush=True)
	while True:
		time.sleep(3600)


if __name__ == "__main__":
	main()

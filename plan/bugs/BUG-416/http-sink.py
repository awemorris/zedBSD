#!/usr/bin/env python3
"""A host HTTP server for qemu-usbnet.sh: answers every GET with a short body.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    http-sink.py PORT LOG

Listens on 127.0.0.1:PORT (the guest reaches it as 10.0.2.2:PORT through
QEMU's user network) and writes one line per request to LOG: the length of
the request head and its path's length.  The guest's request is what the
test sizes, so its frames are the guest's transmissions.
"""
from __future__ import annotations

import socketserver
import sys
import threading


class Handler(socketserver.BaseRequestHandler):
	"""Reads one request head and answers it."""

	def handle(self) -> None:
		data = b""
		self.request.settimeout(15)
		try:
			while b"\r\n\r\n" not in data:
				chunk = self.request.recv(65536)
				if not chunk:
					break
				data += chunk
		except OSError:
			pass
		line = data.split(b"\r\n", 1)[0].split(b" ")
		path = line[1] if len(line) > 1 else b""
		with LOCK:
			LOG.write(f"request {len(data)} path {len(path)}\n")
			LOG.flush()
		body = b"ok\n"
		try:
			self.request.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % len(body) + body)
		except OSError:
			pass


class Server(socketserver.ThreadingTCPServer):
	allow_reuse_address = True
	daemon_threads = True


LOCK = threading.Lock()
LOG = open(sys.argv[2], "a", encoding="ascii")
with Server(("127.0.0.1", int(sys.argv[1])), Handler) as server:
	server.serve_forever()

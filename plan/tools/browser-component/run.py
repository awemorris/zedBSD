#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Runs the public client against a bounded loopback-only HTTP failure fixture."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

parser = argparse.ArgumentParser()
parser.add_argument('client')
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]

class Handler(BaseHTTPRequestHandler):
    first_visits = 0

    def do_GET(self):
        name = self.path.lstrip('/')
        if name == 'first.html':
            Handler.first_visits += 1
            if Handler.first_visits > 1:
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                return
        if name not in ('first.html', 'second.html'):
            self.send_error(404)
            return
        payload = (root / 'plan/tools/browser-component/pages' / name).read_bytes()
        self.send_response(200)
        # first.html is not kept for Back (the view keeps left pages unless they are no-store): going back to it
        # loads it again over the network, which the client's async history checks need.
        if name == 'first.html':
            self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(payload)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass

server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
environment = os.environ.copy()
environment['VK_DRIVER_FILES'] = '/usr/share/vulkan/icd.d/lvp_icd.json'
try:
    result = subprocess.run([str(root / args.client),
                             f'http://127.0.0.1:{server.server_port}'],
                            cwd=root, env=environment, timeout=60)
finally:
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
raise SystemExit(result.returncode)

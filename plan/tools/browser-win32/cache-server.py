#!/usr/bin/python3 -I
# The HTTP server of the persistent cache part of storage-test.c, on 127.0.0.1.
#   cache-server.py PORT
# It answers, and counts the requests of each path (GET and POST alike):
#   /max-age      200, "max-age-body", Cache-Control: max-age=3600
#   /etag         200, "etag-body", ETag "v1", Cache-Control: max-age=1; a request with
#                 If-None-Match: "v1" gets 304 with Cache-Control: max-age=3600
#   /etag-change  200, "change-v1", ETag "w1", Cache-Control: max-age=1; a request with If-None-Match: "w1" gets
#                 200 with a new body, "change-v2", ETag "w2", max-age=3600 (the page changed, not a 304);
#                 one with If-None-Match: "w2" gets 304 with max-age=3600
#   /etag-gone    200, "gone-v1", ETag "g1", Cache-Control: max-age=1; a request with If-None-Match: "g1" gets
#                 200 "gone-v2" with Cache-Control: no-store (the page changed and may not be kept); one
#                 without If-None-Match after that gets 200 "gone-v3", no-store
#   /no-store     200, "no-store-body", Cache-Control: no-store, max-age=3600
#   /plain        200, "plain-body", nothing about caching
#   /post         200, "post-body", Cache-Control: max-age=3600 (for a POST)
#   /blob/N       200, 100000 bytes of the letter 'a' + N, Cache-Control: max-age=3600
#   /count?path=P 200, "hits=H conditional=C": the requests of P so far, C of them with If-None-Match
#   /quit         200, then the server stops
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

hits = {}
conditional = {}
lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        pass

    def reply(self, status, body, headers):
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def answer(self):
        parts = urlsplit(self.path)
        path = parts.path
        if self.command == "POST":
            length = int(self.headers.get("Content-Length", "0"))
            self.rfile.read(length)
        if path == "/count":
            target = parse_qs(parts.query).get("path", [""])[0]
            with lock:
                text = "hits=%d conditional=%d" % (hits.get(target, 0), conditional.get(target, 0))
            self.reply(200, text.encode(), [])
            return
        if path == "/quit":
            self.reply(200, b"bye", [("Connection", "close")])
            threading.Thread(target=self.server.shutdown).start()
            return
        with lock:
            hits[path] = hits.get(path, 0) + 1
            if "If-None-Match" in self.headers:
                conditional[path] = conditional.get(path, 0) + 1
        if path == "/max-age":
            self.reply(200, b"max-age-body", [("Cache-Control", "max-age=3600")])
        elif path == "/etag":
            if self.headers.get("If-None-Match") == '"v1"':
                self.reply(304, b"", [("ETag", '"v1"'), ("Cache-Control", "max-age=3600")])
            else:
                self.reply(200, b"etag-body", [("ETag", '"v1"'), ("Cache-Control", "max-age=1")])
        elif path == "/etag-change":
            tag = self.headers.get("If-None-Match")
            if tag == '"w1"':
                self.reply(200, b"change-v2", [("ETag", '"w2"'), ("Cache-Control", "max-age=3600")])
            elif tag == '"w2"':
                self.reply(304, b"", [("ETag", '"w2"'), ("Cache-Control", "max-age=3600")])
            else:
                self.reply(200, b"change-v1", [("ETag", '"w1"'), ("Cache-Control", "max-age=1")])
        elif path == "/etag-gone":
            tag = self.headers.get("If-None-Match")
            if tag == '"g1"':
                self.reply(200, b"gone-v2", [("Cache-Control", "no-store")])
            elif hits[path] > 1:
                self.reply(200, b"gone-v3", [("Cache-Control", "no-store")])
            else:
                self.reply(200, b"gone-v1", [("ETag", '"g1"'), ("Cache-Control", "max-age=1")])
        elif path == "/no-store":
            self.reply(200, b"no-store-body", [("Cache-Control", "no-store, max-age=3600")])
        elif path == "/plain":
            self.reply(200, b"plain-body", [])
        elif path == "/post":
            self.reply(200, b"post-body", [("Cache-Control", "max-age=3600")])
        elif path.startswith("/blob/"):
            letter = chr(ord("a") + int(path[len("/blob/"):]))
            self.reply(200, (letter * 100000).encode(), [("Cache-Control", "max-age=3600")])
        else:
            self.reply(404, b"not found", [])

    do_GET = answer
    do_POST = answer


server = ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler)
server.daemon_threads = True
server.serve_forever()

#!/usr/bin/env python3
# A loopback HTTP and HTTPS server for the host test of fetch (run-fetch-https.sh); the host's Python ssl module.
#   python3 -I https-server.py <port-file> <body-file> <tls cert> <tls key> [<more cert> <more key> <name>]...
# It listens on 127.0.0.1 with ports the kernel chooses and writes "<name> <port>" lines to <port-file> (name "plain"
# for HTTP, "tls" for the first certificate, then each extra one under its name, "tls12" for the first certificate
# limited to TLS 1.2).  Paths:
#   /file         the body with Content-Length            /chunked     the body in chunks of 1000 bytes
#   /eof-notify   the body up to the end, with close_notify /eof-cut   the body up to the end, no close_notify
#   /abs          302 to /file                              /dir/rel     302 to the relative "file2" (= /dir/file2)
#   /dir/file2    the body with Content-Length              /missing     404
#   /to-https     (plain) 302 to https://localhost:<tls port>/file
#   /loop         302 to /loop
# Each connection is served on its own thread; the server runs until killed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import socket
import ssl
import sys
import threading


def read_request(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return None
        data += chunk
        if len(data) > 65536:
            return None
    line = data.split(b"\r\n", 1)[0].decode("latin-1")
    parts = line.split(" ")
    if len(parts) != 3 or parts[0] != "GET":
        return None
    return parts[1]


def head(status, fields):
    text = "HTTP/1.1 %s\r\n" % status
    for name, value in fields:
        text += "%s: %s\r\n" % (name, value)
    return (text + "\r\n").encode("latin-1")


def serve(conn, body, ports, secure):
    path = read_request(conn)
    if path is None:
        return "close"
    if path in ("/file", "/dir/file2"):
        conn.sendall(head("200 OK", [("Content-Length", len(body)), ("Connection", "close")]) + body)
    elif path == "/chunked":
        conn.sendall(head("200 OK", [("Transfer-Encoding", "chunked"), ("Connection", "close")]))
        for start in range(0, len(body), 1000):
            piece = body[start:start + 1000]
            conn.sendall(b"%x;ext=1\r\n" % len(piece) + piece + b"\r\n")
        conn.sendall(b"0\r\nX-Trailer: done\r\n\r\n")
    elif path in ("/eof-notify", "/eof-cut"):
        conn.sendall(head("200 OK", [("Connection", "close")]) + body)
        if path == "/eof-cut":
            return "cut"
    elif path == "/abs":
        conn.sendall(head("302 Found", [("Location", "/file"), ("Content-Length", 0)]))
    elif path == "/dir/rel":
        conn.sendall(head("302 Found", [("Location", "file2"), ("Content-Length", 0)]))
    elif path == "/loop":
        conn.sendall(head("302 Found", [("Location", "/loop"), ("Content-Length", 0)]))
    elif path == "/to-https" and not secure:
        location = "https://localhost:%d/file" % ports["tls"]
        conn.sendall(head("301 Moved Permanently", [("Location", location), ("Content-Length", 0)]))
    else:
        conn.sendall(head("404 Not Found", [("Content-Length", 0)]))
    return "close"


def handle(raw, context, body, ports):
    conn = raw
    try:
        raw.settimeout(30)
        if context is not None:
            conn = context.wrap_socket(raw, server_side=True)
        how = serve(conn, body, ports, context is not None)
        if context is not None and how != "cut":
            try:
                conn = conn.unwrap()
            except (OSError, ssl.SSLError):
                pass
        conn.close()
    except (OSError, ssl.SSLError):
        try:
            raw.close()
        except OSError:
            pass


def listen(context, body, ports):
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", 0))
    server.listen(16)

    def loop():
        while True:
            raw, _ = server.accept()
            threading.Thread(target=handle, args=(raw, context, body, ports), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()
    return server.getsockname()[1]


def make_context(cert, key, tls12=False):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    if tls12:
        context.maximum_version = ssl.TLSVersion.TLSv1_2
    return context


def main():
    port_file, body_file, cert, key = sys.argv[1:5]
    extra = sys.argv[5:]
    with open(body_file, "rb") as f:
        body = f.read()
    ports = {}
    ports["tls"] = listen(make_context(cert, key), body, ports)
    ports["tls12"] = listen(make_context(cert, key, True), body, ports)
    ports["plain"] = listen(None, body, ports)
    for index in range(0, len(extra) - 2, 3):
        ports[extra[index + 2]] = listen(make_context(extra[index], extra[index + 1]), body, ports)
    with open(port_file + ".part", "w") as f:
        for name, port in ports.items():
            f.write("%s %d\n" % (name, port))
    import os
    os.rename(port_file + ".part", port_file)
    threading.Event().wait()


main()

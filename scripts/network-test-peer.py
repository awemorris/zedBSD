#!/usr/bin/env python3
"""Deterministic UDP echo and HTTP peer for the PC-98 network test."""
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

import argparse
import selectors
import socket
from pathlib import Path


def mark(path: Path, text: str) -> None:
    with path.open("a", encoding="ascii") as output:
        output.write(text + "\n")
        output.flush()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    args.log.write_text("PEER READY\n", encoding="ascii")

    selector = selectors.DefaultSelector()
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind(("0.0.0.0", 8081))
    udp.setblocking(False)
    selector.register(udp, selectors.EVENT_READ, "udp")

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", 8080))
    listener.listen(1)
    listener.setblocking(False)
    selector.register(listener, selectors.EVENT_READ, "listen")

    clients: set[socket.socket] = set()
    while True:
        for key, _ in selector.select(timeout=1.0):
            if key.data == "udp":
                data, address = udp.recvfrom(2048)
                udp.sendto(data, address)
                if data == b"zedBSD UDP echo":
                    mark(args.log, "UDP PASS")
            elif key.data == "listen":
                client, _ = listener.accept()
                client.setblocking(False)
                clients.add(client)
                selector.register(client, selectors.EVENT_READ, "http")
            else:
                client = key.fileobj
                data = client.recv(4096)
                if data.startswith(b"GET / HTTP/1.0\r\n"):
                    body = b"zedBSD network test\n"
                    response = (b"HTTP/1.0 200 OK\r\nContent-Length: " +
                                str(len(body)).encode("ascii") +
                                b"\r\nConnection: close\r\n\r\n" + body)
                    client.sendall(response)
                    mark(args.log, "TCP PASS")
                selector.unregister(client)
                clients.discard(client)
                client.close()


if __name__ == "__main__":
    raise SystemExit(main())

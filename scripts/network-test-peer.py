#!/usr/bin/env python3
"""Deterministic UDP echo and HTTP peer for the PC-98 network test."""
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

import argparse
import selectors
import socket
import struct
from pathlib import Path


def mark(path: Path, text: str) -> None:
    with path.open("a", encoding="ascii") as output:
        output.write(text + "\n")
        output.flush()


def dns_name(data: bytes, offset: int) -> tuple[str, int]:
    labels = []
    while offset < len(data) and data[offset]:
        length = data[offset]
        if length > 63 or offset + 1 + length > len(data):
            raise ValueError("bad DNS name")
        labels.append(data[offset + 1:offset + 1 + length].decode("ascii"))
        offset += length + 1
    if offset >= len(data):
        raise ValueError("unterminated DNS name")
    return ".".join(labels), offset + 1


def encoded_name(name: str) -> bytes:
    return b"".join(bytes((len(label),)) + label.encode("ascii")
                    for label in name.split(".")) + b"\0"


def dns_response(query: bytes, truncate: bool = False) -> bytes:
    if len(query) < 12:
        raise ValueError("short DNS query")
    name, end = dns_name(query, 12)
    if end + 4 > len(query):
        raise ValueError("short DNS question")
    qtype, qclass = struct.unpack_from("!HH", query, end)
    question = query[12:end + 4]
    flags = 0x8180
    answers = []
    if truncate:
        flags = 0x8380
    elif qclass == 1 and qtype == 1 and name == "zedbsd.test":
        answers.append(b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 300, 4) +
                       socket.inet_aton("192.0.2.123"))
    elif qclass == 1 and qtype == 1 and name in ("alias.zedbsd.test",
                                                  "truncated.zedbsd.test"):
        target = encoded_name("zedbsd.test")
        answers.append(b"\xc0\x0c" + struct.pack("!HHIH", 5, 1, 300,
                                                   len(target)) + target)
        answers.append(target + struct.pack("!HHIH", 1, 1, 300, 4) +
                       socket.inet_aton("192.0.2.123"))
    elif qclass == 1 and qtype == 12 and \
            name == "123.2.0.192.in-addr.arpa":
        target = encoded_name("zedbsd.test")
        answers.append(b"\xc0\x0c" + struct.pack("!HHIH", 12, 1, 300,
                                                   len(target)) + target)
    else:
        flags = 0x8183
    return (query[:2] + struct.pack("!HHHHH", flags, 1, len(answers), 0, 0) +
            question + b"".join(answers))


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

    dns_udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dns_udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    dns_udp.bind(("0.0.0.0", 5353))
    dns_udp.setblocking(False)
    selector.register(dns_udp, selectors.EVENT_READ, "dns_udp")

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", 8080))
    listener.listen(1)
    listener.setblocking(False)
    selector.register(listener, selectors.EVENT_READ, "listen")

    dns_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    dns_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    dns_listener.bind(("0.0.0.0", 5353))
    dns_listener.listen(4)
    dns_listener.setblocking(False)
    selector.register(dns_listener, selectors.EVENT_READ, "dns_listen")

    clients: set[socket.socket] = set()
    while True:
        for key, _ in selector.select(timeout=1.0):
            if key.data == "udp":
                data, address = udp.recvfrom(2048)
                udp.sendto(data, address)
                if data == b"zedBSD UDP echo":
                    mark(args.log, "UDP PASS")
            elif key.data == "dns_udp":
                data, address = dns_udp.recvfrom(2048)
                name, _ = dns_name(data, 12)
                dns_udp.sendto(dns_response(
                    data, name == "truncated.zedbsd.test"), address)
                mark(args.log, "DNS UDP PASS")
            elif key.data == "listen":
                client, _ = listener.accept()
                client.setblocking(False)
                clients.add(client)
                selector.register(client, selectors.EVENT_READ, "http")
            elif key.data == "dns_listen":
                client, _ = dns_listener.accept()
                client.setblocking(False)
                clients.add(client)
                selector.register(client, selectors.EVENT_READ, "dns_tcp")
            elif key.data == "dns_tcp":
                client = key.fileobj
                data = client.recv(4096)
                if len(data) >= 2:
                    length = struct.unpack_from("!H", data)[0]
                    if len(data) >= length + 2:
                        response = dns_response(data[2:2 + length])
                        client.sendall(struct.pack("!H", len(response)) + response)
                        mark(args.log, "DNS TCP PASS")
                selector.unregister(client)
                clients.discard(client)
                client.close()
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

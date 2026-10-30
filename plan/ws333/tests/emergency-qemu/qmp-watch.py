#!/usr/bin/env python3
# Watches one QEMU guest through QMP until it stops: captures the screen
# every INTERVAL seconds as OUT/PREFIX-NNN.png (QEMU writes a PPM, which is
# turned into a PNG once the guest stopped) and waits for the SHUTDOWN
# event.  Prints one line, "shutdown REASON SECONDS" or "timeout SECONDS",
# where SECONDS counts from the connection.  Reads no console or serial log.
#   qmp-watch.py SOCKET OUT PREFIX TIMEOUT INTERVAL
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import json
import os
import select
import socket
import struct
import sys
import time
import zlib


def ppm_to_png(source, destination):
    with open(source, "rb") as stream:
        data = stream.read()
    fields = []
    position = 0
    while len(fields) < 4:
        while data[position:position + 1].isspace():
            position += 1
        if data[position:position + 1] == b"#":
            position = data.index(b"\n", position) + 1
            continue
        end = position
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[position:end])
        position = end
    position += 1
    width, height = int(fields[1]), int(fields[2])
    pixels = data[position:position + width * height * 3]
    rows = b"".join(b"\x00" + pixels[row * width * 3:(row + 1) * width * 3] for row in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)

    with open(destination, "wb") as stream:
        stream.write(b"\x89PNG\r\n\x1a\n")
        stream.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        stream.write(chunk(b"IDAT", zlib.compress(rows, 6)))
        stream.write(chunk(b"IEND", b""))


def convert_all(out, prefix, count):
    for index in range(count):
        source = os.path.join(out, "%s-%03d.ppm" % (prefix, index))
        if os.path.exists(source):
            ppm_to_png(source, os.path.join(out, "%s-%03d.png" % (prefix, index)))


def main():
    path, out, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
    limit, interval = float(sys.argv[4]), float(sys.argv[5])
    deadline = time.monotonic() + 30.0
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    while True:
        try:
            sock.connect(path)
            break
        except OSError:
            if time.monotonic() > deadline:
                print("timeout 0 (no QMP socket)")
                return 1
            time.sleep(0.2)
    buffer = b""

    def send(message):
        sock.sendall((json.dumps(message) + "\n").encode())

    def lines(wait):
        nonlocal buffer
        ready, _, _ = select.select([sock], [], [], wait)
        if ready:
            data = sock.recv(65536)
            if not data:
                return None
            buffer += data
        found = []
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            if line.strip():
                found.append(json.loads(line))
        return found

    while True:
        greeting = lines(5.0)
        if greeting is None:
            print("closed 0")
            return 1
        if greeting:
            break
    send({"execute": "qmp_capabilities"})
    start = time.monotonic()
    next_shot = start
    shot = 0
    while True:
        now = time.monotonic()
        if now - start > limit:
            print("timeout %.0f" % (now - start))
            send({"execute": "quit"})
            convert_all(out, prefix, shot)
            return 1
        if now >= next_shot:
            name = os.path.join(out, "%s-%03d.ppm" % (prefix, shot))
            send({"execute": "screendump", "arguments": {"filename": name, "format": "ppm"}})
            shot += 1
            next_shot += interval
        messages = lines(1.0)
        if messages is None:
            print("closed %.0f" % (time.monotonic() - start))
            return 1
        for message in messages:
            if message.get("event") == "SHUTDOWN":
                reason = message.get("data", {}).get("reason", "?")
                print("shutdown %s %.0f" % (reason, time.monotonic() - start))
                convert_all(out, prefix, shot)
                return 0


sys.exit(main())

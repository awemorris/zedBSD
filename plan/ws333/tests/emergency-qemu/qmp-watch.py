#!/usr/bin/env python3
# Watches one QEMU guest through QMP until it stops: captures the screen
# every INTERVAL seconds as OUT/PREFIX-NNN.png and waits for the SHUTDOWN
# event.  Prints one line, "shutdown REASON SECONDS" or "timeout SECONDS",
# where SECONDS counts from the connection.  Reads no console or serial log.
#   qmp-watch.py SOCKET OUT PREFIX TIMEOUT INTERVAL
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import json
import os
import socket
import sys
import time


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
    stream = sock.makefile("rw")
    json.loads(stream.readline())
    stream.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
    stream.flush()
    start = time.monotonic()
    next_shot = start
    shot = 0
    sock.settimeout(1.0)
    while True:
        now = time.monotonic()
        if now - start > limit:
            print("timeout %.0f" % (now - start))
            stream.write(json.dumps({"execute": "quit"}) + "\n")
            stream.flush()
            return 1
        if now >= next_shot:
            name = os.path.join(out, "%s-%03d.png" % (prefix, shot))
            stream.write(json.dumps({"execute": "screendump",
                                     "arguments": {"filename": name, "format": "png"}}) + "\n")
            stream.flush()
            shot += 1
            next_shot += interval
        try:
            line = stream.readline()
        except (socket.timeout, TimeoutError):
            continue
        if not line:
            print("closed %.0f" % (time.monotonic() - start))
            return 1
        message = json.loads(line)
        if message.get("event") == "SHUTDOWN":
            reason = message.get("data", {}).get("reason", "?")
            print("shutdown %s %.0f" % (reason, time.monotonic() - start))
            return 0


sys.exit(main())

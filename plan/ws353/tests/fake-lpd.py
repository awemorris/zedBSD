#!/usr/bin/python3
# A stand-in LPD printer (RFC 1179 "receive a printer job") for qemu-printers.sh, on the host: it answers every command
# and subcommand with a zero byte, takes the data and control files whole, and logs one line per job to its log
# ("job queue=Q data=N control=M").  It listens on 127.0.0.1:PORT (QEMU's user network reaches it as 10.0.2.2:PORT)
# until it is killed.
#   /usr/bin/python3 -I plan/ws353/tests/fake-lpd.py PORT LOG
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import socket
import sys


def read_line(stream):
    line = b""
    while not line.endswith(b"\n"):
        byte = stream.read(1)
        if not byte:
            return None
        line += byte
    return line


def read_exact(stream, count):
    data = b""
    while len(data) < count:
        part = stream.read(count - len(data))
        if not part:
            return None
        data += part
    return data


def serve(connection, log):
    stream = connection.makefile("rb")
    first = read_line(stream)
    if first is None or first[:1] != b"\x02":
        return
    queue = first[1:-1].decode("ascii", "replace")
    connection.sendall(b"\0")
    sizes = {}
    while True:
        command = read_line(stream)
        if command is None:
            break
        if command[:1] == b"\x01":
            log.write("abort queue=%s\n" % queue)
            log.flush()
            return
        kind = {b"\x02": "control", b"\x03": "data"}.get(command[:1])
        if kind is None:
            return
        size = int(command[1:].split(b" ", 1)[0])
        connection.sendall(b"\0")
        body = read_exact(stream, size + 1)
        if body is None:
            break
        sizes[kind] = size
        connection.sendall(b"\0")
    log.write("job queue=%s data=%d control=%d\n" % (queue, sizes.get("data", -1), sizes.get("control", -1)))
    log.flush()


def main():
    port = int(sys.argv[1])
    log = open(sys.argv[2], "a")
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(4)
    log.write("listening port=%d\n" % port)
    log.flush()
    while True:
        connection, _ = listener.accept()
        connection.settimeout(60)
        try:
            serve(connection, log)
        except (OSError, ValueError) as error:
            log.write("error %s\n" % error)
            log.flush()
        finally:
            connection.close()


if __name__ == "__main__":
    main()

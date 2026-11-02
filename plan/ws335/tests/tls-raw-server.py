#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""A one-connection TLS server that answers with a hand-made message, for the cases OpenSSL's s_server cannot make.

  tls-raw-server.py SCRIPT PORT-FILE      listens on 127.0.0.1 (a port the kernel chooses, written to PORT-FILE),
                                          takes one connection, reads the ClientHello, sends what SCRIPT names and
                                          prints the record the client answers with ("alert <level> <description>",
                                          "record <type> <length>" or "eof"), then ends

SCRIPT:
  no-key-share    a TLS 1.3 ServerHello (supported_versions 0x0304, TLS_AES_128_GCM_SHA256, the client's session
                  id echoed) without key_share: RFC 8446 4.2.8 makes the client answer missing_extension (109)

The bytes are written here from RFC 8446 4.1.3 and 4.2.1; nothing of the library is used.  It ends after 30 seconds
at most.
"""

import socket
import struct
import sys


def read_exact(connection, count):
    data = b""
    while len(data) < count:
        piece = connection.recv(count - len(data))
        if not piece:
            return None
        data += piece
    return data


def read_record(connection):
    header = read_exact(connection, 5)
    if header is None:
        return None, None
    kind, _version, length = struct.unpack("!BHH", header)
    body = read_exact(connection, length)
    return kind, body


def server_hello_without_key_share(session_id):
    extensions = struct.pack("!HHH", 43, 2, 0x0304)
    body = struct.pack("!H", 0x0303) + bytes(range(0x20, 0x40))
    body += struct.pack("!B", len(session_id)) + session_id
    body += struct.pack("!HB", 0x1301, 0) + struct.pack("!H", len(extensions)) + extensions
    message = struct.pack("!B", 2) + struct.pack("!I", len(body))[1:] + body
    return struct.pack("!BHH", 22, 0x0303, len(message)) + message


def main():
    if len(sys.argv) != 3 or sys.argv[1] != "no-key-share":
        print(__doc__)
        return 2
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(30)
    with open(sys.argv[2], "w") as port_file:
        port_file.write("%d\n" % listener.getsockname()[1])
    connection, _ = listener.accept()
    connection.settimeout(30)
    kind, body = read_record(connection)
    if kind != 22 or body is None or len(body) < 4 + 2 + 32 + 1 or body[0] != 1:
        print("no ClientHello")
        return 1
    session_length = body[4 + 2 + 32]
    session_id = body[4 + 2 + 32 + 1:4 + 2 + 32 + 1 + session_length]
    connection.sendall(server_hello_without_key_share(session_id))
    kind, body = read_record(connection)
    if kind is None:
        print("eof")
    elif kind == 21 and len(body) == 2:
        print("alert %d %d" % (body[0], body[1]))
    else:
        print("record %d %d" % (kind, len(body)))
    connection.close()
    listener.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

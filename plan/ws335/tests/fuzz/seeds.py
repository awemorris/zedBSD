#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Writes the seed inputs of the fuzz targets (run-fuzz.sh).

  seeds.py LIMBO-VEC OUT       OUT/<target>/seed-NNN for every target

The certificates are the first ones of x509-limbo's SERVER cases (the record file vectors.py made); the handshake
messages and records are written here from the syntax of RFC 8446 4 and RFC 5246 7.4 (well-formed examples for the
fuzzer to start from, not expected values).
"""

import os
import struct
import sys


def certificates(path, count):
    found = []
    with open(path) as stream:
        for line in stream:
            key, _, value = line.partition(" = ")
            key = key.strip()
            if key == "peer" or key.startswith("trusted.") or key.startswith("untrusted."):
                try:
                    der = bytes.fromhex(value.strip())
                except ValueError:
                    continue
                if 0 < len(der) < 8192 and der not in found:
                    found.append(der)
            if len(found) >= count:
                break
    return found


def vec8(data):
    return struct.pack("!B", len(data)) + data


def vec16(data):
    return struct.pack("!H", len(data)) + data


def vec24(data):
    return struct.pack("!I", len(data))[1:] + data


def ext(kind, data):
    return struct.pack("!H", kind) + vec16(data)


def handshake(kind, body):
    return struct.pack("!B", kind) + vec24(body)


def record(kind, body):
    return struct.pack("!BH", kind, 0x0303) + vec16(body)


def write(out, target, seeds):
    directory = os.path.join(out, target)
    os.makedirs(directory, exist_ok=True)
    for index, seed in enumerate(seeds):
        with open(os.path.join(directory, "seed-%03d" % index), "wb") as stream:
            stream.write(seed)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    certs = certificates(sys.argv[1], 64)
    out = sys.argv[2]
    random = bytes(range(32))
    session = bytes(range(32, 64))
    hello13 = struct.pack("!H", 0x0303) + random + vec8(session) + struct.pack("!HB", 0x1301, 0) + vec16(
        ext(43, struct.pack("!H", 0x0304)) + ext(51, struct.pack("!H", 0x001d) + vec16(bytes(range(1, 33)))))
    retry = struct.pack("!H", 0x0303) + bytes.fromhex(
        "cf21ad74e59a6111be1d8c021e65b891c2a211167abb8c5e079e09e2c8a8339c") + vec8(session) + struct.pack(
        "!HB", 0x1301, 0) + vec16(ext(43, struct.pack("!H", 0x0304)) + ext(51, struct.pack("!H", 0x0017)) +
                               ext(44, vec16(b"cookie")))
    hello12 = struct.pack("!H", 0x0303) + random + vec8(b"") + struct.pack("!HB", 0xc02b, 0) + vec16(
        ext(0xff01, b"\x00") + ext(23, b"") + ext(11, b"\x01\x00"))
    cert13 = vec8(b"") + vec24(b"".join(vec24(c) + vec16(b"") for c in certs[:3]))
    cert12 = vec24(b"".join(vec24(c) for c in certs[:3]))
    ske = struct.pack("!BH", 3, 0x001d) + vec8(bytes(range(32))) + struct.pack("!H", 0x0403) + vec16(b"\x30\x06\x02\x01\x01\x02\x01\x01")
    ske384 = struct.pack("!BH", 3, 0x0018) + vec8(b"\x04" + bytes(96)) + struct.pack("!H", 0x0503) + vec16(b"\x30\x00")
    write(out, "der", certs)
    write(out, "x509", certs)
    write(out, "pem", [b"-----BEGIN CERTIFICATE-----\n" + __import__("base64").encodebytes(c) + b"-----END CERTIFICATE-----\n" for c in certs[:8]])
    write(out, "server-hello", [hello13, retry, hello12])
    write(out, "encrypted-extensions", [vec16(b""), vec16(ext(0, b"")), vec16(ext(10, vec16(b"\x00\x1d\x00\x17")))])
    write(out, "certificate", [cert13, cert12])
    write(out, "server-key-exchange", [ske, ske384])
    write(out, "msg-other", [struct.pack("!H", 0x0403) + vec16(b"\x30\x00"), vec8(b"") + vec16(ext(13, vec16(b"\x04\x03"))),
                             struct.pack("!IIB", 7200, 1, 1) + b"\x00" + vec16(b"ticket") + vec16(b""), b"\x01"])
    write(out, "record", [b"\x00" + record(22, handshake(2, hello12)), b"\x01" + b"hello\x17", b"\x01" + b"\x16" + bytes(16),
                          b"\x02" + record(23, bytes(40)), b"\x03\x17" + b"application data"])
    write(out, "conn", [record(22, handshake(2, hello12) + handshake(11, cert12)), record(22, handshake(2, hello13)),
                        record(22, handshake(2, retry)), record(21, b"\x02\x28")])
    write(out, "mlkem", [bytes(1088), bytes(1184)])
    return 0


if __name__ == "__main__":
    sys.exit(main())

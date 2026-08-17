#!/usr/bin/env python3
"""Wait for an ASCII marker in the PC-98 text VRAM through QMP."""
import argparse
import json
from pathlib import Path
import socket
import time


def qmp_command(stream, name, arguments=None):
    request = {"execute": name}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write((json.dumps(request) + "\n").encode())
    stream.flush()
    while True:
        response = json.loads(stream.readline())
        if "return" in response:
            return response["return"]
        if "error" in response:
            raise RuntimeError(str(response["error"]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qmp", required=True)
    parser.add_argument("--dump", required=True)
    parser.add_argument("--marker", required=True)
    parser.add_argument("--timeout-ms", type=int, default=60000)
    args = parser.parse_args()
    deadline = time.monotonic() + args.timeout_ms / 1000.0
    qmp_path = Path(args.qmp)
    dump_path = Path(args.dump)
    connection = None
    while time.monotonic() < deadline:
        try:
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.connect(str(qmp_path))
            break
        except OSError:
            if connection is not None:
                connection.close()
            connection = None
            time.sleep(0.05)
    if connection is None:
        raise SystemExit("PC-98 QMP socket did not appear")
    stream = connection.makefile("rwb", buffering=0)
    json.loads(stream.readline())
    qmp_command(stream, "qmp_capabilities")
    marker = args.marker.encode("ascii")
    try:
        while time.monotonic() < deadline:
            response = qmp_command(stream, "human-monitor-command", {
                "command-line": 'pmemsave 0xa0000 0x2000 "' +
                str(dump_path) + '"'
            })
            if not dump_path.exists():
                if response:
                    print("PC-98 pmemsave: " + str(response))
                time.sleep(0.1)
                continue
            data = dump_path.read_bytes()
            # PC-98 text VRAM stores one little-endian 16-bit code per cell.
            text = bytes(data[index] for index in range(0, len(data), 2))
            if marker in text:
                return
            time.sleep(0.1)
    finally:
        stream.close()
        connection.close()
    raise SystemExit("PC-98 screen marker timed out: " + args.marker)


if __name__ == "__main__":
    main()

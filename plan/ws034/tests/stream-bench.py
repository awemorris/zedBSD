#!/usr/bin/env python3
"""ws034-p054: guest pipe/device throughput, smallest of three runs.

  stream-bench.py SERIAL_SOCKET    (guest logged in, e.g. by boot-hda.sh)
"""
import re, subprocess, sys

CASES = [
    ("cat-cat", "cat /tmp/f | cat > /dev/null"),
    ("cat-dd4k", "cat /tmp/f | dd of=/dev/null bs=4096 2>/dev/null"),
    ("dd4k-dd4k", "dd if=/tmp/f bs=4096 2>/dev/null | dd of=/dev/null bs=4096 2>/dev/null"),
    ("dd64k-dd64k", "dd if=/tmp/f bs=65536 2>/dev/null | dd of=/dev/null bs=65536 2>/dev/null"),
    ("cat-file", "cat /tmp/f > /dev/null"),
    ("zero-1m", "dd if=/dev/zero of=/dev/null bs=1048576 count=16 2>/dev/null"),
]

def run(socket, command):
    return subprocess.run(["python3", "plan/tools/guest/serial.py", "--socket", socket,
                           "--timeout", "300", "run", command],
                          capture_output=True, text=True).stdout

def main():
    socket = sys.argv[1]
    run(socket, "dd if=/dev/zero of=/tmp/f bs=512 count=32768 2>/dev/null; cat /tmp/f > /dev/null")
    for name, command in CASES:
        times = []
        for _ in range(3):
            match = re.search(r"real ([0-9.]+)", run(socket, 'time sh -c "%s"' % command.replace('"', '\\"')))
            if match:
                times.append(float(match.group(1)))
        print(name, min(times) if times else "none", flush=True)

main()

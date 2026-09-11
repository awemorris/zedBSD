#!/usr/bin/env python3
"""PTY-only protocol fixture. Never opens block devices or modifies media."""
import sys

mode = sys.argv[1]
if mode == "missing":
    sys.exit(0)
if mode == "overflow":
    sys.stdout.write("x" * 70000)
    sys.exit(1)
registration = 3 if mode == "stale" else 2
print(f"Target /dev/nvme0n1 registration={registration} sector-size=512 sectors=524288")
prompt = "Type 'ERASE nvme0n1:2' to initialize: "
print(prompt, end="", flush=True)
if mode == "stale":
    sys.exit(1)
assert sys.stdin.readline().rstrip("\r\n") == "ERASE nvme0n1:2"
if mode == "repeated":
    print(prompt, end="", flush=True)
sys.exit(1 if mode == "nonzero" else 0)

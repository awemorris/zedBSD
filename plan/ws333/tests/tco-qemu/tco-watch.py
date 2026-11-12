#!/usr/bin/env python3
# Watches one q35 guest's ICH9 TCO watchdog through QMP for SECONDS seconds: notes the WATCHDOG and SHUTDOWN events
# with their time from the connection, and every PROBE seconds reads, through the human monitor, the TCO's count
# (TCOBASE+0), its first control (TCOBASE+8, the halt in bit 11) and the reset's lock in the chipset configuration
# (RCBA+3410h, NO_REBOOT in bit 5).  TCOBASE is the ACPI I/O base the firmware gave the LPC bridge (00:1f.0,
# offset 40h) plus 60h, read once the guest has run for PROBE seconds; RCBA is the bridge's offset F0h.
# Prints one line per probe ("probe T count=C control=X general=Y") and per event ("event T NAME DATA"), then
# "end T REASON" (watchdog, shutdown or timeout).  Reads no console or serial log.
#   tco-watch.py SOCKET SECONDS PROBE
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import json
import re
import select
import socket
import sys
import time


class Monitor:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(path)
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise SystemExit("tco-watch: no QMP socket")
        self.buffer = b""
        self.events = []
        self.read_message()
        self.command("qmp_capabilities")

    def read_message(self, timeout=None):
        while b"\n" not in self.buffer:
            ready, _, _ = select.select([self.sock], [], [], timeout)
            if not ready:
                return None
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def command(self, name, arguments=None):
        request = {"execute": name}
        if arguments is not None:
            request["arguments"] = arguments
        self.sock.sendall((json.dumps(request) + "\n").encode())
        while True:
            message = self.read_message(30)
            if message is None:
                raise SystemExit("tco-watch: no reply to " + name)
            if "event" in message:
                self.events.append(message)
                continue
            return message.get("return")

    def hmp(self, line):
        return self.command("human-monitor-command", {"command-line": line}) or ""


def number(text):
    found = re.findall(r"0x[0-9a-fA-F]+|[0-9a-fA-F]+$", text.strip().splitlines()[-1] if text.strip() else "")
    return int(found[-1], 16) if found else -1


def pci_config(monitor, offset):
    # The LPC bridge's configuration through the legacy mechanism #1 ports, the guest's address put back after.
    saved = number(monitor.hmp("i /w 0xcf8"))
    monitor.hmp("o /w 0xcf8 0x%x" % (0x80000000 | (31 << 11) | (offset & 0xfc)))
    value = number(monitor.hmp("i /w 0xcfc"))
    monitor.hmp("o /w 0xcf8 0x%x" % (saved & 0xffffffff))
    return value


def main():
    path, seconds, probe = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    monitor = Monitor(path)
    start = time.time()
    next_probe = start + probe
    base = None
    reason = "timeout"
    try:
        while True:
            now = time.time()
            if now - start >= seconds:
                break
            for message in monitor.events:
                if message["event"] in ("STOP", "RESUME"):
                    continue
                print("event %.1f %s %s" % (now - start, message["event"], json.dumps(message.get("data", {}))), flush=True)
                if message["event"] == "WATCHDOG":
                    reason = "watchdog"
                if message["event"] == "SHUTDOWN" and reason == "timeout":
                    reason = "shutdown"
            monitor.events = []
            if reason != "timeout":
                break
            if now >= next_probe:
                next_probe += probe
                monitor.command("stop")
                if base is None:
                    base = (pci_config(monitor, 0x40) & 0xff80) + 0x60
                    rcba = pci_config(monitor, 0xf0) & 0xffffc000
                count = number(monitor.hmp("i /h 0x%x" % base)) & 0x3ff
                control = number(monitor.hmp("i /h 0x%x" % (base + 8)))
                general = number(monitor.hmp("xp /1wx 0x%x" % (rcba + 0x3410)))
                monitor.command("cont")
                print("probe %.1f base=0x%x count=%d control=0x%x general=0x%x" % (now - start, base, count, control, general), flush=True)
            message = monitor.read_message(0.5)
            if message is not None and "event" in message:
                monitor.events.append(message)
    except EOFError:
        if reason == "timeout":
            reason = "shutdown"
    print("end %.1f %s" % (time.time() - start, reason), flush=True)


main()

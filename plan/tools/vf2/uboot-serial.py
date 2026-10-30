#!/usr/bin/env python3
# Drives the VisionFive 2's U-Boot over the serial line: power-cycles the board through its SwitchBot plug, stops the
# autoboot, types each command of a file at the U-Boot prompt (waiting for the prompt after each), then keeps the line's
# output for SECONDS more (the kernel the last command started).  Everything the line printed goes to OUT.
#   python3 -I plan/tools/vf2/uboot-serial.py PLUG COMMANDS OUT SECONDS [DEVICE]
# A command line starting with "#" is skipped.  The plug's power-cycle uses plan/tools/switchbot.py.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import os
import subprocess
import sys
import termios
import time

PROMPTS = (b"StarFive #", b"=> ")


def open_line(device):
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


class Line:
    def __init__(self, fd, log):
        self.fd = fd
        self.log = log
        self.buffer = b""

    def pump(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                data = b""
            if data:
                self.buffer += data
                self.log.write(data)
                self.log.flush()
            else:
                time.sleep(0.02)

    def wait_for(self, needles, seconds):
        start = len(self.buffer)
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.pump(0.1)
            tail = self.buffer[start:]
            for needle in needles:
                if needle in tail:
                    return True
        return False

    def send(self, text):
        for byte in text.encode():
            os.write(self.fd, bytes([byte]))
            time.sleep(0.005)


def main():
    if len(sys.argv) < 5:
        print("usage: uboot-serial.py PLUG COMMANDS OUT SECONDS [DEVICE]", file=sys.stderr)
        return 2
    plug, commands, out, seconds = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
    device = sys.argv[5] if len(sys.argv) > 5 else "/dev/ttyUSB0"
    with open(commands) as source:
        lines = [line.rstrip("\n") for line in source if line.strip() and not line.startswith("#")]
    fd = open_line(device)
    with open(out, "wb") as log:
        line = Line(fd, log)
        subprocess.run([sys.executable, "-I", "plan/tools/switchbot.py", "cycle", plug, "10"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
        if not line.wait_for((b"Hit any key", b"autoboot"), 90):
            print("no autoboot prompt")
            return 1
        line.send("\r")
        line.pump(0.3)
        line.send("\r")
        if not line.wait_for(PROMPTS, 10):
            print("no U-Boot prompt")
            return 1
        for command in lines:
            line.send(command + "\r")
            if command.startswith("bootefi") or command.startswith("boot"):
                break
            if not line.wait_for(PROMPTS, 120):
                print("no prompt after: " + command)
                return 1
        line.pump(seconds)
    os.close(fd)
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())

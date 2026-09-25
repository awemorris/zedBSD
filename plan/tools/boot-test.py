#!/usr/bin/env python3
"""Watch a booting guest's screen and photograph the login prompt.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

The console draws to the framebuffer, so the screen is what a person at the
machine reads.  This connects to QMP, asks the emulator for the screen, and
reads the characters back out of the picture by matching each 8x16 cell against
the console font the kernel itself uses (src/drivers/platform/pcat/graphics/
vgafont.c).  That is exact: a cell either is a glyph of that font or it is not.
The picture is saved as a PNG whether or not the prompt appears, so a failure
can be looked at.
"""
from __future__ import annotations

import argparse
import json
import re
import socket
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

GLYPH_WIDTH = 8
GLYPH_HEIGHT = 16
FONT_GLYPHS = 256


def load_font(source: Path) -> dict[bytes, str]:
    """Reads the console font and returns a bitmap-to-character table."""
    text = source.read_text(encoding="utf-8", errors="replace")
    body = text[text.index("drv_pcat_vgafont16"):]
    body = body[body.index("{") + 1: body.index("};")]
    values = [int(value, 0) for value in re.findall(r"0x[0-9a-fA-F]+", body)]

    # Rejects a font that is not the expected 256 glyphs of 16 rows.
    if len(values) != FONT_GLYPHS * GLYPH_HEIGHT:
        raise SystemExit(f"boot-test: unexpected font size: {len(values)}")

    table: dict[bytes, str] = {}
    # Space is read first so that it owns the empty bitmap: the control codes
    # draw nothing either, and an unreadable screen full of NUL is of no use
    # to whoever has to look at the saved text.
    for code in [0x20] + [c for c in range(FONT_GLYPHS) if c != 0x20]:
        rows = bytes(values[code * GLYPH_HEIGHT:(code + 1) * GLYPH_HEIGHT])
        # Keeps the first character owning a bitmap: space wins over the other
        # blank cells, and a duplicate drawing cannot shadow it.
        if rows not in table:
            table[rows] = chr(code)
    return table


class Monitor:
    """One QMP session."""

    def __init__(self, path: str, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.socket.connect(path)
                break
            except OSError:
                # Waits for the emulator to create its socket.
                if time.monotonic() >= deadline:
                    raise SystemExit("boot-test: no QMP socket")
                time.sleep(0.2)
        self.socket.settimeout(30.0)
        self.buffer = b""
        self.read_object()
        self.command("qmp_capabilities")

    def read_object(self) -> dict:
        """Reads one JSON object from the monitor."""
        while True:
            newline = self.buffer.find(b"\n")
            if newline >= 0:
                line = self.buffer[:newline]
                self.buffer = self.buffer[newline + 1:]
                if line.strip():
                    return json.loads(line)
                continue
            chunk = self.socket.recv(65536)

            # Reports a monitor that closed before answering.
            if not chunk:
                raise SystemExit("boot-test: QMP closed")
            self.buffer += chunk

    def command(self, name: str, **arguments) -> dict:
        """Sends one command and returns its answer."""
        request = {"execute": name}
        if arguments:
            request["arguments"] = arguments
        self.socket.sendall(json.dumps(request).encode() + b"\n")
        while True:
            answer = self.read_object()
            if "return" in answer or "error" in answer:
                return answer

    def screendump(self, path: Path) -> None:
        """Writes the screen to a file in PPM form."""
        answer = self.command("screendump", filename=str(path), format="ppm")

        # Reports a screen the emulator refused to write.
        if "error" in answer:
            raise SystemExit(f"boot-test: screendump: {answer['error']}")


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    """Reads a binary PPM and returns its width, height and pixels."""
    data = path.read_bytes()
    fields: list[bytes] = []
    offset = 0
    while len(fields) < 4:
        while offset < len(data) and data[offset:offset + 1].isspace():
            offset += 1

        # Skips a comment line.
        if data[offset:offset + 1] == b"#":
            offset = data.index(b"\n", offset) + 1
            continue
        start = offset
        while offset < len(data) and not data[offset:offset + 1].isspace():
            offset += 1
        fields.append(data[start:offset])
    width = int(fields[1])
    height = int(fields[2])
    return width, height, data[offset + 1:]


def read_cells(width: int, height: int, pixels: bytes, pitch: int,
               font: dict[bytes, str], left: int = 0,
               top: int = 0) -> list[str]:
    """Reads the characters out of a picture drawn in cells `pitch` wide,
    with the grid starting `left` and `top` pixels into the picture."""
    rows: list[str] = []
    for cell_row in range((height - top) // GLYPH_HEIGHT):
        line = []
        for cell_column in range((width - left) // pitch):
            bitmap = bytearray()
            for row in range(GLYPH_HEIGHT):
                y = top + cell_row * GLYPH_HEIGHT + row
                bits = 0
                for column in range(GLYPH_WIDTH):
                    x = left + cell_column * pitch + column
                    offset = (y * width + x) * 3
                    red, green, blue = pixels[offset:offset + 3]

                    # Treats a lit pixel as part of the glyph.
                    if red + green + blue >= 128 * 3:
                        bits |= 0x80 >> column
                bitmap.append(bits)
            line.append(font.get(bytes(bitmap), "�"))
        rows.append("".join(line).rstrip())
    return rows


def read_text(path: Path, font: dict[bytes, str]) -> list[str]:
    """Reads the console text out of a screen picture.

    A kernel drawing into a linear framebuffer puts its glyphs side by side,
    eight pixels apart.  A kernel using the VGA text buffer instead leaves the
    drawing to the adapter, which spaces the same 8x16 glyphs nine pixels apart
    and fills the ninth column itself; that is what a BIOS-booted i386 guest
    shows.  Both are read here by trying each spacing and keeping whichever
    recognises more of the screen, so one tool reads either console.

    A console that centres an 80x25 grid on a larger screen (the Raspberry Pi
    4 console on its 640x480 framebuffer) starts the grid away from the
    corner, so the centred origin is tried as well as the corner.
    """
    width, height, pixels = read_ppm(path)
    origins = [(0, 0)]
    centred = (max(0, (width - 80 * GLYPH_WIDTH) // 2),
               max(0, (height - 25 * GLYPH_HEIGHT) // 2))
    if centred != (0, 0):
        origins.append(centred)
    best: list[str] = []
    best_unknown = -1
    for left, top in origins:
        for pitch in (GLYPH_WIDTH, GLYPH_WIDTH + 1):
            rows = read_cells(width, height, pixels, pitch, font, left, top)
            unknown = sum(line.count("�") for line in rows)
            if best_unknown < 0 or unknown < best_unknown:
                best = rows
                best_unknown = unknown
    return best


def write_png(source: Path, destination: Path) -> None:
    """Converts a PPM screen to a PNG."""
    width, height, pixels = read_ppm(source)
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += pixels[y * width * 3:(y + 1) * width * 3]

    def chunk(kind: bytes, payload: bytes) -> bytes:
        """Frames one PNG chunk."""
        head = struct.pack(">I", len(payload)) + kind
        return head + payload + struct.pack(">I", zlib.crc32(kind + payload))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    destination.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b""))


def main() -> int:
    """Runs the boot watch."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--monitor", required=True)
    parser.add_argument("--screenshot", required=True)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--pattern", default=r"^(?:\S+ )?login: ?")
    arguments = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    font = load_font(
        root / "src/drivers/platform/pcat/graphics/vgafont.c")
    prompt = re.compile(arguments.pattern)
    screenshot = Path(arguments.screenshot)
    frame = screenshot.with_suffix(".ppm")
    monitor = Monitor(arguments.monitor, timeout=30.0)

    deadline = time.monotonic() + arguments.timeout
    last: list[str] = []
    while time.monotonic() < deadline:
        monitor.screendump(frame)
        last = read_text(frame, font)

        # Stops as soon as the prompt is on the screen.  The pattern is
        # anchored at the start of a line, so a path inside a message --
        # "getty: /bin/login: Connection timed out" -- is not read as the
        # prompt.  The rest of the line is free: a daemon writing while the
        # prompt is up lands on the same line.
        if any(prompt.search(line) for line in last):
            write_png(frame, screenshot)
            frame.unlink(missing_ok=True)
            screenshot.with_suffix(".txt").write_text(
                "\n".join(last) + "\n", encoding="utf-8")
            print(f"boot-test: saw {arguments.pattern!r}")
            return 0
        time.sleep(2.0)

    # Keeps the last screen so a failure can be looked at.
    if frame.exists():
        write_png(frame, screenshot)
        frame.unlink(missing_ok=True)
        screenshot.with_suffix(".txt").write_text(
            "\n".join(last) + "\n", encoding="utf-8")
    print(f"boot-test: never saw {arguments.pattern!r}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())

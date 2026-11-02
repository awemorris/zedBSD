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

Several patterns may be asked for; each must be seen on some screen, not all
on one, because a boot that writes more than a screenful scrolls the first of
them away.  The first screen showing each one is kept beside the screenshot.
Steps given after them type on the guest's serial line and wait for the screen
again; only the screen decides, and what the guest writes to the serial line
is read away and dropped.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading
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


# The QEMU key names (qcodes) of the characters other than letters and digits
# that a keys step may type, on a US layout.
KEY_NAMES = {
    " ": ["spc"], "-": ["minus"], "_": ["shift", "minus"], "/": ["slash"],
    ".": ["dot"], ",": ["comma"], "=": ["equal"], ":": ["shift", "semicolon"],
    ";": ["semicolon"], "'": ["apostrophe"], "\"": ["shift", "apostrophe"],
    "|": ["shift", "backslash"], ">": ["shift", "dot"], "<": ["shift", "comma"],
    "~": ["shift", "grave_accent"], "$": ["shift", "4"], "*": ["shift", "8"],
    "&": ["shift", "7"], "?": ["shift", "slash"], "!": ["shift", "1"],
    "@": ["shift", "2"], "#": ["shift", "3"], "%": ["shift", "5"],
    "^": ["shift", "6"], "(": ["shift", "9"], ")": ["shift", "0"],
    "+": ["shift", "equal"], "[": ["bracket_left"], "]": ["bracket_right"],
    "{": ["shift", "bracket_left"], "}": ["shift", "bracket_right"],
    "\\": ["backslash"], "`": ["grave_accent"],
}


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

    def send_keys(self, text: str) -> None:
        """Presses each character's key and then return on the keyboard."""
        for character in text:
            names = KEY_NAMES.get(character)
            if names is None and character.isalpha() and character.isupper():
                names = ["shift", character.lower()]
            elif names is None and (character.isalpha() or
                                    character.isdigit()):
                names = [character]

            # Refuses a character that has no key in the table.
            if names is None:
                raise SystemExit(f"boot-test: no key for {character!r}")
            keys = [{"type": "qcode", "data": name} for name in names]
            answer = self.command("send-key", keys=keys)
            if "error" in answer:
                raise SystemExit(f"boot-test: send-key: {answer['error']}")
            time.sleep(0.05)
        self.command("send-key", keys=[{"type": "qcode", "data": "ret"}])

    def send_mouse(self, text: str) -> None:
        """Moves the emulated mouse by "DX,DY" and clicks ",BUTTON" if named."""
        parts = text.split(",")
        events = [
            {"type": "rel", "data": {"axis": "x", "value": int(parts[0])}},
            {"type": "rel", "data": {"axis": "y", "value": int(parts[1])}},
        ]
        answer = self.command("input-send-event", events=events)
        if "error" in answer:
            raise SystemExit(f"boot-test: input-send-event: {answer['error']}")

        # Presses and releases the named button (left, middle or right).
        if len(parts) > 2:
            for down in (True, False):
                button = {"type": "btn",
                          "data": {"button": parts[2], "down": down}}
                answer = self.command("input-send-event", events=[button])
                if "error" in answer:
                    raise SystemExit(
                        f"boot-test: input-send-event: {answer['error']}")
                time.sleep(0.05)

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

    # The pcat console centres the part of a row or column of cells that does
    # not fit (1080 = 67 * 16 + 8 starts the rows 4 pixels down).
    margins = ((width % GLYPH_WIDTH) // 2, (height % GLYPH_HEIGHT) // 2)
    if margins not in origins:
        origins.append(margins)

    # A text a window system draws (a terminal in an X window, ws275) sits at
    # an origin of its own that is on none of the grids above; the caller
    # names it in BOOT_TEXT_ORIGINS as "x,y" pairs separated by spaces.
    for pair in os.environ.get("BOOT_TEXT_ORIGINS", "").split():
        left_text, _, top_text = pair.partition(",")
        origin = (int(left_text), int(top_text))
        if origin not in origins:
            origins.append(origin)
    # Keeps the reading with the most recognised glyphs over unrecognised
    # cells: a blank part of a large screen (the centred grid on a 1920x1080
    # console that writes from the corner, ws035-p112) recognises nothing.
    best: list[str] = []
    best_score = None
    for left, top in origins:
        for pitch in (GLYPH_WIDTH, GLYPH_WIDTH + 1):
            rows = read_cells(width, height, pixels, pitch, font, left, top)
            unknown = sum(line.count("�") for line in rows)
            known = sum(len(line.replace(" ", "")) for line in rows) - unknown
            score = known - unknown
            if best_score is None or score > best_score:
                best = rows
                best_score = score
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


DEFAULT_PATTERN = r"(?:^(?:\S+ )?|\S)login: ?"


class SerialLine:
    """The guest's serial line, used only to type on.

    The emulator holds a guest whose output nobody reads once a client is
    connected, so a thread reads the line and drops what arrives.
    """

    def __init__(self, path: str, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.socket = socket.socket(socket.AF_UNIX,
                                            socket.SOCK_STREAM)
                self.socket.connect(path)
                break
            except OSError:
                # Waits for the emulator to create its socket.
                if time.monotonic() >= deadline:
                    raise SystemExit("boot-test: no serial socket")
                time.sleep(0.2)
        drain = threading.Thread(target=self.drain, daemon=True)
        drain.start()

    def drain(self) -> None:
        """Reads the guest's output away until the line closes."""
        while True:
            try:
                chunk = self.socket.recv(65536)
            except OSError:
                return

            # Stops when the emulator closes the line.
            if not chunk:
                return

    def type_line(self, text: str) -> None:
        """Types one line and the return key."""
        self.socket.sendall(text.encode() + b"\r")


def save_screen(frame: Path, destination: Path, rows: list[str]) -> None:
    """Keeps one screen as a PNG and the text read from it."""
    write_png(frame, destination)
    destination.with_suffix(".txt").write_text(
        "\n".join(rows) + "\n", encoding="utf-8")


def watch_patterns(monitor: Monitor, frame: Path, font: dict[bytes, str],
                   patterns: list[re.Pattern], deadline: float,
                   interval: float, keep: Path | None) -> tuple[bool,
                                                               list[str]]:
    """Looks at the screen until every pattern has been seen on one.

    A pattern is matched against each line; it is anchored by itself, so a
    path inside a message -- "getty: /bin/login: Connection timed out" --
    is not read as the prompt, and the rest of the line is free: a daemon
    writing while the prompt is up lands on the same line.  When `keep` is
    given, the first screen showing pattern N is kept as keep-N.png.
    """
    seen = [False] * len(patterns)
    last: list[str] = []
    while time.monotonic() < deadline:
        monitor.screendump(frame)
        last = read_text(frame, font)
        for index, pattern in enumerate(patterns):
            # Records a pattern seen for the first time.
            if not seen[index] and any(pattern.search(line)
                                       for line in last):
                seen[index] = True
                if keep is not None:
                    save_screen(frame, keep.with_name(
                        f"{keep.stem}-{index + 1}.png"), last)

        # Stops as soon as every pattern has been seen.
        if all(seen):
            return True, last
        time.sleep(interval)
    for index, pattern in enumerate(patterns):
        # Says which pattern never came.
        if not seen[index]:
            print(f"boot-test: never saw {pattern.pattern!r}",
                  file=sys.stderr)
    return False, last


class StepAction(argparse.Action):
    """Collects --then-type and --then-expect in the order given."""

    def __call__(self, parser, namespace, values, option_string=None):
        steps = getattr(namespace, "steps", None) or []
        kind = {"--then-type": "type", "--then-keys": "keys",
                "--then-mouse": "mouse"}.get(option_string, "expect")
        steps.append((kind, values))
        namespace.steps = steps


def main() -> int:
    """Runs the boot watch."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--monitor", required=True)
    parser.add_argument("--screenshot", required=True)
    parser.add_argument("--frame", default=None,
                        help="scratch file for each screen frame "
                             "(default: beside the screenshot)")
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--interval", type=float, default=2.0,
                        help="seconds between two looks at the screen")
    parser.add_argument("--pattern", action="append", default=None,
                        help="a line the screen must show; repeat for "
                             "several (default: the login prompt)")
    parser.add_argument("--serial", default=None,
                        help="the guest's serial socket, typed on by the "
                             "steps")
    parser.add_argument("--then-type", action=StepAction, dest="steps",
                        help="after the patterns, types a line on the "
                             "serial socket")
    parser.add_argument("--then-keys", action=StepAction, dest="steps",
                        help="after the patterns, types a line on the "
                             "emulated keyboard through the monitor")
    parser.add_argument("--then-mouse", action=StepAction, dest="steps",
                        help="after the patterns, moves the emulated mouse "
                             "by DX,DY and clicks BUTTON if given")
    parser.add_argument("--then-expect", action=StepAction, dest="steps",
                        help="after the patterns, waits for a line on the "
                             "screen")
    parser.add_argument("--then-timeout", type=float, default=120.0)
    arguments = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    font = load_font(
        root / "src/drivers/platform/pcat/graphics/vgafont.c")
    texts = arguments.pattern or [DEFAULT_PATTERN]
    patterns = [re.compile(text) for text in texts]
    steps = arguments.steps or []
    screenshot = Path(arguments.screenshot)
    frame = screenshot.with_suffix(".ppm")
    if arguments.frame is not None:
        frame = Path(arguments.frame)

    # Refuses steps that have nowhere to type.
    if any(kind == "type" for kind, _ in steps) and arguments.serial is None:
        raise SystemExit("boot-test: --then-* needs --serial")
    monitor = Monitor(arguments.monitor, timeout=30.0)

    # Watches the boot; several patterns keep the screen each first showed on.
    keep = screenshot if len(patterns) > 1 else None
    deadline = time.monotonic() + arguments.timeout
    found, last = watch_patterns(monitor, frame, font, patterns, deadline,
                                 arguments.interval, keep)
    if frame.exists():
        save_screen(frame, screenshot, last)
        frame.unlink(missing_ok=True)

    # Reports a boot that never showed what was asked for.
    if not found:
        return 1
    print("boot-test: saw " + ", ".join(repr(text) for text in texts))

    # Succeeds at once when nothing is to be typed.
    if not steps:
        return 0

    # Types each step's line and waits for each step's screen.
    line = None
    if any(kind == "type" for kind, _ in steps):
        line = SerialLine(arguments.serial, timeout=30.0)
    after = screenshot.with_name(f"{screenshot.stem}-after.png")
    for kind, text in steps:
        if kind == "type":
            line.type_line(text)
            continue
        if kind == "keys":
            monitor.send_keys(text)
            continue
        if kind == "mouse":
            monitor.send_mouse(text)
            continue
        deadline = time.monotonic() + arguments.then_timeout
        found, last = watch_patterns(monitor, frame, font,
                                     [re.compile(text)], deadline,
                                     arguments.interval, None)
        if frame.exists():
            save_screen(frame, after, last)
            frame.unlink(missing_ok=True)

        # Stops at the first screen that never came.
        if not found:
            return 1
        print(f"boot-test: then saw {text!r}")

    # Succeeded: every pattern and every step's screen was seen.
    return 0


if __name__ == "__main__":
    sys.exit(main())

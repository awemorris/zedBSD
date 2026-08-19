#!/usr/bin/env python3
"""Generate deterministic UTF-8 byte arrays for the HAL console."""

from pathlib import Path
import re
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} INPUT OUTPUT", file=sys.stderr)
        return 2

    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    entries: list[tuple[str, bytes]] = []
    for line_number, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        try:
            identifier, text = line.split("\t", 1)
        except ValueError as error:
            raise SystemExit(f"{source}:{line_number}: expected a tab") from error
        if not re.fullmatch(r"[a-z][a-z0-9_]*", identifier):
            raise SystemExit(f"{source}:{line_number}: invalid identifier {identifier!r}")
        entries.append((identifier, text.encode("utf-8")))

    lines = [
        "/* Generated from src/kern/messages.txt; do not edit. */",
        "#ifndef ZEDBSD_MESSAGES_H",
        "#define ZEDBSD_MESSAGES_H",
        "",
    ]
    for identifier, encoded in entries:
        values = ", ".join(f"0x{byte:02x}" for byte in encoded + b"\0")
        lines.append(f"static const char zedbsd_msg_{identifier}[] = {{ {values} }};")
    lines.extend(["", "#endif", ""])
    contents = "\n".join(lines)
    if not output.exists() or output.read_text(encoding="ascii") != contents:
        output.write_text(contents, encoding="ascii", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

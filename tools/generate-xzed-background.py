#!/usr/bin/env python3
"""Generate the 640x480 monochrome Xzed root background."""

from pathlib import Path
import argparse


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    width, height, cell = 640, 480, 8
    rows = []
    for y in range(height):
        row = "".join(
            "." if ((x // cell) + (y // cell)) % 2 == 0 else "+"
            for x in range(width)
        )
        rows.append(f'"{row}"')
    body = [
        "/* XPM */",
        "static char *xzed_background[] = {",
        f'"{width} {height} 2 1",',
        '". c #000000",',
        '"+ c #ffffff",',
    ]
    body.extend(f"{row}," for row in rows[:-1])
    body.append(rows[-1])
    body.append("};")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(body) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()

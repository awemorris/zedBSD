#!/usr/bin/env python3
"""Verify clear/store and its two initial dummy tiles using the pinned 4.2 XML."""
import hashlib
from pathlib import Path
import runpy
import sys
import xml.etree.ElementTree as ET


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: clear-packet-check.py PINNED_XML HOST_OUTPUT")
    oracle = runpy.run_path(str(Path(__file__).with_name("noop-packet-check.py")))
    xml = Path(sys.argv[1]).read_bytes()
    if hashlib.sha256(xml).hexdigest() != oracle["PINNED_SHA256"]:
        raise SystemExit("packet XML does not match WS141's audited snapshot")
    root = ET.fromstring(xml)
    packet = oracle["packet_image"]
    common = {"Image Width (pixels)": 64, "Image Height (pixels)": 64,
              "Number of Render Targets": 1, "Early-Z disable": 1}
    render = [
        ("Tile Rendering Mode Cfg (Common)", common),
        ("Tile Rendering Mode Cfg (Clear Colors Part1)", {"Clear Color low 32 bits": 0xff317ce0}),
        ("Tile Rendering Mode Cfg (Color)", {"Render Target 0 Internal Type": 2}),
        ("Tile Rendering Mode Cfg (ZS Clear Values)", {"Z Clear Value": 0x3f800000}),
        ("Tile List Initial Block Size", {"Use auto-chained tile lists": 1}),
        ("Multicore Rendering Tile List Set Base", {"address": 0x40000}),
        ("Multicore Rendering Supertile Cfg", {"Number of Bin Tile Lists": 1,
            "Total Frame Height in Tiles": 1, "Total Frame Width in Tiles": 1,
            "Total Frame Height in Supertiles": 1, "Total Frame Width in Supertiles": 1,
            "Supertile Height in Tiles": 1, "Supertile Width in Tiles": 1}),
    ]
    for initial_tile in range(2):
        render += [("Tile Coordinates", {}), ("End of Loads", {}),
                   ("Store Tile Buffer General", {"Buffer to Store": 8})]
        if initial_tile == 0:
            render.append(("Clear Tile Buffers", {"Clear Z/Stencil Buffer": 1,
                                                   "Clear all Render Targets": 1}))
        render.append(("End of Tile Marker", {}))
    render += [("Flush VCD cache", {}),
               ("Start Address of Generic Tile List", {"start": 0x30000, "end": 0x30013}),
               ("Supertile Coordinates", {}), ("End of rendering", {})]
    streams = {
        "bin": [("Number of Layers", {"Number of Layers": 1}),
                ("Tile Binning Mode Cfg", {"Width (in pixels)": 64,
                    "Height (in pixels)": 64, "Number of Render Targets": 1}),
                ("Flush VCD cache", {}), ("Start Tile Binning", {}), ("Flush", {})],
        "render": render,
        "tile": [("Tile Coordinates Implicit", {}), ("End of Loads", {}),
                 ("Branch to Implicit Tile List", {}),
                 ("Store Tile Buffer General", {"Buffer to Store": 0, "Output Image Format": 27,
                     "Height in UB or Stride": 256, "Address": 0x100000}),
                 ("End of Tile Marker", {}), ("Return from sub-list", {})],
    }
    observed = {}
    for line in Path(sys.argv[2]).read_text().splitlines():
        if line.startswith("clear-image "):
            _, name, encoded = line.split()
            observed[name] = bytes.fromhex(encoded)
    if observed.keys() != streams.keys():
        raise SystemExit("missing or unexpected clear streams")
    for name, fields in streams.items():
        expected = b"".join(packet(root, kind, values) for kind, values in fields)
        if observed[name] != expected:
            raise SystemExit(f"{name}: generated clear stream differs from fixed 4.2 XML")
    print("clear-packet-check PASS")


if __name__ == "__main__":
    main()

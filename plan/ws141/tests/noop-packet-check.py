#!/usr/bin/env python3
"""Compare the generated noop streams with fields from the pinned V3D 4.2 XML.

The oracle reads the primary packet definition rather than the C templates or
their length constants.  It needs WS141's recovered, ignored Mesa snapshot.
"""

import hashlib
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


PINNED_SHA256 = "b13f995b1a4b606a1adcb2c048ff9bca067106be14e22ce1ee86e01055094e1b"


def packet_image(root, name, values):
    packets = [node for node in root.findall("packet")
               if node.get("name") == name
               and int(node.get("min_ver", "0")) <= 42
               and int(node.get("max_ver", "999")) >= 42]
    if len(packets) != 1:
        raise ValueError(f"expected one 4.2 definition for {name}")
    packet = packets[0]
    payload = 0
    bits = 0
    names = set()
    for field in packet.findall("field"):
        field_name = field.get("name")
        names.add(field_name)
        start = int(field.get("start"))
        size = int(field.get("size"))
        bits = max(bits, start + size)
        value = values.get(field_name, int(field.get("default", "0")))
        if field.get("minus_one") == "true":
            value -= 1
        if field.get("type") == "address":
            # Address fields preserve their original low-bit alignment.
            alignment_bits = start % 8
            if value & ((1 << alignment_bits) - 1):
                raise ValueError(f"unaligned {name}/{field_name}")
            value >>= alignment_bits
        if not 0 <= value < (1 << size):
            raise ValueError(f"unrepresentable {name}/{field_name}")
        payload |= value << start
    if values.keys() - names:
        raise ValueError(f"unknown fields in {name}: {values.keys() - names}")
    return bytes([int(packet.get("code"))]) + payload.to_bytes((bits + 7) // 8, "little")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: noop-packet-check.py PINNED_XML HOST_OUTPUT")
    xml = Path(sys.argv[1]).read_bytes()
    if hashlib.sha256(xml).hexdigest() != PINNED_SHA256:
        raise SystemExit("packet XML does not match WS141's audited snapshot")
    root = ET.fromstring(xml)
    streams = {
        "bin": [
            ("Number of Layers", {"Number of Layers": 1}),
            ("Tile Binning Mode Cfg", {"Width (in pixels)": 1,
                                       "Height (in pixels)": 1,
                                       "Number of Render Targets": 1}),
            ("Flush VCD cache", {}),
            ("Start Tile Binning", {}),
            ("Flush", {}),
        ],
        "render": [
            ("Tile Rendering Mode Cfg (Common)", {"Image Width (pixels)": 1,
                                                  "Image Height (pixels)": 1,
                                                  "Number of Render Targets": 1,
                                                  "Early-Z disable": 1}),
            ("Tile Rendering Mode Cfg (Color)", {"Render Target 0 Internal Type": 2}),
            ("Tile Rendering Mode Cfg (ZS Clear Values)", {"Z Clear Value": 0x3f800000}),
            ("Tile List Initial Block Size", {"Use auto-chained tile lists": 1}),
            ("Multicore Rendering Tile List Set Base", {"address": 0x45678000}),
            ("Multicore Rendering Supertile Cfg", {"Number of Bin Tile Lists": 1,
                                                   "Total Frame Height in Tiles": 1,
                                                   "Total Frame Width in Tiles": 1,
                                                   "Total Frame Height in Supertiles": 1,
                                                   "Total Frame Width in Supertiles": 1,
                                                   "Supertile Height in Tiles": 1,
                                                   "Supertile Width in Tiles": 1}),
            ("Start Address of Generic Tile List", {"start": 0x3abcd013,
                                                     "end": 0x3abcd026}),
            ("Supertile Coordinates", {}),
            ("End of rendering", {}),
        ],
        "tile": [
            ("Tile Coordinates Implicit", {}),
            ("End of Loads", {}),
            ("Branch to Implicit Tile List", {}),
            ("Store Tile Buffer General", {"Buffer to Store": 8}),
            ("End of Tile Marker", {}),
            ("Return from sub-list", {}),
        ],
    }
    observed = {}
    for line in Path(sys.argv[2]).read_text().splitlines():
        if line.startswith("noop-image "):
            _, name, encoded = line.split()
            if name in observed:
                raise ValueError(f"duplicate image: {name}")
            observed[name] = bytes.fromhex(encoded)
    if observed.keys() != streams.keys():
        raise ValueError("missing or unexpected generated streams")
    for name, packets in streams.items():
        expected = b"".join(packet_image(root, packet, values) for packet, values in packets)
        if observed[name] != expected:
            raise ValueError(f"{name} differs from pinned XML: expected {expected.hex()}, got {observed[name].hex()}")
    print("noop-packet-check: PASS (4.2 XML, bin/render/tile)")


if __name__ == "__main__":
    main()

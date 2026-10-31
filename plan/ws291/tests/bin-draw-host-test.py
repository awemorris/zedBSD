#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Decodes bin-draw-host-test's hex lines with Mesa's v3d_packet.xml and checks the draws.

Usage: bin-draw-host-test.py VERSION XML < lines   (VERSION 42 or 71)
Every sequence must decode packet by packet to exactly its length; the draw
after the state must be the expected packets with the expected fields, and
the rest NOPs.
"""
import sys
import xml.etree.ElementTree as ET


def applies(element, version):
    low = int(element.get("min_ver", "0"))
    high = int(element.get("max_ver", "999"))
    return low <= version <= high


def start_bits(text):
    if text.endswith("b"):
        return int(text[:-1]) * 8
    return int(text)


def fields_of(element, version):
    fields = {}
    for field in element.iter("field"):
        if not applies(field, version):
            continue
        fields[field.get("name")] = (start_bits(field.get("start")), int(field.get("size")))
    return fields


def load(path, version):
    root = ET.parse(path).getroot()
    packets = {}
    for packet in root.iter("packet"):
        if not applies(packet, version):
            continue
        fields = fields_of(packet, version)
        bits = max([start + size for start, size in fields.values()] + [0])
        packets[int(packet.get("code"))] = (packet.get("name"), 1 + (bits + 7) // 8, fields)
    structs = {s.get("name"): fields_of(s, version) for s in root.iter("struct") if applies(s, version)}
    return packets, structs


def value(payload, start, size):
    number = int.from_bytes(payload, "little")
    return (number >> start) & ((1 << size) - 1)


def decode(packets, data):
    out = []
    offset = 0
    while offset < len(data):
        code = data[offset]
        assert code in packets, f"unknown packet {code} at {offset}"
        name, length, fields = packets[code]
        payload = data[offset + 1:offset + length]
        assert len(payload) == length - 1, f"{name} runs past the sequence"
        out.append((name, {key: value(payload, *field) for key, field in fields.items()}))
        offset += length
    return out


EXPECT = {
    "array": [
        ("Base Vertex Base Instance", {"Base Vertex": 0, "Base Instance": 0}),
        ("Vertex Array Prims", {"mode": 4, "Length": 36, "Index of First Vertex": 7}),
    ],
    "array-instanced": [
        ("Base Vertex Base Instance", {"Base Vertex": 0, "Base Instance": 3}),
        ("Vertex Array Instanced Prims", {"mode": 4, "Instance Length": 3, "Number of Instances": 5000, "Index of First Vertex": 0}),
    ],
    "indexed32": [
        ("Base Vertex Base Instance", {"Base Vertex": 0xfffffffe, "Base Instance": 0}),
        ("Index Buffer Setup", {"Address": 0x10000, "Size": 4096}),
        ("Indexed Prim List", {"mode": 4, "Index type": 2, "Length": 300, "Enable Primitive Restarts": 0, "Index Offset": 48}),
    ],
    "indexed16-instanced": [
        ("Base Vertex Base Instance", {"Base Vertex": 0, "Base Instance": 1}),
        ("Index Buffer Setup", {"Address": 0x20002, "Size": 2004}),
        ("Indexed Instanced Prim List", {"mode": 4, "Index type": 1, "Instance Length": 1000, "Number of Instances": 6500, "Enable Primitive Restarts": 0, "Index Offset": 4}),
    ],
}

ATTRIBUTES = {
    "attribute-vertex": {"Address": 0x123450, "Vec size": 3, "Type": 2, "Number of values read by Coordinate shader": 3,
                         "Number of values read by Vertex shader": 3, "Instance Divisor": 0, "Stride": 32, "Maximum Index": 4095},
    "attribute-instance": {"Address": 0x123450, "Vec size": 0, "Type": 2, "Number of values read by Coordinate shader": 2,
                           "Number of values read by Vertex shader": 2, "Instance Divisor": 1, "Stride": 16, "Maximum Index": 4095},
}

STATE_BYTES = 123


def main():
    version = int(sys.argv[1])
    packets, structs = load(sys.argv[2], version)
    record = structs["GL Shader State Attribute Record"]
    seen = 0
    for line in sys.stdin:
        name, text = line.split()
        data = bytes.fromhex(text)
        if name in ATTRIBUTES:
            got = {key: value(data, *field) for key, field in record.items()}
            for key, want in ATTRIBUTES[name].items():
                assert got[key] == want, f"{name}: {key} {got[key]} != {want}"
            seen += 1
            continue
        state = decode(packets, data[:STATE_BYTES])
        assert state[-1][0] == "Stencil Cfg", f"{name}: state ends with {state[-1][0]}"
        draw = decode(packets, data[STATE_BYTES:])
        want = EXPECT[name]
        for index, (packet, values) in enumerate(want):
            assert draw[index][0] == packet, f"{name}: packet {index} is {draw[index][0]}, not {packet}"
            for key, number in values.items():
                assert draw[index][1][key] == number, f"{name}: {packet} {key} {draw[index][1][key]} != {number}"
        rest = [packet for packet, _ in draw[len(want):]]
        assert all(packet == "NOP" for packet in rest), f"{name}: after the draw {rest}"
        print(f"bin-draw-host-test {version}: {name}: {len(state)} state packets, " + ", ".join(p for p, _ in want) + f", {len(rest)} NOPs")
        seen += 1
    assert seen == len(EXPECT) + len(ATTRIBUTES), f"{seen} lines"
    print(f"bin-draw-host-test {version}: PASS")


main()

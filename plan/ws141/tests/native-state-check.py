#!/usr/bin/env python3
"""Check complete native records against fixed 4.2 XML and decode UIF blocks.

The pixel oracle walks physical blocks/utiles in storage order and recovers
coordinates, rather than reusing the production raster-to-tiled offset formula.
No native fetch, filtering, QPU scheduling or DMA is exercised here.
"""

import hashlib
from pathlib import Path
import runpy
import sys
import xml.etree.ElementTree as ET


def structure_image(root, name, values):
    nodes = [node for node in root.findall("struct")
             if node.get("name") == name
             and int(node.get("min_ver", "0")) <= 42
             and int(node.get("max_ver", "999")) >= 42]
    if len(nodes) != 1:
        raise ValueError(f"expected one 4.2 structure: {name}")
    encoded = 0
    length = 0
    known = set()
    for field in nodes[0].findall("field"):
        field_name = field.get("name")
        known.add(field_name)
        text = field.get("start")
        start = int(text[:-1]) * 8 if text.endswith("b") else int(text)
        size = int(field.get("size"))
        length = max(length, start + size)
        value = values.get(field_name, int(field.get("default", "0")))
        if field.get("minus_one") == "true":
            value -= 1
        if field.get("type") == "address":
            low_bits = start % 32
            if value & ((1 << low_bits) - 1):
                raise ValueError(f"unaligned {name}/{field_name}")
            value >>= low_bits
        if not 0 <= value < 1 << size:
            raise ValueError(f"unrepresentable {name}/{field_name}")
        encoded |= value << start
    if values.keys() - known:
        raise ValueError(f"unknown {name} fields: {values.keys() - known}")
    return encoded.to_bytes((length + 7) // 8, "little")


def shader_fields():
    fields = {
        "Enable clipping": 1,
        "Turn off early-z test": 1,
        "Fragment shader uses real pixel centre W in addition to centroid W2": 1,
        "Disable implicit point/line varyings": 1,
        "Number of varyings in Fragment Shader": 32,
        "Coordinate Shader output VPM segment size": 4,
        "Coordinate Shader input VPM segment size": 1,
        "Min Coord Shader input segments required in play": 1,
        "Vertex Shader output VPM segment size": 5,
        "Vertex Shader input VPM segment size": 1,
        "Min Vertex Shader input segments required in play": 1,
        "Address of default attribute values": 0xabcde000,
    }
    for stage, index in (("Coordinate", 0), ("Vertex", 1), ("Fragment", 2)):
        fields[f"{stage} Shader Code Address"] = 0x12345000 + index * 4096
        fields[f"{stage} Shader Uniforms Address"] = 0x56789000 + index * 4096
        fields[f"{stage} Shader Propagate NaNs"] = 1
        fields[f"{stage} Shader start in final thread section"] = int(index != 2)
    return fields


def texture_fields(width, height, stride, swap):
    return {
        "Texture base pointer": 0x23456000,
        "Array Stride (64-byte aligned)": stride,
        "Image Width": width,
        "Image Height": height,
        "Image Depth": 1,
        "Texture type": 4,
        "Extended": 1,
        "Swizzle R": 4 if swap else 2,
        "Swizzle G": 3,
        "Swizzle B": 2 if swap else 4,
        "Swizzle A": 5,
        "Level 0 is strictly UIF": 1,
        "UIF XOR disable": 1,
    }


def decode_pixels(tiled, raster):
    if len(tiled) != 34816 or len(raster) != 20960:
        raise ValueError("wrong full pixel reservations")
    seen = set()
    cursor = 0
    for column in range(2):
        for block_row in range(17):
            for block_column in range(4):
                for utile_row in range(2):
                    for utile_column in range(2):
                        for row in range(4):
                            for pixel in range(4):
                                x = column * 32 + block_column * 8 + utile_column * 4 + pixel
                                y = block_row * 8 + utile_row * 4 + row
                                observed = tiled[cursor:cursor + 4]
                                cursor += 4
                                expected = bytes(4)
                                if x < 37 and y < 131:
                                    expected = bytes((x, 0, y, 0))
                                    seen.add((x, y))
                                if observed != expected:
                                    raise ValueError(f"UIF pixel/padding differs at {x},{y}")
    if len(seen) != 37 * 131 or cursor != len(tiled):
        raise ValueError("UIF walk omitted or repeated a visible word")
    for y in range(131):
        expected = b"".join(bytes((x, 0, y, 0)) for x in range(37)) + bytes([0x5a]) * 12
        if raster[y * 160:(y + 1) * 160] != expected:
            raise ValueError(f"source raster/padding changed at row {y}")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: native-state-check.py PINNED_XML HOST_OUTPUT")
    oracle = runpy.run_path(str(Path(__file__).with_name("noop-packet-check.py")))
    xml = Path(sys.argv[1]).read_bytes()
    if hashlib.sha256(xml).hexdigest() != oracle["PINNED_SHA256"]:
        raise ValueError("packet XML differs from the audited fixed snapshot")
    root = ET.fromstring(xml)
    observed = {}
    for line in Path(sys.argv[2]).read_text().splitlines():
        if line.startswith("native-image "):
            _, name, encoded = line.split()
            if name in observed:
                raise ValueError(f"duplicate native image: {name}")
            observed[name] = bytes.fromhex(encoded)
    expected = {
        "shader": ("GL Shader State Record", shader_fields()),
        "texture-rgba": ("Texture Shader State", texture_fields(1, 1, 16, False)),
        "texture-bgra": ("Texture Shader State", texture_fields(37, 131, 544, True)),
        "sampler": ("Sampler State", {"Mag filter Nearest": 1,
                                       "Mip filter Nearest": 1,
                                       "sRGB Disable": 1,
                                       "Wrap S": 2, "Wrap T": 1, "Wrap R": 1}),
    }
    for components in range(1, 5):
        expected[f"attribute{components}"] = ("GL Shader State Attribute Record", {
            "Address": 0x98765430, "Vec size": components % 4,
            "Type": 2, "Number of values read by Coordinate shader": components,
            "Number of values read by Vertex shader": components - 1,
            "Stride": 2048, "Maximum Index": 0x12345678,
        })
    if observed.keys() != expected.keys() | {"tiled", "raster", "bin"}:
        raise ValueError("missing or unexpected native images")
    for name, (structure, fields) in expected.items():
        encoded = structure_image(root, structure, fields)
        if encoded != observed[name]:
            raise ValueError(f"{name} differs from pinned XML: {encoded.hex()} != {observed[name].hex()}")
    packets = [
        ("Point size", {"Point Size": 0x3f800000}),
        ("Line width", {"Line width": 0x3f800000}),
        ("Sample State", {"Mask": 15, "Coverage": 0x3f80}),
        ("clip_window", {"Clip Window Left Pixel Coordinate": 3,
                         "Clip Window Bottom Pixel Coordinate": 4,
                         "Clip Window Width in pixels": 5,
                         "Clip Window Height in pixels": 6}),
        ("Clipper XY Scaling", {"Viewport Half-Width in 1/256th of pixel": 0x45000000,
                                "Viewport Half-Height in 1/256th of pixel": 0x44800000}),
        ("Clipper Z Scale and Offset", {"Viewport Z Scale (Zc to Zs)": 0x3f800000}),
        ("Clipper Z min/max clipping planes", {"Maximum Zw": 0x3f800000}),
        ("Viewport Offset", {"Coarse X": 1023, "Fine X": 40 * 256,
                             "Coarse Y": 1023, "Fine Y": 4 * 256}),
        ("Cfg Bits", {"Direct3D Provoking Vertex": 1, "Depth-Test Function": 7,
                      "Line Rasterization": 1, "Enable Reverse Facing Primitive": 1,
                      "Clockwise Primitives": 1}),
        ("Color Write Masks", {"Mask": 0xfff0}),
        ("Blend Enables", {}),
        ("Transform Feedback Specs", {}),
        ("Occlusion Query Counter", {}),
    ]
    for name, noun, mask in (("Flat Shade Flags", "Flat Shade", 0x81000001),
                             ("Non-perspective Flags", "Non-perspective", 0x40000002)):
        packets.append((name, {f"{noun} Flags for varyings V0*24": mask & 0xffffff,
                               f"Action for {noun} Flags of higher numbered varyings": 1}))
        packets.append((name, {"Varying offset V0": 1,
                               f"{noun} Flags for varyings V0*24": mask >> 24,
                               f"Action for {noun} Flags of higher numbered varyings": 1}))
    packets.extend([
        ("Zero All Centroid Flags", {}),
        ("VCM Cache Size", {"Number of 16-vertex batches for rendering": 2,
                            "Number of 16-vertex batches for binning": 2}),
        ("GL Shader State", {"address": 0x12345000, "number of attribute arrays": 3}),
        ("Vertex Array Prims", {"mode": 4, "Length": 9}),
    ])
    bin_image = b"".join(oracle["packet_image"](root, name, fields) for name, fields in packets)
    if observed["bin"] != bin_image:
        raise ValueError(f"bin differs from fixed XML: {bin_image.hex()} != {observed['bin'].hex()}")
    decode_pixels(observed["tiled"], observed["raster"])
    print("native-state-check: PASS (8 full 4.2 records and complete draw BCL; 4847 pixels, zero UIF padding and unchanged raster)")


if __name__ == "__main__":
    main()

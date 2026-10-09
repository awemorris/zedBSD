#!/usr/bin/env python3
"""Compare complete native pass streams with independently packed fixed V3D 4.2 XML fields.

Addresses are synthetic; the preserved draw payload tests enclosing stream
assembly and supplies no GPU execution or shader-input lifetime evidence.
"""

import hashlib
from pathlib import Path
import runpy
import sys
import xml.etree.ElementTree as ET


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: native-pass-check.py PINNED_XML HOST_OUTPUT")
    oracle = runpy.run_path(str(Path(__file__).with_name("noop-packet-check.py")))
    xml = Path(sys.argv[1]).read_bytes()
    if hashlib.sha256(xml).hexdigest() != oracle["PINNED_SHA256"]:
        raise ValueError("packet XML differs from the audited fixed snapshot")
    root = ET.fromstring(xml)
    packet = lambda name, **fields: oracle["packet_image"](root, name, fields)
    observed = {}
    for line in Path(sys.argv[2]).read_text().splitlines():
        if line.startswith("pass-image "):
            _, name, encoded = line.split()
            if name in observed:
                raise ValueError(f"duplicate pass image: {name}")
            observed[name] = bytes.fromhex(encoded)

    bin_prefix = packet("Number of Layers", **{"Number of Layers": 1})
    bin_prefix += packet("Tile Binning Mode Cfg", **{
        "Width (in pixels)": 130, "Height (in pixels)": 129,
        "Number of Render Targets": 1})
    bin_prefix += packet("Flush VCD cache") + packet("Start Tile Binning")
    expected = {
        "load-bin": bin_prefix + bytes((index * 17 + 3) & 255 for index in range(232)) + packet("Flush"),
        "clear-bin": bin_prefix + packet("Flush"),
    }
    for load, store, name in ((True, True, "load"), (False, True, "clear"), (False, False, "discard")):
        tile = packet("Tile Coordinates Implicit")
        if load:
            tile += packet("Load Tile Buffer General", **{
                "Address": 0x400000, "Height in UB or Stride": 576,
                "Input Image Format": 27})
        tile += packet("End of Loads")
        tile += packet("Prim List Format", **{"primitive type": 2})
        tile += packet("Set InstanceID")
        tile += packet("Branch to Implicit Tile List")
        if store:
            tile += packet("Store Tile Buffer General", **{
                "Address": 0x400000, "Height in UB or Stride": 576,
                "Output Image Format": 27})
        else:
            tile += packet("Store Tile Buffer General", **{"Buffer to Store": 8})
        tile += packet("Clear Tile Buffers", **{
            "Clear Z/Stencil Buffer": 1, "Clear all Render Targets": 1})
        tile += packet("End of Tile Marker") + packet("Return from sub-list")
        expected[f"{name}-tile"] = tile
        if name == "discard":
            continue
        render = packet("Tile Rendering Mode Cfg (Common)", **{
            "Image Width (pixels)": 130, "Image Height (pixels)": 129,
            "Number of Render Targets": 1, "Early-Z disable": 1})
        render += packet("Tile Rendering Mode Cfg (Clear Colors Part1)", **{
            "Clear Color low 32 bits": 0x12345678})
        render += packet("Tile Rendering Mode Cfg (Color)", **{"Render Target 0 Internal Type": 2})
        render += packet("Tile Rendering Mode Cfg (ZS Clear Values)", **{"Z Clear Value": 0x3f800000})
        render += packet("Tile List Initial Block Size", **{"Use auto-chained tile lists": 1})
        render += packet("Multicore Rendering Supertile Cfg", **{
            "Number of Bin Tile Lists": 1, "Total Frame Height in Tiles": 3,
            "Total Frame Width in Tiles": 3, "Total Frame Height in Supertiles": 3,
            "Total Frame Width in Supertiles": 3, "Supertile Height in Tiles": 1,
            "Supertile Width in Tiles": 1})
        for dummy in range(2):
            render += packet("Tile Coordinates") + packet("End of Loads")
            render += packet("Store Tile Buffer General", **{"Buffer to Store": 8})
            if dummy == 0:
                render += packet("Clear Tile Buffers", **{
                    "Clear Z/Stencil Buffer": 1, "Clear all Render Targets": 1})
            render += packet("End of Tile Marker")
        render += packet("Flush VCD cache")
        render += packet("Multicore Rendering Tile List Set Base", **{"address": 0x800000})
        render += packet("Start Address of Generic Tile List", **{
            "start": 0x300000, "end": 0x300000 + len(tile)})
        for row in range(3):
            for column in range(1, 3):
                render += packet("Supertile Coordinates", **{
                    "row number in supertiles": row, "column number in supertiles": column})
        render += packet("End of rendering")
        expected[f"{name}-render"] = render
    if observed.keys() != expected.keys():
        raise ValueError(f"unexpected pass streams: {observed.keys() ^ expected.keys()}")
    for name, encoded in expected.items():
        if observed[name] != encoded:
            raise ValueError(f"{name} differs from fixed XML: {encoded.hex()} != {observed[name].hex()}")
    print("native-pass-check: PASS (7 complete fixed 4.2 pass streams; load, clear, discard, relocations and type/instance workarounds)")


if __name__ == "__main__":
    main()

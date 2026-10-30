#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Renders one static reftest ID (test and its first reference) with browser, so the difference can be looked at.

  ref-one.py [--chrome] [--out DIR] ID...      (ID like css/css-grid/foo.html or /css/css-grid/foo.html)

For each ID it writes DIR/<n>-test.png, <n>-ref.png and a <n>-diff.png (the test, the reference, and
a red map of differing pixels side by side in <n>-all.png) and prints the number of differing pixels.
With --chrome the same pages are also rendered with chrome-run.js (<n>-ctest.png, <n>-cref.png).
"""
import argparse, importlib.util, json, sys, tempfile
from pathlib import Path
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("rwr", ROOT / "plan/ws074/tests/run-wpt-reftests.py")
rwr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rwr)
from PIL import Image, ImageChops

def refs_of(path):
    p = rwr.ReftestMetadata()
    p.feed((rwr.WPT / path).read_text(errors="replace"))
    out = []
    for rel, href in p.references:
        out.append((rel, str((Path(path).parent / href)) if not href.startswith("/") else href[1:]))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chrome", action="store_true")
    ap.add_argument("--out", default=str(ROOT / "build/tmp/p8/ref"))
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=600)
    ap.add_argument("ids", nargs="+")
    a = ap.parse_args()
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    fonts = rwr.font_directory()
    program = ROOT / "build/ws074-host/plain/browser"
    cprogram = ROOT / "plan/ws074/tests/chrome-run.js"
    tmp = Path(tempfile.mkdtemp(prefix="ref-", dir=out)); data = tmp / "data"; data.mkdir()
    with rwr.suite_server() as base:
        for n, tid in enumerate(a.ids, 1):
            tid = tid.lstrip("/")
            refs = refs_of(tid)
            if not refs:
                print(tid, "no reference"); continue
            rel, ref = refs[0]
            ti, _ = rwr.render(program, fonts, a.width, a.height, base + quote(tid, safe="/?=&"), tmp / "t.ppm", data, 40)
            ri, _ = rwr.render(program, fonts, a.width, a.height, base + quote(ref, safe="/?=&"), tmp / "r.ppm", data, 40)
            ti.save(out / f"{n}-test.png"); ri.save(out / f"{n}-ref.png")
            if ti.size == ri.size:
                d = ImageChops.difference(ti, ri).convert("L").point(lambda v: 255 if v else 0)
                cnt = sum(1 for v in d.getdata() if v)
                bb = d.getbbox()
            else:
                d = Image.new("L", ti.size); cnt = -1; bb = None
            w, h = ti.size
            allimg = Image.new("RGB", (w * 3 + 20, h), (128, 128, 255))
            allimg.paste(ti, (0, 0)); allimg.paste(ri, (w + 10, 0))
            red = Image.new("RGB", ti.size, (0, 0, 0)); red.paste((255, 0, 0), mask=d)
            allimg.paste(red, (2 * w + 20, 0))
            if bb:
                x0, y0, x1, y1 = max(bb[0] - 8, 0), max(bb[1] - 8, 0), min(bb[2] + 8, w), min(bb[3] + 8, h)
                x1 = min(x1, x0 + 400); y1 = min(y1, y0 + 300)
                crop = Image.new("RGB", ((x1 - x0) * 3 + 20, y1 - y0), (128, 128, 255))
                for k, im in enumerate((ti, ri, red)):
                    crop.paste(im.crop((x0, y0, x1, y1)), (k * (x1 - x0 + 10), 0))
                allimg = crop
            allimg.save(out / f"{n}-all.png")
            print(n, rel, tid, "refdiff", cnt, "bbox", bb, "size", ti.size, ri.size)
            if a.chrome:
                cti, _ = rwr.render(cprogram, fonts, a.width, a.height, base + quote(tid, safe="/?=&"), tmp / "ct.ppm", data, 60)
                cri, _ = rwr.render(cprogram, fonts, a.width, a.height, base + quote(ref, safe="/?=&"), tmp / "cr.ppm", data, 60)
                cti.save(out / f"{n}-ctest.png"); cri.save(out / f"{n}-cref.png")
                if cti.size == cri.size:
                    dd = ImageChops.difference(cti, cri).convert("L").point(lambda v: 255 if v else 0)
                    print("   chrome refdiff", sum(1 for v in dd.getdata() if v))
                # ours test vs chrome test
                if cti.size == ti.size:
                    dd = ImageChops.difference(cti, ti).convert("L").point(lambda v: 255 if v else 0)
                    print("   ours-vs-chrome test", sum(1 for v in dd.getdata() if v), dd.getbbox())

main()

#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Runs one scripted reftest ID and writes the test's and the reference's captures side by side with a difference map.

  scripted-one.py [--chrome] [--out DIR] ID...      (ID like /css/css-view-transitions/foo.html)

Writes DIR/<n>-all.png (test | reference | differing pixels, cropped to where they differ) and prints the outcome
and the differing pixel count.  With --chrome the capture is made by chrome-run.js (run it under NODE_PATH and PATH
as for the comparison measurement).
"""
import argparse, json, subprocess, sys, tempfile
from pathlib import Path
from PIL import Image, ImageChops
ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "plan/ws074/phase173/evidence/20261013-q1012/manifest.json"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chrome", action="store_true")
    ap.add_argument("--out", default=str(ROOT / "build/tmp/p8/sone"))
    ap.add_argument("--build", default=str(ROOT / "build/ws074-host/plain"))
    ap.add_argument("ids", nargs="+")
    a = ap.parse_args()
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    program = ROOT / "plan/ws074/tests/chrome-run.js" if a.chrome else Path(a.build) / "wpt-ready-render"
    for n, tid in enumerate(a.ids, 1):
        if not tid.startswith("/"):
            tid = "/" + tid
        work = Path(tempfile.mkdtemp(prefix="so-", dir=out))
        subprocess.run([sys.executable, str(ROOT / "plan/ws074/tests/run-interop2025-scripted-reftests.py"),
            "--manifest", str(MANIFEST), "--program", str(program), "--out", str(work), "--only", tid,
            "--budget-seconds", "300"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        rep = json.loads((work / "report.json").read_text())
        r = rep["results"][0]
        raw = Path(rep["raw_directory"])
        imgs = []
        for c in r["captures"]:
            rec = json.loads((raw / c["record"]).read_text())
            if "png" in rec:
                imgs.append(Image.open(rec["png"]["file"]).convert("RGB"))
            else:
                print("   capture", c.get("resource"), rec.get("outcome"), str(rec.get("readiness"))[:200])
        info = [(e["equal"], e["different_pixels"], e["fuzzy"]) for e in r["edges"]]
        print(n, tid, r["outcome"], info)
        if len(imgs) >= 2 and imgs[0].size == imgs[1].size:
            ti, ri = imgs[0], imgs[1]
            d = ImageChops.difference(ti, ri).convert("L").point(lambda v: 255 if v else 0)
            bb = d.getbbox() or (0, 0, 50, 50)
            w, h = ti.size
            x0, y0, x1, y1 = max(bb[0] - 8, 0), max(bb[1] - 8, 0), min(bb[2] + 8, w), min(bb[3] + 8, h)
            x1 = min(x1, x0 + 500); y1 = min(y1, y0 + 400)
            red = Image.new("RGB", ti.size, (0, 0, 0)); red.paste((255, 0, 0), mask=d)
            crop = Image.new("RGB", ((x1 - x0) * 3 + 20, y1 - y0), (128, 128, 255))
            for k, im in enumerate((ti, ri, red)):
                crop.paste(im.crop((x0, y0, x1, y1)), (k * (x1 - x0 + 10), 0))
            crop.save(out / f"{n}-all.png")
main()

#!/bin/sh
# Runs single static reftest IDs with the host build and keeps the images of a failure (build/tmp/p8/st1/NAME/).
#   sh plan/ws074/phase260/tools/st1.sh /css/css-grid/foo.html ...
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
cd "$(dirname -- "$0")/../../../.."
mkdir -p build/tmp/p8/st1
python3 -I - "$@" <<'P'
import json, subprocess, sys
m = json.load(open("plan/ws074/phase173/evidence/20261013-q1012/manifest.json"))
for ident in sys.argv[1:]:
    row = [r for r in m["selected"] if r["id"] == ident]
    if not row:
        print(ident, "not selected")
        continue
    name = ident.strip("/").replace("/", "_")
    out = "build/tmp/p8/st1/" + name
    mf = dict(m)
    mf["selected"] = row
    open(out + ".json", "w").write(json.dumps(mf))
    subprocess.run([sys.executable, "plan/ws074/tests/run-wpt-reftests.py", "--program", "build/ws074-host/plain/browser", "--root", ".",
        "--selection-manifest", out + ".json", "--limit", "0", "--render-timeout", "30", "--save-failures", "1", "--out", out],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    r = json.load(open(out + "/report.json"))["results"][0]
    print(ident, r["status"], {k: v for k, v in r.items() if k not in ("test", "status")})
P

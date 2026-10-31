#!/bin/sh
# Runs single harness IDs with the host build, with the comparison fonts (the --font-dir the runner needs), and prints the outcome.
#   sh plan/ws074/phase260/tools/h1.sh /css/css-grid/foo.html ...      (the result JSON is in build/tmp/p8/h1out/)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
cd "$(dirname -- "$0")/../../../.."
mkdir -p build/tmp/p8/h1out
for id in "$@"; do
	out=build/tmp/p8/h1out/$(echo "$id" | tr '/' '_').json
	timeout 120 python3 plan/ws074/tests/run-interop2025-harness.py --manifest plan/ws074/phase173/evidence/20261013-q1012/manifest.json \
	    --program build/ws074-host/plain/browser --test-id "$id" --out "$out" --font-dir userland/desktop/fonts > "$out.log" 2>&1
	python3 - "$out" "$id" <<'P'
import json, sys
out, ident = sys.argv[1:3]
try:
    d = json.load(open(out))
except Exception as e:
    print(ident, "no result", e)
    sys.exit()
print(ident, d.get("outcome"))
for l in d.get("stdout", "").splitlines():
    if l.startswith("WS074_WPT_RESULT "):
        r = json.loads(l[len("WS074_WPT_RESULT "):])
        if r["status"] != 0:
            print("   ", r["name"][:70], "|", r["message"][:200].replace("\n", " "))
P
done

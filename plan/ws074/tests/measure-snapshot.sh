#!/bin/sh
# ws074: measures the whole fixed selection with a snapshot of the host build, in a directory under /tmp, and compares with Chromium.
#
#   sh plan/ws074/tests/measure-snapshot.sh NAME [JOBS]
#
# Writes /tmp/p8out/NAME (the measurement and gap.json) and prints the comparison.  The snapshot lets the build be redone meanwhile.
# The static reftests are run in JOBS shards at once (static-subset.py) and merged.
# CHROME (default build/ws074-cmp/chrome156) is the Chromium measurement to compare with.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
name=$1
jobs=${2:-24}
out=/tmp/p8out/$name
mkdir -p "$out/bin" "$out/static"
for f in browser wpt-ready-render libbrowser.so; do cp build/ws074-host/plain/$f "$out/bin/$f"; done
python3 plan/ws074/tests/interop2025-chrome-gap.py measure --kind ours --out "$out" --build "$out/bin" --jobs "$jobs" --only harness,scripted,crash
python3 plan/ws074/tests/static-subset.py --jobs "$jobs" --out "$out/static-shards" --program "$out/bin/browser" / > "$out/static-subset.log" 2>&1 || true
python3 - "$out" <<'P'
import json, glob, sys
out = sys.argv[1]
results = []
for f in sorted(glob.glob(out + "/static-shards/s*/report.json")):
    results += json.load(open(f))["results"]
json.dump({"results": results}, open(out + "/static/report.json", "w"))
print("static: merged", len(results))
P
python3 plan/ws074/tests/interop2025-chrome-gap.py compare --chrome "${CHROME:-build/ws074-cmp/chrome156}" --ours "$out" --write "$out/gap.json"

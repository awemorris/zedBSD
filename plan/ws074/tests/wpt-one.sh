#!/bin/sh
# ws074: runs one fixed Interop 2025 testharness ID with browser and prints its outcome and callbacks.
#
#   sh plan/ws074/tests/wpt-one.sh /navigation-api/navigate-event/foo.html [more IDs]
#
# BROWSER (default build/ws074-host/plain/browser) selects the program.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
program=${BROWSER:-build/ws074-host/plain/browser}
out=$(mktemp -d build/tmp/wpt-one.XXXXXX)
n=0
for id in "$@"; do
	n=$((n + 1))
	python3 plan/ws074/tests/run-interop2025-harness.py --wpt build/ws074-suites/wpt \
	    --manifest plan/ws074/phase173/evidence/20261013-q1012/manifest.json --program "$program" --test-id "$id" \
	    --out "$out/$n" --settle-ms 5000 --process-timeout 20 --font-dir userland/desktop/fonts > "$out/$n.log" 2>&1 || true
	python3 - "$out/$n" "$id" <<'P'
import json, pathlib, sys
f = pathlib.Path(sys.argv[1])
if not f.is_file():
    print(sys.argv[2], "no result file")
    raise SystemExit
j = json.loads(f.read_text())
print(sys.argv[2], j["outcome"])
for r in j.get("result_callbacks", []):
    print("  %s %s: %s" % ("PASS" if r["status"] == 0 else "FAIL", r["name"], (r.get("message") or "")[:200]))
if j.get("stderr"):
    print("  stderr:", j["stderr"][:300])
P
done

#!/bin/sh
# Runs single scripted reftest IDs with the host build and prints the outcome.
#   sh plan/ws074/phase260/tools/s1.sh /css/css-anchor-position/foo.html ...
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
cd "$(dirname -- "$0")/../../../.."
mkdir -p build/tmp/p8/s1out
ids=$(echo "$@" | tr ' ' ',')
timeout 300 python3 plan/ws074/tests/run-interop2025-scripted-reftests.py --manifest plan/ws074/phase173/evidence/20261013-q1012/manifest.json \
    --program build/ws074-host/plain/wpt-ready-render --out build/tmp/p8/s1out --only "$ids" > build/tmp/p8/s1out.log 2>&1
python3 - <<'P'
import json,glob
d=json.load(open("build/tmp/p8/s1out/report.json"))
for r in d["results"]:
    print(r.get("test_id"), r.get("outcome"))
P

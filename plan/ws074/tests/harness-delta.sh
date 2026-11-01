#!/bin/sh
# ws074: measures only the harness runner with the current host build (a snapshot under OUT) and lists the changes against a gap JSON.
#
#   sh plan/ws074/tests/harness-delta.sh GAP.json OUTNAME [JOBS]
#
# The other runners' results are taken from DEV (default build/tmp/p8out/dev149) by links, so the totals compare.
# HOSTOUT (default build/ws074-host2/plain) is the host build; OUTROOT (default build/tmp/p8out) is where OUTNAME is made.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
gap=$1
name=$2
jobs=${3:-24}
root=${OUTROOT:-build/tmp/p8out}
out=$root/$name
dev=${DEV:-$root/dev149}
mkdir -p "$out/bin"
for f in browser wpt-ready-render libbrowser.so; do cp "${HOSTOUT:-build/ws074-host2/plain}/$f" "$out/bin/$f"; done
python3 plan/ws074/tests/interop2025-chrome-gap.py measure --kind ours --out "$out" --build "$out/bin" --jobs "$jobs" --only harness > "$out/measure.log" 2>&1
for f in scripted static crash.json runs.json; do [ -e "$out/$f" ] || ln -s "$(cd "$dev" && pwd)/$f" "$out/$f"; done
python3 plan/ws074/tests/interop2025-chrome-gap.py compare --chrome build/ws074-cmp/chrome156 --ours "$out" --write "$out/gap.json" | tail -6
python3 plan/ws074/tests/gap-diff.py "$gap" "$out/gap.json"

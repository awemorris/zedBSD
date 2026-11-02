#!/bin/sh
# Makes the H.265 streams of the real-machine check from the HEVC_v1 conformance cache: each zip unpacked in a
# directory of its own (OUT/x/NAME), its bitstream copied to OUT/NAME.h265, the reference hashes by ffmpeg
# (-f framehash -hash sha256 -pix_fmt nv12, one hash a line, in output order) to OUT/NAME.sha256, and one line of
# the stream's first SPS and PPS (profile, size, tools) to OUT/features.txt (h265-features.py).  Main 10 and
# 4:4:4 streams are kept but marked (the decoder takes Main, 8 bit, 4:2:0 only).
#   usage: plan/ws324/tests/h265-conformance-prep.sh CACHE OUT   (CACHE: build/sources/h265-conformance)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
CACHE=${1:?usage: h265-conformance-prep.sh CACHE OUT}
OUT=${2:?usage: h265-conformance-prep.sh CACHE OUT}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT/x"
: > "$OUT/features.txt"
for zip in "$CACHE"/*.zip; do
	name=$(basename "$zip" .zip)
	mkdir -p "$OUT/x/$name"
	timeout 120 unzip -q -o "$zip" -d "$OUT/x/$name" || { echo "$name: unzip failed"; continue; }
	stream=$(find "$OUT/x/$name" -type f \( -name '*.bit' -o -name '*.bin' -o -name '*.hevc' -o -name '*.265' \) | head -1)
	[ -n "$stream" ] || { echo "$name: no bitstream"; continue; }
	cp "$stream" "$OUT/$name.h265"
	timeout 300 ffmpeg -v error -i "$OUT/$name.h265" -pix_fmt nv12 -f framehash -hash sha256 - 2> "$OUT/$name.ffmpeg.err" |
		awk -F', *' '!/^#/ && NF >= 6 { sub(/^SHA256=/, "", $6); print $6 }' > "$OUT/$name.sha256"
	timeout 120 ffmpeg -v trace -i "$OUT/$name.h265" -c copy -bsf:v trace_headers -f null - 2>&1 |
		/usr/bin/python3 -I "$here/h265-features.py" "$name" "$(wc -l < "$OUT/$name.sha256")" >> "$OUT/features.txt"
done
sync
echo "done: $(ls "$OUT"/*.h265 | wc -l) streams in $OUT"

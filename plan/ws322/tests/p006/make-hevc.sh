#!/bin/sh
# Makes the HEVC_v1 conformance bitstreams (plan/tools/h265/conformance/list.txt, every class) ready for vkvideo-probe
# on the board (ws322-p006): OUT/<name>.h265 is the zip's bitstream (unzip -p) and OUT/<name>.sha256 the SHA-256 of
# each frame of the host's ffmpeg's decode as NV12, in output order (ffmpeg -f framehash, the form vkvideo-probe
# --expect reads).  A stream ffmpeg cannot read because a PPS comes before its SPS is given to ffmpeg as the copy of
# plan/ws324/tests/conformance/reorder-ps.py (the board's probe gets the zip's bitstream).  OUT/list.txt has each
# name, its class and its frames; a stream that could not be made goes to OUT/skipped.txt with why.
#   sh plan/ws322/tests/p006/make-hevc.sh OUT
# The cache is $H265SW_CONFORMANCE_DIR (default /home/awe/zedBSD-claude1/build/sources/h265-conformance, read only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: make-hevc.sh OUT}
top=$(cd -- "$(dirname -- "$0")/../../../.." && pwd)
cache=${H265SW_CONFORMANCE_DIR:-/home/awe/zedBSD-claude1/build/sources/h265-conformance}
list=$top/plan/tools/h265/conformance/list.txt
mkdir -p "$out"
: > "$out/list.txt"
: > "$out/skipped.txt"
ff="timeout 900 ffmpeg -hide_banner -loglevel error -nostdin -threads 1 -f hevc"
grep -v '^#' "$list" | while IFS='	' read -r name bytes class; do
	zip=$cache/$name.zip
	member=$(unzip -Z1 "$zip" | grep -v '__MACOSX\|/\._\|^\._' | grep -i '\.\(bit\|bin\)$' | head -1)
	if [ -z "$member" ]; then
		echo "$name no bitstream in the zip" >> "$out/skipped.txt"
		continue
	fi
	unzip -p "$zip" "$member" > "$out/$name.h265"
	$ff -i "$out/$name.h265" -f framehash -hash sha256 -pix_fmt nv12 - 2> "$out/$name.fferr" |
		grep -v '^#' | awk -F', *' '{ print $6 }' > "$out/$name.sha256"
	if [ ! -s "$out/$name.sha256" ]; then
		/usr/bin/python3 -I "$top/plan/ws324/tests/conformance/reorder-ps.py" < "$out/$name.h265" |
			$ff -i pipe:0 -f framehash -hash sha256 -pix_fmt nv12 - 2> "$out/$name.fferr" |
			grep -v '^#' | awk -F', *' '{ print $6 }' > "$out/$name.sha256"
		echo "$name ffmpeg read the reordered copy" >> "$out/skipped.txt"
	fi
	if [ ! -s "$out/$name.sha256" ]; then
		echo "$name ffmpeg gave no frame" >> "$out/skipped.txt"
		continue
	fi
	printf '%s\t%s\t%s\n' "$name" "$class" "$(wc -l < "$out/$name.sha256")" >> "$out/list.txt"
done
echo "$(wc -l < "$out/list.txt") streams, $(du -sk "$out" | cut -f1) KiB"
sync

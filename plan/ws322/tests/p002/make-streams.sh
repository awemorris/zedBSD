#!/bin/sh
# Makes the Wave511 probe's streams and their reference hashes in OUT (ws322-p002): three H.264 streams of
# plan/ws083/tests/streams, an H.265 stream with B pictures at 352x288 and an H.264 and an H.265 stream at
# 1920x1080, all made with the host's ffmpeg (libx264, libx265) from its test source; then each stream decoded by
# the host's ffmpeg to NV12 in display order and every frame's FNV-1a written to OUT/NAME.ref, one hex per line.
#   sh plan/ws322/tests/p002/make-streams.sh OUT
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
out=${1:?usage: make-streams.sh OUT}
here=$(dirname "$0")
top=$here/../../../..
mkdir -p "$out"
cp "$top/plan/ws083/tests/streams/pb-high-352-pyramid.h264" "$out/h264-pyramid.h264"
cp "$top/plan/ws083/tests/streams/pb-main-352.h264" "$out/h264-main.h264"
cp "$top/plan/ws083/tests/streams/i-main-352-slices.h264" "$out/h264-slices.h264"
ff="timeout 300 ffmpeg -hide_banner -loglevel error -y"
$ff -f lavfi -i testsrc2=size=352x288:rate=25 -frames:v 15 -pix_fmt yuv420p -c:v libx265 \
	-x265-params bframes=3:b-pyramid=1:keyint=30:min-keyint=30:open-gop=0:repeat-headers=1:log-level=error \
	-f hevc "$out/h265-b.h265"
$ff -f lavfi -i testsrc2=size=1920x1080:rate=30 -frames:v 10 -pix_fmt yuv420p -c:v libx264 -profile:v high \
	-bf 2 -g 30 -f h264 "$out/h264-1080.h264"
$ff -f lavfi -i testsrc2=size=1920x1080:rate=30 -frames:v 10 -pix_fmt yuv420p -c:v libx265 \
	-x265-params bframes=2:keyint=30:min-keyint=30:open-gop=0:repeat-headers=1:log-level=error \
	-f hevc "$out/h265-1080.h265"
for s in h264-pyramid.h264 h264-main.h264 h264-slices.h264 h265-b.h265 h264-1080.h264 h265-1080.h265; do
	name=${s%.*}
	size=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x "$out/$s")
	$ff -i "$out/$s" -f rawvideo -pix_fmt nv12 "$out/$name.nv12"
	/usr/bin/python3 -I "$here/fnv-frames.py" "$out/$name.nv12" "$size" > "$out/$name.ref"
	echo "$name $size $(wc -l < "$out/$name.ref") frames"
done

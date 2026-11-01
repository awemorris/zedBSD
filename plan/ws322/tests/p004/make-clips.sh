#!/bin/sh
# Makes the board check's streams, clip and references in OUT (ws322-p004): the ws083 H.264 streams with their
# references (plan/ws083/tests/streams: 64x64 Baseline, 352x288 Main, High with B pyramids, slices and scaling
# matrices), the Wave511 probe's 1920x1080 High stream of 10 frames (plan/ws322/tests/p002/make-streams.sh) with the
# host's ffmpeg's per-frame SHA-256 of its NV12 decode (the ws083 form vkvideo-probe --expect reads), and a 1920x1080
# High MP4 clip of 30 frames a second, 20 s, with B pictures and grain at 8 Mbit/s (its bytes go round the decoder's
# 4 MiB ring several times) with media-probe --video-hash's reference (plan/tools/rtx-video/nv12-hashes.py).
#   sh plan/ws322/tests/p004/make-clips.sh OUT
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
out=${1:?usage: make-clips.sh OUT}
here=$(dirname "$0")
top=$here/../../../..
mkdir -p "$out/streams" "$out/p002"
ff="timeout 600 ffmpeg -hide_banner -loglevel error -y"
cp "$top"/plan/ws083/tests/streams/*.h264 "$top"/plan/ws083/tests/streams/*.sha256 "$out/streams/"
sh "$top/plan/ws322/tests/p002/make-streams.sh" "$out/p002" > "$out/p002.txt"
cp "$out/p002/h264-1080.h264" "$out/streams/"
$ff -i "$out/streams/h264-1080.h264" -f framehash -hash sha256 -pix_fmt nv12 - | grep -v '^#' | awk -F', *' '{ print $6 }' \
	> "$out/streams/h264-1080.sha256"
$ff -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 20 -vf noise=alls=12:allf=t -pix_fmt yuv420p -c:v libx264 \
	-profile:v high -bf 2 -g 60 -b:v 8M -maxrate 8M -bufsize 16M "$out/h264-1080p30.mp4"
/usr/bin/python3 -I "$top/plan/tools/rtx-video/nv12-hashes.py" "$out/h264-1080p30.mp4" > "$out/h264-1080p30.ref"
for stream in "$out"/streams/*.h264; do
	echo "$(basename "$stream") $(wc -l < "${stream%.h264}.sha256") frames $(wc -c < "$stream") bytes"
done
echo "h264-1080p30.mp4 $(wc -l < "$out/h264-1080p30.ref") frames $(wc -c < "$out/h264-1080p30.mp4") bytes"

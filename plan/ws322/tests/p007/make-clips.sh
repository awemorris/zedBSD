#!/bin/sh
# Makes ws322-p007's streams and references in OUT, beside p004's (plan/ws322/tests/p004/make-clips.sh, whose OUT is
# P004 here): the p004 1080p30 clip's stream as raw H.264 (20 s, an IDR picture every 60, the stream a reset at each
# IDR picture is tried on), a 1920x1080 High clip of 60 frames a second (20 s, B pictures, grain, 12 Mbit/s) as MP4
# with media-probe's reference and as raw H.264, and a 3840x2160 High stream of 30 frames a second (3 s, 20 Mbit/s,
# level 5.1); each raw stream with the host's ffmpeg's per-frame SHA-256 of its NV12 decode (vkvideo-probe --expect).
#   sh plan/ws322/tests/p007/make-clips.sh OUT P004
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
out=${1:?usage: make-clips.sh OUT P004}
p004=${2:?the p004 clips}
top=$(dirname "$0")/../../../..
mkdir -p "$out"
ff="timeout 1200 ffmpeg -hide_banner -loglevel error -y"
hashes() { $ff -i "$1" -f framehash -hash sha256 -pix_fmt nv12 - | grep -v '^#' | awk -F', *' '{ print $6 }' > "$2"; }
$ff -i "$p004/h264-1080p30.mp4" -c:v copy -bsf:v h264_mp4toannexb -f h264 "$out/h264-1080p30.h264"
hashes "$out/h264-1080p30.h264" "$out/h264-1080p30.sha256"
$ff -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 20 -vf noise=alls=12:allf=t -pix_fmt yuv420p -c:v libx264 \
	-profile:v high -bf 2 -g 120 -b:v 12M -maxrate 12M -bufsize 24M "$out/h264-1080p60.mp4"
/usr/bin/python3 -I "$top/plan/tools/rtx-video/nv12-hashes.py" "$out/h264-1080p60.mp4" > "$out/h264-1080p60.ref"
$ff -i "$out/h264-1080p60.mp4" -c:v copy -bsf:v h264_mp4toannexb -f h264 "$out/h264-1080p60.h264"
hashes "$out/h264-1080p60.h264" "$out/h264-1080p60.sha256"
$ff -f lavfi -i testsrc2=size=3840x2160:rate=30 -t 3 -vf noise=alls=12:allf=t -pix_fmt yuv420p -c:v libx264 \
	-profile:v high -level 5.1 -bf 2 -g 30 -b:v 20M -maxrate 20M -bufsize 40M -f h264 "$out/h264-2160p30.h264"
hashes "$out/h264-2160p30.h264" "$out/h264-2160p30.sha256"
for stream in "$out"/*.h264; do
	echo "$(basename "$stream") $(wc -l < "${stream%.h264}.sha256") frames $(wc -c < "$stream") bytes"
done
echo "h264-1080p60.mp4 $(wc -l < "$out/h264-1080p60.ref") frames $(wc -c < "$out/h264-1080p60.mp4") bytes"

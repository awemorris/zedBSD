#!/bin/sh
# Makes the clips of WS319's playback checks on the Latitude 5330 in OUT/mp4: H.264 (libx264, High, B pictures) and
# H.265 (libx265, Main, B pictures) MP4s of testsrc2 with grain at 1920x1080, 30 frames a second for 20 s and 60 for
# 10 s, each with media-probe --video-hash's reference (plan/tools/rtx-video/nv12-hashes.py, the frames' NV12 SHA-256
# in display order).
#   sh plan/ws319/tests/make-clips.sh OUT
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
out=${1:?usage: make-clips.sh OUT}
top=$(cd -- "$(dirname -- "$0")/../../.." && pwd)
mkdir -p "$out/mp4"
ff="timeout 900 ffmpeg -hide_banner -loglevel error -nostdin -y"
x264="-c:v libx264 -preset fast -profile:v high -bf 3 -refs 3 -g 60 -keyint_min 60 -sc_threshold 0"
x265="-c:v libx265 -preset fast -x265-params log-level=error:bframes=3:ref=3:keyint=60:min-keyint=60:scenecut=0 -tag:v hvc1"
src() { echo "-f lavfi -i testsrc2=size=1920x1080:rate=$1 -t $2 -vf noise=alls=12:allf=t -pix_fmt yuv420p"; }
$ff $(src 30 20) $x264 -b:v 8M "$out/mp4/h264-1080p30.mp4"
$ff $(src 60 10) $x264 -b:v 12M "$out/mp4/h264-1080p60.mp4"
$ff $(src 30 20) $x265 -b:v 8M "$out/mp4/h265-1080p30.mp4"
$ff $(src 60 10) $x265 -b:v 12M "$out/mp4/h265-1080p60.mp4"
for clip in h264-1080p30 h264-1080p60 h265-1080p30 h265-1080p60; do
	/usr/bin/python3 -I "$top/plan/tools/rtx-video/nv12-hashes.py" "$out/mp4/$clip.mp4" > "$out/mp4/$clip.ref"
done
sync
wc -l "$out"/mp4/*.ref

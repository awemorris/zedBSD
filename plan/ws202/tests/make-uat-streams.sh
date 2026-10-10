#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Prepare source-owned, synthetic physical-playback inputs; no prior build output is an image input.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws202-uat-streams"
work=$fresh_dir
out=$repo/plan/ws202/tests/streams
mkdir -p "$out"
streams=$(sh plan/ws202/tests/make-streams.sh)
for name in h264-high-b-aac.mp4 h264-high-b.video.sha256 h264-high-b-aac.rms h264-nocts.mp4 h264-nocts.video.sha256 aac-stereo.m4a; do
    cp "$streams/$name" "$out/$name"
done

# Twelve seconds leaves time for pause, full screen and seek before ordinary end-of-file.
ffmpeg -nostdin -v error -f lavfi -i 'testsrc2=size=640x360:rate=25:duration=12' \
    -f lavfi -i 'sine=frequency=997:sample_rate=48000:duration=12' \
    -fflags +bitexact -flags:v +bitexact -flags:a +bitexact -map_metadata -1 \
    -c:v libx264 -profile:v high -pix_fmt yuv420p \
    -x264-params 'bframes=3:b-pyramid=normal:ref=4:open-gop=1:keyint=30:min-keyint=30:scenecut=0:threads=1' \
    -c:a aac -profile:a aac_low -movflags +faststart -shortest "$work/h264-uat.mp4"
ffmpeg -nostdin -v error -i "$work/h264-uat.mp4" -an -c:v copy -bsf:v h264_mp4toannexb -f h264 "$work/h264-uat.h264"
ffmpeg -nostdin -v error -i "$work/h264-uat.mp4" -an -pix_fmt nv12 -f rawvideo "$work/h264-uat.nv12"
ffprobe -v error -select_streams v -show_entries frame=pts_time -of json "$work/h264-uat.mp4" > "$work/h264-uat-frames.json"
ffmpeg -nostdin -v error -i "$work/h264-uat.mp4" -vn -t 12 -af 'pan=stereo|c0=c0|c1=c0' -ar 48000 -ac 2 -f s16le "$work/h264-uat.s16"
python3 "$repo/plan/ws202/tests/make-reference.py" "$work" --name=h264-uat --width=640 --height=360 --audio-name=h264-uat --nocts-name=
for name in h264-uat.mp4 h264-uat.h264 h264-uat.video.sha256 h264-uat.rms; do
    cp "$work/$name" "$out/$name"
done
ffmpeg -version > "$out/encoder-version.txt"
sha256sum "$out"/*.mp4 "$out"/*.m4a "$out"/*.h264 "$out"/*.sha256 "$out"/*.rms > "$work/SHA256SUMS"
sed "s|$out/||" "$work/SHA256SUMS" > "$out/SHA256SUMS"
printf '%s\n' "$out"

#!/bin/sh
# ws177-p027〜p031 (案 T): makes the files T1 copies into the guest to play the new containers with Video Player:
# frag.mp4 (fragmented MP4, H.264 and AAC), sample.ts (MPEG-TS, H.264 and AAC), sample.ogv (Ogg, Theora and Vorbis),
# sample.avi (AVI, MPEG-4 Part 2 and MP3), each 640x360 at 25 fps and 6 s, made with the host's ffmpeg into a new
# folder (nothing is removed; Q1's plan/tools/q1-clean.sh removes old runs).  Prints the folder.
#   sh plan/ws177/tests/make-media-t-samples.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
fresh_out build/ws177-media-t-samples
out=$fresh_dir
src="-f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 6"
ffmpeg -loglevel error -y $src -c:v libx264 -g 25 -pix_fmt yuv420p -c:a aac \
    -movflags frag_keyframe+empty_moov+default_base_moof "$out/frag.mp4"
ffmpeg -loglevel error -y $src -c:v libx264 -g 25 -pix_fmt yuv420p -c:a aac -f mpegts "$out/sample.ts"
ffmpeg -loglevel error -y $src -c:v libtheora -g 25 -c:a libvorbis "$out/sample.ogv"
ffmpeg -loglevel error -y $src -c:v mpeg4 -g 25 -c:a libmp3lame "$out/sample.avi"
echo "$out"

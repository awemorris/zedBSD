#!/bin/sh
# Makes the clips of ws322-p006's playback check in OUT: H.265 MP4s of testsrc2 with grain made by the host's ffmpeg
# (libx265, Main, B pictures, three references, an IDR every 60 frames, tag hvc1): 1920x1080 at 30 frames a second
# for 20 s (8 Mbit/s) and at 60 for 10 s, with media-probe --video-hash's references (plan/tools/rtx-video/
# nv12-hashes.py); two HEVC_v1 conformance streams of HEVC (make-hevc.sh) in MP4s of 25 frames a second with their
# references; JVT H.264 streams of JVT (p007's make-jvt.sh) in MP4s of 25 frames a second; and Annex B H.265 streams
# for vkvideo-probe with ffmpeg's NV12 SHA-256 a frame: x265 pictures 1016x760 and 1000x562 (widths that are not a
# multiple of 16) and a stream of three sequences of other sizes (416x240, 832x480, 1280x720, each from an IDR picture).
#   sh plan/ws322/tests/p006/make-play.sh OUT HEVC JVT
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
out=${1:?usage: make-play.sh OUT HEVC JVT}
hevc=${2:?the HEVC streams}
jvt=${3:?the JVT streams}
top=$(cd -- "$(dirname -- "$0")/../../../.." && pwd)
mkdir -p "$out/mp4" "$out/raw"
ff="timeout 900 ffmpeg -hide_banner -loglevel error -nostdin -y"
x265="-c:v libx265 -preset fast -x265-params log-level=error:bframes=3:ref=3:keyint=60:min-keyint=60:scenecut=0 -tag:v hvc1"
hashes() { /usr/bin/python3 -I "$top/plan/tools/rtx-video/nv12-hashes.py" "$1" > "$2"; }
framehash() { $ff -f hevc -i "$1" -f framehash -hash sha256 -pix_fmt nv12 - | grep -v '^#' | awk -F', *' '{ print $6 }' > "$2"; }

# The made H.265 clips.
$ff -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 20 -vf noise=alls=12:allf=t -pix_fmt yuv420p $x265 -b:v 8M "$out/mp4/h265-1080p30.mp4"
$ff -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 10 -vf noise=alls=12:allf=t -pix_fmt yuv420p $x265 -b:v 12M "$out/mp4/h265-1080p60.mp4"
for clip in h265-1080p30 h265-1080p60; do
	hashes "$out/mp4/$clip.mp4" "$out/mp4/$clip.ref"
done

# Conformance streams and JVT streams in MP4s; their references are their streams' frame hashes, in order.
for n in DSLICE_A_HHI_5 RAP_B_Bossen_2; do
	$ff -r 25 -f hevc -i "$hevc/$n.h265" -c copy -tag:v hvc1 "$out/mp4/$n.mp4"
	hashes "$out/mp4/$n.mp4" "$out/mp4/$n.ref"
done
for n in CAPM3_Sony_D CAWP1_TOSHIBA_E MR2_TANDBERG_E CVSE3_Sony_H; do
	$ff -r 25 -i "$jvt/$n.h264" -c copy "$out/mp4/$n.mp4"
	hashes "$out/mp4/$n.mp4" "$out/mp4/$n.ref"
done

# Annex B streams for vkvideo-probe: two widths not a multiple of 16, and a stream of three sizes.
$ff -f lavfi -i testsrc2=size=1016x760:rate=30 -t 2 -pix_fmt yuv420p $x265 -f hevc "$out/raw/odd-1016x760.h265"
$ff -f lavfi -i testsrc2=size=1000x562:rate=30 -t 2 -pix_fmt yuv420p $x265 -f hevc "$out/raw/odd-1000x562.h265"
: > "$out/raw/sizes.h265"
for size in 416x240 832x480 1280x720; do
	$ff -f lavfi -i testsrc2=size=$size:rate=30 -t 1 -pix_fmt yuv420p $x265 -f hevc "$out/raw/part-$size.h265"
	cat "$out/raw/part-$size.h265" >> "$out/raw/sizes.h265"
done
for n in odd-1016x760 odd-1000x562; do
	framehash "$out/raw/$n.h265" "$out/raw/$n.sha256"
done

# The stream of three sizes: each part's own hashes, in order (ffmpeg scales the pictures of a later size to the
# first one's when it decodes the stream whole).
: > "$out/raw/sizes.sha256"
for size in 416x240 832x480 1280x720; do
	framehash "$out/raw/part-$size.h265" "$out/raw/part-$size.sha256"
	cat "$out/raw/part-$size.sha256" >> "$out/raw/sizes.sha256"
done
for f in "$out"/mp4/*.ref "$out"/raw/*.sha256; do
	echo "$(basename "$f") $(grep -c . "$f") frames"
done
sync

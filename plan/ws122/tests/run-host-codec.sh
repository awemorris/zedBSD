#!/bin/sh
# ws122-p004: the host tests of the player's decoding add-in.
#  1. host-layout.c: the add-in's view of AVPacket's, AVFrame's, AVCodecParameters' and AVCodec's first fields
#     (userland/desktop/libmedia/avcodec-layout.h) against FFmpeg's
#     headers of each version the add-in knows: the host's (Debian 13, libavcodec-dev, major 61) and the image's
#     package (FFmpeg 9.0.2, major 63, its staged headers; LIBAVCODEC_STAGE names them, default the main checkout's
#     build/packages/libavcodec/stage).  Skipped (with a line) when a set of headers is not there.
#  2. host-codec.c: with mediafile and libmedia's decoders (ASan and UBSan), against the host's FFmpeg 7 by dlopen,
#     every file: the player's sample.mp4 (MPEG-4 Part 2 and AAC) and files made here with the host's ffmpeg (H.264 and
#     AAC in MP4 and Matroska, H.265 in MP4, VP9 and Opus in WebM, Theora and Vorbis in Ogg, MJPEG and PCM in AVI,
#     H.264 and MP3 in MPEG-TS, 4 s each).
# Last line: run-host-codec: PASS.
#   sh plan/ws122/tests/run-host-codec.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
out=build/ws122-host-codec
stage=${LIBAVCODEC_STAGE:-/home/awe/zedBSD-claude1/build/packages/libavcodec/stage/usr/include}
. plan/tools/fresh-out.sh
fresh_out "$out"
mkdir -p "$out/media"
status=0

# 1. The layouts.
if [ -f /usr/include/x86_64-linux-gnu/libavcodec/avcodec.h ]; then
	cc -I. -I/usr/include/x86_64-linux-gnu plan/ws122/tests/host-layout.c -o "$out/layout-host" && "$out/layout-host" 61 | tail -1 || status=1
else
	echo "layout 61: skipped (libavcodec-dev is not installed)"
fi
if [ -f "$stage/libavcodec/avcodec.h" ]; then
	cc -I. -isystem "$stage" plan/ws122/tests/host-layout.c -o "$out/layout-image" && "$out/layout-image" 63 | tail -1 || status=1
else
	echo "layout 63: skipped (no staged FFmpeg 9.0.2 headers at $stage)"
fi

# 2. The decoding.
cc -std=gnu99 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -I. \
    plan/ws122/tests/host-codec.c userland/desktop/libmedia/decoder.c userland/desktop/libmedia/avcodec.c userland/desktop/libmedia/bitstream.c \
    userland/desktop/mediafile/mediafile.c userland/desktop/mediafile/mp4.c userland/desktop/mediafile/mkv.c userland/desktop/mediafile/ts.c userland/desktop/mediafile/ogg.c userland/desktop/mediafile/avi.c \
    -ldl -lpthread -o "$out/host-codec" || { echo "run-host-codec: FAIL (build)"; exit 1; }
files=plan/ws122/tests/sample.mp4
if command -v ffmpeg > /dev/null 2>&1; then
	src="-f lavfi -i testsrc2=size=320x180:rate=25 -f lavfi -i sine=frequency=440:sample_rate=44100 -t 4"
	ffmpeg -loglevel error -y $src -c:v libx264 -bf 2 -g 25 -c:a aac "$out/media/h264-aac.mp4" && files="$files $out/media/h264-aac.mp4"
	ffmpeg -loglevel error -y $src -c:v libx264 -bf 2 -g 25 -c:a aac "$out/media/h264-aac.mkv" && files="$files $out/media/h264-aac.mkv"
	ffmpeg -loglevel error -y $src -c:v libx265 -x265-params log-level=error -g 25 -tag:v hvc1 -c:a aac "$out/media/hevc-aac.mp4" && files="$files $out/media/hevc-aac.mp4"
	ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=320x180:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 4 \
	    -c:v libvpx-vp9 -g 25 -c:a libopus "$out/media/vp9-opus.webm" && files="$files $out/media/vp9-opus.webm"
	# ws177-p031: the readers of p028-p030 and the decoders that need extradata (Vorbis, Theora) or a name (PCM).
	ffmpeg -loglevel error -y $src -c:v libtheora -g 25 -c:a libvorbis "$out/media/theora-vorbis.ogv" && files="$files $out/media/theora-vorbis.ogv"
	ffmpeg -loglevel error -y $src -c:v mjpeg -c:a pcm_s16le "$out/media/mjpeg-pcm.avi" && files="$files $out/media/mjpeg-pcm.avi"
	ffmpeg -loglevel error -y $src -c:v libx264 -bf 2 -g 25 -c:a libmp3lame "$out/media/h264-mp3.ts" && files="$files $out/media/h264-mp3.ts"
else
	echo "made files: skipped (no ffmpeg on the host)"
fi
for file in $files; do
	if UBSAN_OPTIONS=halt_on_error=1 timeout 120 "$out/host-codec" "$file" > "$out/$(basename "$file").log" 2>&1; then
		tail -1 "$out/$(basename "$file").log"
	else
		echo "FAILED $file:"
		grep -v '^ok ' "$out/$(basename "$file").log" | head -20
		status=1
	fi
done

if [ "$status" = 0 ]; then echo "run-host-codec: PASS"; else echo "run-host-codec: FAIL"; fi
exit "$status"

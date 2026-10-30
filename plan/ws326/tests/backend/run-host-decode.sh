#!/bin/sh
# Decodes H.264 in MP4, Matroska and MPEG-TS (made here with the host's ffmpeg: B-frames, CABAC and CAVLC, an
# open-GOP-free and a multi-slice file) through libmedia's decoder layer and its software back end, and compares the
# pictures with ffmpeg's own decoding: number of pictures, and the MD5 of the whole output, of the first and of the last
# picture.  ASan and UBSan.  It needs the decoder (h264sw-decoder.c) to be implemented.
# Last line: run-host-decode: PASS or FAIL.
#   sh plan/ws326/tests/backend/run-host-decode.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws326-host-decode
fresh_out "$out"

lib=userland/desktop/libmedia
if ! cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -I. \
    plan/ws326/tests/backend/host-decode.c $lib/decoder.c $lib/bitstream.c $lib/picture.c $lib/bits.c $lib/h264sw-*.c \
    userland/desktop/mediafile/mediafile.c userland/desktop/mediafile/mp4.c userland/desktop/mediafile/mkv.c \
    userland/desktop/mediafile/ts.c userland/desktop/mediafile/ogg.c userland/desktop/mediafile/avi.c \
    -lpthread -lm -o "$out/host-decode" > "$out/build.log" 2>&1; then
	echo "run-host-decode: FAIL (build)"
	head -20 "$out/build.log"
	exit 1
fi
if ! command -v ffmpeg > /dev/null 2>&1; then
	echo "run-host-decode: FAIL (no ffmpeg)"
	exit 1
fi

status=0
size=320x180
frame_bytes=$((320 * 180 * 3 / 2))
src="-f lavfi -i testsrc2=size=$size:rate=25 -t 2"
# name:extension:encoder options
for spec in "cabac-bf:mp4:-c:v libx264 -bf 2 -g 25" "cavlc-bf:mkv:-c:v libx264 -bf 2 -g 25 -coder 0 -profile:v main" \
    "baseline:mp4:-c:v libx264 -profile:v baseline -g 25" "ts-bf:ts:-c:v libx264 -bf 2 -g 25" \
    "slices:mp4:-c:v libx264 -bf 1 -g 25 -x264-params slices=4"; do
	name=${spec%%:*}
	rest=${spec#*:}
	ext=${rest%%:*}
	options=${rest#*:}
	file="$out/$name.$ext"
	# shellcheck disable=SC2086
	if ! ffmpeg -loglevel error -y $src $options -an "$file"; then
		echo "$name: FAIL (ffmpeg could not make the file)"
		status=1
		continue
	fi
	ffmpeg -loglevel error -y -i "$file" -an -fps_mode passthrough -pix_fmt yuv420p -f rawvideo "$out/$name.ref.yuv" || { status=1; continue; }
	if ! ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 300 "$out/host-decode" "$file" "$out/$name.yuv" > "$out/$name.log" 2>&1; then
		echo "$name: FAIL (host-decode)"
		tail -5 "$out/$name.log"
		status=1
		continue
	fi
	refsize=$(wc -c < "$out/$name.ref.yuv")
	oursize=$(wc -c < "$out/$name.yuv")
	refframes=$((refsize / frame_bytes))
	ourframes=$((oursize / frame_bytes))
	whole_ref=$(md5sum < "$out/$name.ref.yuv" | cut -d' ' -f1)
	whole=$(md5sum < "$out/$name.yuv" | cut -d' ' -f1)
	first_ref=$(head -c "$frame_bytes" "$out/$name.ref.yuv" | md5sum | cut -d' ' -f1)
	first=$(head -c "$frame_bytes" "$out/$name.yuv" | md5sum | cut -d' ' -f1)
	last_ref=$(tail -c "$frame_bytes" "$out/$name.ref.yuv" | md5sum | cut -d' ' -f1)
	last=$(tail -c "$frame_bytes" "$out/$name.yuv" | md5sum | cut -d' ' -f1)
	backend=$(sed -n 's/.*backend=\([^ ]*\).*/\1/p' "$out/$name.log" | tail -1)
	if [ "$refframes" = "$ourframes" ] && [ "$whole_ref" = "$whole" ] && [ "$first_ref" = "$first" ] && [ "$last_ref" = "$last" ] && [ "$backend" = "h264-software" ]; then
		echo "$name: PASS ($ourframes pictures, backend $backend, first $first, last $last)"
	else
		echo "$name: FAIL (pictures ref $refframes ours $ourframes, backend $backend, whole $whole_ref/$whole, first $first_ref/$first, last $last_ref/$last)"
		status=1
	fi
done

if [ "$status" = 0 ]; then echo "run-host-decode: PASS"; else echo "run-host-decode: FAIL"; fi
exit "$status"

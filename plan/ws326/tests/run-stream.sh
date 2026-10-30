#!/bin/sh
# The whole-stream test of the software H.264 decoder (libmedia's h264sw-*): decode-stream.c is built
# with every h264sw-*.c except the back end and bits.c, with ASan and UBSan, and
#  1. decodes each sample stream and compares the MD5 of every output frame (cropped, I420) with the
#     frames of the host's ffmpeg (the secondary oracle): the samples of plan/ws083/tests/streams and
#     plan/ws236/tests, and short 352x288 streams made here with ffmpeg's libx264 (CAVLC Baseline,
#     CABAC Main with B pictures, High with the 8x8 transform and a scaling matrix, several slices,
#     weighted prediction);
#  2. decodes 100 damaged copies of each of two streams (CABAC, CAVLC; fixed seed) and checks the recovery at
#     the next IDR picture;
#  3. starts a stream at each of its pictures, and once after a reset.
# Without ffmpeg the comparisons are skipped (said so) and the decoding still runs.
#   sh plan/ws326/tests/run-stream.sh
# Last line: run-stream: PASS or run-stream: FAIL.  Output under build/tmp/ws326-stream (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws326-stream
fresh_out "$out"
status=0

modules=""
for file in userland/desktop/libmedia/h264sw-*.c; do
	case "$file" in
	*/h264sw-backend.c) ;;
	*) modules="$modules $file" ;;
	esac
done

if ! cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I. plan/ws326/tests/stream/decode-stream.c $modules userland/desktop/libmedia/bits.c \
    -lm -o "$out/decode-stream" > "$out/build.log" 2>&1; then
	echo "decode-stream: FAIL (build)"
	head -20 "$out/build.log"
	echo "run-stream: FAIL"
	exit 1
fi

export ASAN_OPTIONS=detect_leaks=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

have_ffmpeg=0
if command -v ffmpeg > /dev/null 2>&1; then
	have_ffmpeg=1
fi

# Makes the short streams with libx264: name, then the ffmpeg arguments after the input.
make_stream() {
	name=$1
	shift
	if ! timeout 120 ffmpeg -v error -nostdin -f lavfi -i "$source_filter" -frames:v 24 -c:v libx264 "$@" \
	    -f h264 "$out/streams/$name.h264" > "$out/streams/$name.log" 2>&1; then
		echo "$name: FAIL (ffmpeg could not make it)"
		head -5 "$out/streams/$name.log"
		status=1
	fi
}

mkdir -p "$out/streams"
if [ "$have_ffmpeg" = 1 ]; then
	source_filter="testsrc2=size=352x288:rate=25,format=yuv420p"
	make_stream cavlc-baseline -profile:v baseline -x264-params keyint=12:ref=3
	make_stream cabac-main-b -profile:v main -x264-params keyint=24:bframes=3:b-pyramid=normal:ref=4:direct=temporal
	make_stream cabac-main-spatial -profile:v main -x264-params keyint=24:bframes=2:b-pyramid=none:ref=2:direct=spatial
	make_stream high-8x8-cqm -profile:v high -x264-params keyint=24:bframes=2:8x8dct=1:cqm=jvt
	make_stream high-cavlc-8x8 -profile:v high -x264-params keyint=24:bframes=2:8x8dct=1:cabac=0
	make_stream main-slices -profile:v main -x264-params keyint=24:bframes=2:slices=4
	make_stream main-qp45 -profile:v main -x264-params keyint=24:bframes=3:qp=45
	make_stream high-qp8 -profile:v high -x264-params keyint=24:bframes=1:qp=8
	make_stream open-gop -profile:v high -x264-params keyint=8:min-keyint=8:open-gop=1:bframes=2:scenecut=0
	make_stream repeat-headers -profile:v high -x264-params keyint=6:min-keyint=6:bframes=2:repeat-headers=1:scenecut=0
	make_stream repeat-headers-cavlc -profile:v main -x264-params keyint=6:min-keyint=6:bframes=2:repeat-headers=1:scenecut=0:cabac=0
	source_filter="testsrc2=size=352x288:rate=25,format=yuv420p,fade=in:0:12,fade=out:12:12"
	make_stream weighted -profile:v high -x264-params keyint=24:bframes=2:weightp=2:weightb=1
	make_stream weighted-cavlc -profile:v main -x264-params keyint=24:bframes=0:weightp=2:cabac=0
	source_filter="testsrc2=size=360x200:rate=25,format=yuv420p"
	make_stream crop-360x200 -profile:v main -x264-params keyint=24:bframes=2
else
	echo "ffmpeg: skipped (not installed): no streams made, no comparisons; the samples are only decoded"
fi

# 1. Each sample stream against ffmpeg.
streams_pass=0
frames_pass=0
for file in plan/ws083/tests/streams/*.h264 plan/ws236/tests/*.h264 "$out"/streams/*.h264; do
	[ -f "$file" ] || continue
	name=$(basename "$file" .h264)
	case "$file" in
	plan/ws236/*) name="ws236-$name" ;;
	esac
	if ! timeout 300 "$out/decode-stream" "$file" > "$out/$name.decode" 2>&1; then
		echo "$name: FAIL (decode; log $out/$name.decode)"
		grep -v '^frame ' "$out/$name.decode" | head -10
		status=1
		continue
	fi
	if [ "$have_ffmpeg" = 0 ]; then
		echo "$name: decoded, comparison skipped (no ffmpeg)"
		continue
	fi
	if ! timeout 300 ffmpeg -v error -nostdin -i "$file" -fps_mode passthrough -pix_fmt yuv420p -f framemd5 \
	    "$out/$name.framemd5" > "$out/$name.ffmpeg.log" 2>&1; then
		echo "$name: FAIL (ffmpeg could not decode it)"
		status=1
		continue
	fi
	grep -v '^#' "$out/$name.framemd5" | awk -F', *' '{ print $6 }' > "$out/$name.expected"
	awk '/^frame / { print $4 }' "$out/$name.decode" > "$out/$name.actual"
	count=$(wc -l < "$out/$name.expected")
	if cmp -s "$out/$name.expected" "$out/$name.actual"; then
		echo "$name: PASS ($count frames, $(grep '^stats' "$out/$name.decode"))"
		streams_pass=$((streams_pass + 1))
		frames_pass=$((frames_pass + count))
	else
		first=$(diff "$out/$name.expected" "$out/$name.actual" | head -1)
		echo "$name: FAIL (ffmpeg $count frames, ours $(wc -l < "$out/$name.actual"); first difference $first)"
		status=1
	fi
done
echo "streams: $streams_pass streams, $frames_pass frames equal to ffmpeg"

# 2. Damaged copies of the streams with an IDR picture every 6 pictures (CABAC and CAVLC).
for file in "$out/streams/repeat-headers.h264" "$out/streams/repeat-headers-cavlc.h264"; do
	[ -f "$file" ] || continue
	name=$(basename "$file" .h264)
	if timeout 900 "$out/decode-stream" --fuzz "$file" 20261031 100 > "$out/fuzz-$name.log" 2>&1; then
		echo "fuzz $name: $(tail -1 "$out/fuzz-$name.log")"
	else
		echo "fuzz $name: FAIL (log $out/fuzz-$name.log)"
		tail -20 "$out/fuzz-$name.log"
		status=1
	fi
done

# 3. Starts at each picture, and after a reset.
for file in "$out/streams/repeat-headers.h264" "$out/streams/open-gop.h264" plan/ws083/tests/streams/pb-main-352.h264; do
	[ -f "$file" ] || continue
	name=$(basename "$file" .h264)
	if timeout 600 "$out/decode-stream" --start "$file" > "$out/start-$name.log" 2>&1; then
		echo "start $name: $(tail -1 "$out/start-$name.log")"
	else
		echo "start $name: FAIL (log $out/start-$name.log)"
		tail -20 "$out/start-$name.log"
		status=1
	fi
done

if [ "$status" = 0 ]; then echo "run-stream: PASS"; else echo "run-stream: FAIL"; fi
exit "$status"

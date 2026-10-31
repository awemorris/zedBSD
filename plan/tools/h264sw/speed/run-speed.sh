#!/bin/sh
# The speed measurement of the software H.264 decoder (libmedia's h264sw-* with the shared vcodec-* modules) on the host:
#  1. builds speed-bench.c with every decoder module except the back end, without sanitizers, at the optimisation of
#     libmedia's build (-O2; override with SPEED_CFLAGS);
#  2. makes 1080p streams with the host's ffmpeg (libx264): High with CABAC, B pictures, the 8x8 transform and
#     weighted prediction at 8 Mbit/s on a grainy picture (the entropy-decoding load) and at CRF 23 on a clean one
#     (the motion-compensation load), and Main with CAVLC at 8 Mbit/s; any files given as arguments (Annex B .h264, or
#     MP4/Matroska/TS, which ffmpeg rewrites to Annex B without decoding) are measured too;
#  3. checks that each decodes to the same frames as ffmpeg (MD5 of the whole I420 output);
#  4. times each SPEED_RUNS times (default 5; best and median frames/s, one thread);
#  5. profiles one stream (SPEED_PROFILE_STREAM, default high-1080-grain) with perf (sudo when perf_event_paranoid
#     needs it) or, without perf, gprof, and sums the self time by function and by module.
# libx264's output differs a little from run to run; to compare two trees, give the first run's streams/*.h264 as
# FILEs to the second, which then replace the streams it made under the same names.
# Prints the summary (also summary.txt in the output directory); the last line is run-speed: PASS or FAIL.
#   sh plan/tools/h264sw/speed/run-speed.sh [FILE...]
#   SPEED_RUNS=3 SPEED_FRAMES=120 SPEED_PROFILE=0 SPEED_OUT=/tmp/x sh plan/tools/h264sw/speed/run-speed.sh
# Output under $SPEED_OUT (default /tmp/h264sw-speed, a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
root=$(pwd)
. plan/tools/fresh-out.sh
out=${SPEED_OUT:-/tmp/h264sw-speed}
fresh_out "$out"
out=$(cd "$out" && pwd -P)
runs=${SPEED_RUNS:-5}
frames=${SPEED_FRAMES:-240}
profile=${SPEED_PROFILE:-1}
profile_stream=${SPEED_PROFILE_STREAM:-high-1080-grain}
cflags=${SPEED_CFLAGS:--O2}
summary="$out/summary.txt"
status=0

say() {
	echo "$*" | tee -a "$summary"
}

if ! command -v ffmpeg > /dev/null 2>&1; then
	echo "run-speed: FAIL (no ffmpeg)"
	exit 1
fi

# 1. The modules, each its own object (the profile maps symbols back to modules by them).
lib=userland/desktop/libmedia
modules=""
for file in $lib/h264sw-*.c $lib/vcodec-*.c $lib/bits.c; do
	[ -f "$file" ] || continue
	case "$file" in
	*/h264sw-backend.c) ;;
	*) modules="$modules $file" ;;
	esac
done
mkdir -p "$out/obj"
objects=""
for file in $modules plan/tools/h264sw/speed/speed-bench.c; do
	object="$out/obj/$(basename "$file" .c).o"
	# shellcheck disable=SC2086
	if ! cc -std=c89 $cflags -g -fno-omit-frame-pointer -Wall -Wextra -Werror -D_DEFAULT_SOURCE -I. -c "$file" \
	    -o "$object" >> "$out/build.log" 2>&1; then
		echo "speed-bench: FAIL (build of $file)"
		tail -20 "$out/build.log"
		echo "run-speed: FAIL"
		exit 1
	fi
	objects="$objects $object"
done
# shellcheck disable=SC2086
if ! cc $objects -lm -o "$out/speed-bench" >> "$out/build.log" 2>&1; then
	echo "speed-bench: FAIL (link)"
	tail -20 "$out/build.log"
	echo "run-speed: FAIL"
	exit 1
fi

say "host: $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //'), $(cc --version | head -1), CFLAGS $cflags"
say "tree: $(git -C "$root" rev-parse --short HEAD 2>/dev/null)$(git -C "$root" diff --quiet -- userland 2>/dev/null || echo ' (modified)')"

# 2. The streams: name, source filter, then the encoder options.
mkdir -p "$out/streams"
make_stream() {
	name=$1
	filter=$2
	shift 2
	if ! timeout 600 ffmpeg -v error -nostdin -f lavfi -i "$filter" -frames:v "$frames" -c:v libx264 "$@" \
	    -f h264 "$out/streams/$name.h264" > "$out/streams/$name.log" 2>&1; then
		say "$name: FAIL (ffmpeg could not make it)"
		status=1
	fi
}
grain="testsrc2=size=1920x1080:rate=30,format=yuv420p,noise=alls=10:allf=t"
clean="testsrc2=size=1920x1080:rate=30,format=yuv420p"
make_stream high-1080-grain "$grain" -profile:v high -preset medium -b:v 8M -maxrate 12M -bufsize 16M
make_stream high-1080-clean "$clean" -profile:v high -preset medium -crf 23
make_stream main-1080-cavlc "$grain" -profile:v main -preset medium -b:v 8M -maxrate 12M -bufsize 16M \
    -x264-params cabac=0
for file in "$@"; do
	name=$(basename "$file")
	name=${name%.*}
	case "$file" in
	*.h264 | *.264)
		cp -- "$file" "$out/streams/$name.h264"
		;;
	*)
		if ! timeout 600 ffmpeg -v error -nostdin -i "$file" -map 0:v:0 -c:v copy -bsf:v h264_mp4toannexb \
		    -f h264 "$out/streams/$name.h264" > "$out/streams/$name.log" 2>&1; then
			say "$name: FAIL (ffmpeg could not rewrite it to Annex B)"
			status=1
		fi
		;;
	esac
done

# 3 and 4. Each stream: the same frames as ffmpeg, then the timing.
first=""
for file in "$out"/streams/*.h264; do
	[ -f "$file" ] || continue
	name=$(basename "$file" .h264)
	info=$(ffprobe -v error -select_streams v:0 -show_entries stream=profile,width,height -of csv=p=0 "$file" \
	    2> /dev/null | head -1)
	bytes=$(wc -c < "$file")
	ours=$(timeout 1200 "$out/speed-bench" --yuv "$file" 2> "$out/$name.yuv.log" | md5sum | cut -d' ' -f1)
	theirs=$(timeout 1200 ffmpeg -v error -nostdin -i "$file" -fps_mode passthrough -f rawvideo - 2> /dev/null |
	    md5sum | cut -d' ' -f1)
	if [ "$ours" != "$theirs" ]; then
		say "$name ($info, $bytes bytes): FAIL (output differs from ffmpeg; $(cat "$out/$name.yuv.log"))"
		status=1
		continue
	fi
	if ! timeout 3600 "$out/speed-bench" "$file" "$runs" > "$out/$name.speed" 2>&1; then
		say "$name ($info, $bytes bytes): FAIL (speed-bench; log $out/$name.speed)"
		status=1
		continue
	fi
	say "$name ($info, $bytes bytes): same as ffmpeg; $(tail -1 "$out/$name.speed" | sed 's/^speed-bench: //')"
	if [ -z "$first" ] || [ "$name" = "$profile_stream" ]; then
		first=$file
	fi
done

# 5. The profile of the first stream: the self time of each function, and summed by module.
profile_lines=30
if [ "$profile" = 1 ] && [ -n "$first" ]; then
	# Which module defines each function: the text symbols of each object.
	: > "$out/symbols.txt"
	for object in "$out"/obj/*.o; do
		module=$(basename "$object" .o)
		nm --defined-only "$object" | awk -v m="$module" '$2 ~ /^[tT]$/ { s = $3; sub(/\..*$/, "", s); print s, m }' >> "$out/symbols.txt"
	done
	perf=""
	if command -v perf > /dev/null 2>&1; then
		if [ "$(cat /proc/sys/kernel/perf_event_paranoid 2> /dev/null || echo 3)" -le 2 ]; then
			perf="perf"
		elif sudo -n true 2> /dev/null; then
			perf="sudo -n perf"
		fi
	fi
	if [ -n "$perf" ]; then
		tool="perf"
		$perf record -q -F 2999 -o "$out/perf.data" -- "$out/speed-bench" "$first" 1 > "$out/profile.run" 2>&1
		# A file written by root is handed back, so the run directory stays the caller's.
		[ "$perf" = perf ] || sudo -n chown "$(id -u):$(id -g)" "$out/perf.data"
		# The report needs no privilege; a compiler's clone suffix (.part.0, .constprop.0) is dropped from the name.
		perf report -i "$out/perf.data" --no-children --sort symbol --stdio -q 2> /dev/null |
		    awk '$1 ~ /%$/ && $2 == "[.]" { p = $1; sub(/%$/, "", p); s = $3; sub(/\..*$/, "", s); print p, s }' |
		    awk '{ t[$2] += $1 } END { for (k in t) print t[k], k }' > "$out/profile.raw"
	else
		tool="gprof"
		# shellcheck disable=SC2086
		cc -std=c89 $cflags -pg -D_DEFAULT_SOURCE -I. $modules plan/tools/h264sw/speed/speed-bench.c -lm \
		    -o "$out/speed-bench-pg" >> "$out/build.log" 2>&1
		(cd "$out" && ./speed-bench-pg "$first" 1 > profile.run 2>&1)
		gprof -b -p "$out/speed-bench-pg" "$out/gmon.out" 2> /dev/null |
		    awk 'NF >= 4 && $1 ~ /^[0-9.]+$/ { s = $NF; sub(/\..*$/, "", s); print $1, s }' > "$out/profile.raw"
	fi
	say "profile ($tool, $(basename "$first" .h264), self time) by function:"
	sort -k1,1 -g -r "$out/profile.raw" | head -n "$profile_lines" | awk 'NR == FNR { m[$1] = $2; next }
	    { printf "  %6.2f%%  %-40s %s\n", $1, $2, ($2 in m) ? m[$2] : "-" }' "$out/symbols.txt" - | tee -a "$summary"
	say "by module:"
	awk 'NR == FNR { m[$1] = $2; next } { k = ($2 in m) ? m[$2] : "(outside)"; t[k] += $1 }
	    END { for (k in t) printf "%.2f %s\n", t[k], k }' "$out/symbols.txt" "$out/profile.raw" |
	    sort -k1,1 -g -r | awk '{ printf "  %6.2f%%  %s\n", $1, $2 }' | tee -a "$summary"
fi

say "output: $out"
if [ "$status" = 0 ]; then say "run-speed: PASS"; else say "run-speed: FAIL"; fi
exit "$status"

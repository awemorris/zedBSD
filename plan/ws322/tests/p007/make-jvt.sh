#!/bin/sh
# Makes the JVT conformance streams of the software decoder's range (plan/tools/h264sw/conformance/list.txt, the
# streams marked in) ready for vkvideo-probe on the board (ws322-p007): OUT/<name>.h264 is the stream's bitstream and
# OUT/<name>.sha256 the SHA-256 of each frame of its reference decoder's YUV as NV12 (what vkvideo-probe --expect
# reads, in display order), its size the host's ffprobe's of the bitstream.  The bitstream and the reference are
# picked as plan/tools/h264sw/conformance/run.sh picks them (files.txt, else the one file of each kind); a stream whose
# files or size cannot be read is listed in OUT/skipped.txt with why.  The zips are fetch.sh's, unpacked under CACHE.
#   sh plan/ws322/tests/p007/make-jvt.sh OUT CACHE
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: make-jvt.sh OUT CACHE}
cache=${2:?the unpacked zips}
top=$(dirname "$0")/../../../..
list=$top/plan/tools/h264sw/conformance/list.txt
overrides=$top/plan/tools/h264sw/conformance/files.txt
mkdir -p "$out"
: > "$out/skipped.txt"
find_one() {
	dir=$1
	shift
	pattern=""
	for extension in "$@"; do
		pattern="$pattern -o -iname *.$extension"
	done
	pattern=${pattern# -o }
	set -f
	# shellcheck disable=SC2086
	find "$dir" -type f \( $pattern \) | sort
	set +f
}
count=0
for entry in $(grep -v '^#' "$list" | awk -F'\t' '$2 == "in" { print $1 }'); do
	name=$(basename "$entry")
	dir=$cache/$name
	[ -d "$dir" ] || { echo "$name not fetched" >> "$out/skipped.txt"; continue; }
	bitstream=""
	reference=""
	limit=""
	if [ -f "$overrides" ]; then
		bitstream=$(awk -v n="$name" '$1 == n { print $2 }' "$overrides")
		reference=$(awk -v n="$name" '$1 == n { print $3 }' "$overrides")
		limit=$(awk -v n="$name" '$1 == n { print $4 }' "$overrides")
	fi
	if [ -n "$bitstream" ]; then
		bitstream=$dir/$bitstream
		reference=$dir/$reference
	else
		bitstream=$(find_one "$dir" 264 h264 jsv 26l jvt avc bit bits)
		reference=$(find_one "$dir" yuv qcif cif)
	fi
	if [ "$(printf '%s\n' "$bitstream" | grep -c .)" != 1 ] || [ "$(printf '%s\n' "$reference" | grep -c .)" != 1 ]; then
		echo "$name files: '$(echo $bitstream)' '$(echo $reference)'" >> "$out/skipped.txt"
		continue
	fi
	size=$(timeout 60 ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x "$bitstream" 2> /dev/null | head -1)
	width=${size%x*}
	height=${size#*x}
	case "$width$height" in *[!0-9]* | "") echo "$name size '$size'" >> "$out/skipped.txt"; continue ;; esac
	frame=$((width * height * 3 / 2))
	frames=$(($(wc -c < "$reference") / frame))
	[ -n "$limit" ] && [ "$limit" -lt "$frames" ] && frames=$limit
	cp "$bitstream" "$out/$name.h264"
	timeout 300 ffmpeg -hide_banner -loglevel error -f rawvideo -pix_fmt yuv420p -s "${width}x$height" -i "$reference" \
		-frames:v "$frames" -pix_fmt nv12 -f framehash -hash sha256 - | grep -v '^#' | awk -F', *' '{ print $6 }' > "$out/$name.sha256"
	echo "$name ${width}x$height $frames frames $(wc -c < "$out/$name.h264") bytes" >> "$out/streams.txt"
	count=$((count + 1))
done
echo "make-jvt: $count streams, $(wc -l < "$out/skipped.txt") skipped"

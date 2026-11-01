#!/bin/sh
# The H.265 parse conformance run over the HEVC_v1 bitstreams (ITU-T H.265.1, plan/tools/h265/conformance/): builds
# conform.c with the parse modules of userland/desktop/libmedia (host cc, -std=c89, ASan and UBSan), and for each
# zip of the cache reads its bitstream with `unzip -p` (nothing is unpacked to disk), runs
#   conform (h265-stream, h265-dpb with 17 slots, h265-std and its StdVideo round trip),
#   ffmpeg -bsf:v trace_headers (the syntax element values) and
#   ffmpeg -threads 1 -loglevel debug (the decoded and output POCs),
# and compares them with compare.py.  When ffmpeg cannot read the stream because a PPS comes before its SPS,
# ffmpeg is given the copy from reorder-ps.py (the parameter sets before the first picture in VPS, SPS, PPS order).
#   sh plan/ws324/tests/conformance/run.sh [<name>...]     (default: every name of list.txt, and HRD_A_Fujitsu_3+du)
# A name <zip>+du is the zip's bitstream through interleave-du.py (decoding unit information SEI and PPS NAL units
# between the slice segments of each picture, which no HEVC_v1 bitstream has), written to the output directory.
# The cache is $H265SW_CONFORMANCE_DIR (default /home/awe/zedBSD-claude1/build/sources/h265-conformance, read only).
# Output: build/tmp/h265-conformance (a fresh run directory): <name>.conform, .trace.gz, .decode.gz, .detail,
# summary.txt.  Each command has a time limit.
# Last line: conformance: PASS (<in> streams, <out> out of range, <d> derived) or conformance: FAIL (<n> streams ...).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
out=build/tmp/h265-conformance
fresh_out "$out"
cache=${H265SW_CONFORMANCE_DIR:-/home/awe/zedBSD-claude1/build/sources/h265-conformance}
here=plan/ws324/tests/conformance
list=plan/tools/h265/conformance/list.txt

# The tool, with every parse module.
lib=userland/desktop/libmedia
modules=""
for file in $lib/h265sw-*.c $lib/h265-*.c; do
	modules="$modules $file"
done
if ! timeout 600 cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
    -fno-sanitize-recover=all -fno-omit-frame-pointer -I. -Iplan/tools/h264sw/unit -Iplan/ws324/tests/unit \
    $here/conform.c $modules $lib/vcodec-bitreader.c $lib/vcodec-nal.c $lib/bits.c -lm \
    -o "$out/conform" > "$out/build.log" 2>&1; then
	echo "conform: build failed"
	head -20 "$out/build.log"
	echo "conformance: FAIL (build)"
	exit 1
fi

names="$*"
if [ -z "$names" ]; then
	names="$(grep -v '^#' "$list" | cut -f1) HRD_A_Fujitsu_3+du"
fi

# The bitstream of the stream being checked: the zip's, or the derived file.
bitstream() {
	if [ -n "$file" ]; then
		cat "$file"
	else
		unzip -p "$zip" "$member"
	fi
}

: > "$out/summary.txt"
for name in $names; do
	# A name with "+du" is the zip's bitstream with decoding unit SEI and PPS NAL units between its segments.
	zip="$cache/${name%+du}.zip"
	member=$(unzip -Z1 "$zip" | grep -v '__MACOSX\|/\._\|^\._' | grep -i '\.\(bit\|bin\)$' | head -1)
	if [ -z "$member" ]; then
		echo "stream $name FAIL (no bitstream in the zip)" | tee -a "$out/summary.txt"
		continue
	fi

	file=""
	source="--zip $zip --member $member"
	if [ "$name" != "${name%+du}" ]; then
		file="$out/$name.bit"
		unzip -p "$zip" "$member" | timeout 300 /usr/bin/python3 -I $here/interleave-du.py > "$file"
		source="--bitstream $file"
	fi

	# conform over the bitstream.
	bitstream | ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	    timeout 600 "$out/conform" - > "$out/$name.conform" 2> "$out/$name.conform.err"
	status=$?
	if [ "$status" != 0 ]; then
		echo "stream $name FAIL (conform exit $status, $out/$name.conform.err)" | tee -a "$out/summary.txt"
		continue
	fi

	# ffmpeg's trace of the syntax; again over the reordered copy when it could not read the stream.
	feed="cat"
	bitstream | timeout 600 ffmpeg -hide_banner -nostdin -f hevc -i pipe:0 -c copy \
	    -bsf:v trace_headers -f null - 2>&1 | sed 's/^\[trace_headers @ 0x[0-9a-f]*\] //' | gzip > "$out/$name.trace.gz"
	if ! zcat "$out/$name.trace.gz" | grep -q '^Packet:'; then
		feed="/usr/bin/python3 -I $here/reorder-ps.py"
		echo "$name: ffmpeg read the reordered copy" >> "$out/reordered.txt"
		bitstream | $feed | timeout 600 ffmpeg -hide_banner -nostdin -f hevc -i pipe:0 -c copy \
		    -bsf:v trace_headers -f null - 2>&1 | sed 's/^\[trace_headers @ 0x[0-9a-f]*\] //' | \
		    gzip > "$out/$name.trace.gz"
	fi

	# ffmpeg's decode, for the POCs.
	bitstream | $feed | timeout 600 ffmpeg -hide_banner -nostdin -threads 1 -loglevel debug \
	    -f hevc -i pipe:0 -f null - 2>&1 | grep 'frame with POC\|^Stream mapping:\|Error\|error' | \
	    gzip > "$out/$name.decode.gz"

	# The comparison.
	if ! timeout 600 /usr/bin/python3 -I $here/compare.py --name "$name" $source \
	    --conform "$out/$name.conform" --trace "$out/$name.trace.gz" --decode "$out/$name.decode.gz" \
	    --detail "$out/$name.detail" >> "$out/summary.txt" 2> "$out/$name.compare.err"; then
		echo "stream $name FAIL (compare.py, $out/$name.compare.err)" | tee -a "$out/summary.txt"
		continue
	fi

	tail -1 "$out/summary.txt"
done
sync

# The verdict: every stream PASS.
pass_in=$(grep -c '^stream [^ +]* PASS class=in' "$out/summary.txt")
pass_out=$(grep -c '^stream [^ +]* PASS class=out' "$out/summary.txt")
pass_derived=$(grep -c '^stream [^ ]*+[^ ]* PASS' "$out/summary.txt")
fail=$(grep -c '^stream .* FAIL' "$out/summary.txt")
if [ "$fail" = 0 ]; then
	echo "conformance: PASS ($pass_in streams, $pass_out out of range, $pass_derived derived)"
	exit 0
fi

echo "conformance: FAIL ($fail streams; $pass_in in, $pass_out out of range and $pass_derived derived passed)"
exit 1

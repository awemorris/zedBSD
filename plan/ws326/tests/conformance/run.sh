#!/bin/sh
# The JVT conformance test of the software H.264 decoder (libmedia's h264sw-*): conform.c is built with every
# h264sw-*.c and vcodec-*.c except the back end, and bits.c, with ASan and UBSan, and run on each stream of
# plan/ws326/tests/conformance/list.txt (or the <dir>/<name> given) whose zip fetch.sh has unpacked: the bitstream
# is decoded and each output frame compared with the reference decoder's YUV (size, count, MD5 of each frame).
#   sh plan/ws326/tests/conformance/run.sh [<dir>/<name>...]
# The unpacked zips are under $H264SW_CONFORMANCE_DIR (default build/tmp/ws326-conformance/zips), as fetch.sh
# leaves them.  Each line of the result says the stream, what list.txt expects (in, out:<reason>, or ?) and what
# happened: PASS (every frame equal), OUT (the decoder refused it, with the reasons conform read), FAIL.
# An expectation of in must PASS and one of out:<reason> must be OUT with that reason; ? only reports.
# The conformance streams with arbitrary slice order also use slice groups (FM1_FT_E, FM2_SVA_C), which are out; so
# the streams of $aso_streams (Baseline and Extended with several slices per picture, in order) are decoded a second
# time with conform --aso, each picture's slices given in reverse, and must still equal the reference.
# Last line: conform-run: PASS (<n> in, <m> out) or conform-run: FAIL.  Output under build/tmp/ws326-conform
# (a fresh run directory): <name>.log per stream and results.txt.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
list=plan/ws326/tests/conformance/list.txt
cache=${H264SW_CONFORMANCE_DIR:-build/tmp/ws326-conformance/zips}
out=build/tmp/ws326-conform
fresh_out "$out"
status=0

modules=""
for file in userland/desktop/libmedia/h264sw-*.c userland/desktop/libmedia/vcodec-*.c; do
	case "$file" in
	*/h264sw-backend.c) ;;
	*) modules="$modules $file" ;;
	esac
done

if ! cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I. plan/ws326/tests/conformance/conform.c $modules userland/desktop/libmedia/bits.c \
    -lm -o "$out/conform" > "$out/build.log" 2>&1; then
	echo "conform: FAIL (build)"
	head -20 "$out/build.log"
	echo "conform-run: FAIL"
	exit 1
fi

export ASAN_OPTIONS=detect_leaks=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

# The bitstream and the reference of a zip whose names the extensions do not settle: <name> <bitstream> <reference>
# [<frames>], paths inside the unpacked zip; <frames> when the reference holds more frames than the bitstream gives.
overrides=plan/ws326/tests/conformance/files.txt

# The streams decoded a second time with their slices in reverse order (see above).
aso_streams="AVCv1/BA1_FT_C AVCv1/BASQP1_Sony_C AVCv1/CI1_FT_B AVCv1/SVA_Base_B AVCv1/SVA_CL1_E AVCv1/CABAST3_Sony_E"

# Finds the one file of a directory whose extension (any case) is one of the given ones.
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

names="$*"
if [ -z "$names" ]; then
	names=$(grep -v '^#' "$list" | cut -f1)
fi

in_pass=0
aso_pass=0
out_count=0
frames_pass=0
: > "$out/results.txt"
for entry in $names; do
	name=$(basename "$entry")
	expect=$(awk -F'\t' -v n="$entry" '$1 == n { print $2 }' "$list")
	dir="$cache/$name"
	if [ ! -d "$dir" ]; then
		echo "$entry [$expect]: FAIL (not fetched: $dir)" | tee -a "$out/results.txt"
		status=1
		continue
	fi

	# The bitstream and the reference: the override, else the one file of each kind.
	bitstream=""
	reference=""
	limit=""
	if [ -f "$overrides" ]; then
		bitstream=$(awk -v n="$name" '$1 == n { print $2 }' "$overrides")
		reference=$(awk -v n="$name" '$1 == n { print $3 }' "$overrides")
		limit=$(awk -v n="$name" '$1 == n { print $4 }' "$overrides")
	fi
	if [ -n "$bitstream" ]; then
		bitstream="$dir/$bitstream"
		reference="$dir/$reference"
	else
		bitstream=$(find_one "$dir" 264 h264 jsv 26l jvt avc bit bits)
		reference=$(find_one "$dir" yuv qcif cif)
		# A zip without a reference (some streams of formats outside the range) can only be refused: compare with nothing.
		[ -z "$reference" ] && reference=/dev/null
	fi
	if [ "$(printf '%s\n' "$bitstream" | grep -c .)" != 1 ] || [ "$(printf '%s\n' "$reference" | grep -c .)" != 1 ]; then
		echo "$entry [$expect]: FAIL (files: bitstream '$(echo $bitstream)', reference '$(echo $reference)'; add a line to files.txt)" |
		    tee -a "$out/results.txt"
		status=1
		continue
	fi

	# shellcheck disable=SC2086
	timeout 1800 "$out/conform" "$bitstream" "$reference" $limit > "$out/$name.log" 2>&1
	last=$(tail -1 "$out/$name.log")
	case "$last" in
	"conform: PASS"*) result=PASS ;;
	"conform: OUT"*) result=OUT ;;
	*) result=FAIL ;;
	esac
	reasons=$(echo "$last" | sed -n 's/^conform: OUT (\(.*\))$/\1/p')

	# Judges the result against the expectation.
	verdict=ok
	case "$expect" in
	in)
		[ "$result" = PASS ] || verdict=WRONG
		;;
	out:*)
		if [ "$result" != OUT ]; then
			verdict=WRONG
		else
			case ",$reasons," in
			*",${expect#out:},"*) ;;
			*) verdict=WRONG ;;
			esac
		fi
		;;
	*)
		[ "$result" = FAIL ] && verdict=WRONG
		;;
	esac
	if [ "$result" = PASS ]; then
		in_pass=$((in_pass + 1))
		frames_pass=$((frames_pass + $(echo "$last" | sed 's/.*(\([0-9]*\) frames)/\1/')))
	fi
	if [ "$result" = OUT ]; then
		out_count=$((out_count + 1))
	fi
	if [ "$verdict" = WRONG ]; then
		status=1
	fi
	echo "$entry [$expect]: $verdict $last" | tee -a "$out/results.txt"

	# The second decode with the slices of each picture in reverse order.
	case " $aso_streams " in
	*" $entry "*) ;;
	*) continue ;;
	esac
	# shellcheck disable=SC2086
	timeout 1800 "$out/conform" --aso "$bitstream" "$reference" $limit > "$out/$name.aso.log" 2>&1
	last=$(tail -1 "$out/$name.aso.log")
	pictures=$(sed -n 's/^aso pictures \([0-9]*\)$/\1/p' "$out/$name.aso.log")
	verdict=ok
	case "$last" in
	"conform: PASS"*) ;;
	*) verdict=WRONG ;;
	esac
	[ "${pictures:-0}" -gt 0 ] || verdict=WRONG
	if [ "$verdict" = WRONG ]; then
		status=1
	else
		aso_pass=$((aso_pass + 1))
	fi
	echo "$entry --aso ($pictures pictures reordered): $verdict $last" | tee -a "$out/results.txt"
done

echo "conform: $in_pass streams ($frames_pass frames) equal to the reference, $out_count refused, $aso_pass equal with the slices reversed"
if [ "$status" = 0 ]; then
	echo "conform-run: PASS ($in_pass in, $out_count out)"
else
	echo "conform-run: FAIL"
fi
exit "$status"

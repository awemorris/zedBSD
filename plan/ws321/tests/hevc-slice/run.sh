#!/bin/sh
# The host test of the kernel's H.265 slice segment header parser (src/drivers/gpu/bcm2711/vk/vulkan-video-hevc-slice.c):
# builds hevc-slice-host-test.c with the parser and libmedia's H.265 parse (host cc, -std=c89, ASan and UBSan), runs it
# over every HEVC_v1 conformance zip of the cache (the bitstream read with `unzip -p`, nothing unpacked to disk), then runs
# the fuzz over the segments the streams gave.
#   sh plan/ws321/tests/hevc-slice/run.sh [<name>...]     (default: every zip of the cache)
# A stream outside the decoder's Main 8-bit 4:2:0 (bcm2712_hevc_check answers ENOTSUP) is counted as refused, not failed.
# The cache is $H265SW_CONFORMANCE_DIR (default /home/awe/zedBSD-claude1/build/sources/h265-conformance, read only).
# $HEVC_SLICE_FUZZ is the number of fuzz cases (default 100000) and $HEVC_SLICE_SEED the seed (default 1).
# Output: build/tmp/hevc-slice (a fresh run directory): <name>.txt for each stream, corpus.bin, fuzz.txt, summary.txt.
# Each command has a time limit.
# Last line: hevc-slice run: PASS (<n> streams, <m> refused) or hevc-slice run: FAIL (<what>).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
out=build/tmp/hevc-slice
fresh_out "$out"
cache=${H265SW_CONFORMANCE_DIR:-/home/awe/zedBSD-claude1/build/sources/h265-conformance}
here=plan/ws321/tests/hevc-slice
cases=${HEVC_SLICE_FUZZ:-100000}
seed=${HEVC_SLICE_SEED:-1}

# The test: the parser under test and libmedia's parse modules.
lib=userland/desktop/libmedia
modules=""
for file in $lib/h265sw-*.c $lib/h265-*.c; do
	modules="$modules $file"
done
if ! timeout 600 cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
    -fno-sanitize-recover=all -fno-omit-frame-pointer -Iinclude -Isrc -I. -Iplan/tools/h264sw/unit -Iplan/ws324/tests/unit \
    $here/hevc-slice-host-test.c src/drivers/gpu/bcm2711/vk/vulkan-video-hevc-slice.c $modules \
    $lib/vcodec-bitreader.c $lib/vcodec-nal.c $lib/bits.c -lm -o "$out/hevc-slice-host-test" > "$out/build.log" 2>&1; then
	echo "hevc-slice: build failed"
	head -20 "$out/build.log"
	echo "hevc-slice run: FAIL (build)"
	exit 1
fi

names="$*"
if [ -z "$names" ]; then
	names=$(ls "$cache" | sed -n 's/\.zip$//p')
fi

: > "$out/summary.txt"
: > "$out/corpus.bin"
failed=""
streams=0
for name in $names; do
	zip="$cache/$name.zip"
	member=$(unzip -Z1 "$zip" 2>/dev/null | grep -v '__MACOSX\|/\._\|^\._' | grep -i '\.\(bit\|bin\)$' | head -1)
	if [ -z "$member" ]; then
		echo "stream $name: FAIL (no bitstream in the zip)" | tee -a "$out/summary.txt"
		failed="$failed $name"
		continue
	fi
	streams=$((streams + 1))

	# The bitstream through the test; the first line of the test's output that names the stream is the summary line.
	unzip -p "$zip" "$member" | ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	    timeout 900 "$out/hevc-slice-host-test" --name "$name" --corpus-out "$out/corpus.bin" - \
	    > "$out/$name.txt" 2> "$out/$name.err"
	status=$?
	last=$(tail -1 "$out/$name.txt")
	grep '^stream \|^  coverage\|^  missing-reference' "$out/$name.txt" >> "$out/summary.txt"
	if [ "$last" != "hevc-slice: PASS" ]; then
		if [ "$last" = "hevc-slice: FAIL" ]; then
			echo "stream $name: FAIL (mismatches, $out/$name.txt)" | tee -a "$out/summary.txt"
		else
			echo "stream $name: FAIL (exit $status without a verdict, $out/$name.err)" | tee -a "$out/summary.txt"
		fi
		failed="$failed $name"
	fi
done
sync

# The fuzz over the real segments the streams gave.
fuzz_ok=1
if [ -s "$out/corpus.bin" ]; then
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	    timeout 3600 "$out/hevc-slice-host-test" --fuzz "$cases" --seed "$seed" "$out/corpus.bin" \
	    > "$out/fuzz.txt" 2> "$out/fuzz.err"
	status=$?
	cat "$out/fuzz.txt" >> "$out/summary.txt"
	if [ "$(tail -1 "$out/fuzz.txt")" != "hevc-slice: PASS" ]; then
		fuzz_ok=0
		echo "fuzz: FAIL (exit $status, $out/fuzz.txt, $out/fuzz.err)" | tee -a "$out/summary.txt"
	fi
else
	fuzz_ok=0
	echo "fuzz: FAIL (no corpus)" | tee -a "$out/summary.txt"
fi
sync

# The verdict.
refused=$(grep -c '^stream [^ ]*: REFUSED' "$out/summary.txt")
if [ -z "$failed" ] && [ "$fuzz_ok" = 1 ]; then
	echo "hevc-slice run: PASS ($streams streams, $refused refused)" | tee -a "$out/summary.txt"
	exit 0
fi
if [ "$fuzz_ok" = 0 ]; then
	failed="$failed fuzz"
fi
echo "hevc-slice run: FAIL ($streams streams, $refused refused; failed:$failed)" | tee -a "$out/summary.txt"
exit 1

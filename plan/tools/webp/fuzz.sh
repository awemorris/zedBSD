#!/bin/sh
# Fuzzes libwebp-compat's interface with libFuzzer (host clang, -fsanitize=fuzzer,address,undefined), the corpus
# seeded with the fixtures of 64 KiB or less (make-fixtures.sh first).  Default 600 seconds.
#   sh plan/tools/webp/fuzz.sh [SECONDS]
# Last line: fuzz: PASS (<seconds> s, <runs> runs) or fuzz: FAIL (crash files under ...).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
seconds=${1:-600}
fixtures=${WEBPC_FIXTURES:-build/tmp/webp-fixtures}
if [ ! -d "$fixtures/files" ]; then
	echo "fuzz: FAIL (no fixtures; run sh plan/tools/webp/make-fixtures.sh)"
	exit 1
fi
out=build/tmp/webp-fuzz
fresh_out "$out"
mkdir -p "$out/include/compat/webp" "$out/corpus" "$out/crashes"
cp include/libc/compat/webp/*.h "$out/include/compat/webp/"
find "$fixtures/files" -name '*.webp' -size -65k -exec cp {} "$out/corpus/" \;
if ! clang -std=c89 -O1 -g -D_DEFAULT_SOURCE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
    -I"$out/include" -Iuserland/desktop/libwebp-compat plan/tools/webp/fuzz-target.c \
    userland/desktop/libwebp-compat/*.c -o "$out/fuzz-target" > "$out/build.log" 2>&1; then
	head -20 "$out/build.log"
	echo "fuzz: FAIL (build)"
	exit 1
fi
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout $((seconds + 120)) "$out/fuzz-target" "$out/corpus" \
	-max_total_time="$seconds" -max_len=65536 -timeout=30 -rss_limit_mb=4096 -artifact_prefix="$out/crashes/" \
	> "$out/fuzz.log" 2>&1
status=$?
runs=$(grep -o 'Done [0-9]* runs' "$out/fuzz.log" | awk '{print $2}')
if [ "$status" = 0 ] && [ -z "$(ls "$out/crashes")" ]; then
	echo "fuzz: PASS ($seconds s, ${runs:-?} runs)"
	exit 0
fi
tail -30 "$out/fuzz.log"
echo "fuzz: FAIL (status $status; crash files under $out/crashes)"
exit 1

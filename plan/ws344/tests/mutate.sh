#!/bin/sh
# Runs every fixture (make-fixtures.sh first) and a fixed set of its mutations (cuts, replaced bytes; see
# fuzz-target.c) through libwebp-compat's interface with ASan and UBSan.  A crash, a sanitizer report or a file
# taking more than 300 s (the program's alarm; 360 s for the whole process) fails.
#   sh plan/ws344/tests/mutate.sh [NAME...]        (default: every fixture of 256 KiB or less)
# Last line: mutate: PASS (<n> files, <m> inputs) or mutate: FAIL (...).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
fixtures=${WEBPC_FIXTURES:-build/tmp/ws344-fixtures}
if [ ! -d "$fixtures/files" ]; then
	echo "mutate: FAIL (no fixtures; run sh plan/ws344/tests/make-fixtures.sh)"
	exit 1
fi
out=build/tmp/ws344-mutate
fresh_out "$out"
mkdir -p "$out/include/compat/webp"
cp include/libc/compat/webp/*.h "$out/include/compat/webp/"
if ! cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -DFUZZ_MUTATE -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -I"$out/include" -Iuserland/desktop/libwebp-compat plan/ws344/tests/fuzz-target.c \
    userland/desktop/libwebp-compat/*.c -o "$out/fuzz-mutate" > "$out/build.log" 2>&1; then
	head -20 "$out/build.log"
	echo "mutate: FAIL (build)"
	exit 1
fi

files=""
if [ $# -gt 0 ]; then
	for name in "$@"; do files="$files $fixtures/files/$name.webp"; done
else
	files=$(find "$fixtures/files" -name '*.webp' -size -257k | sort)
fi
total=0
inputs=0
failed=0
for file in $files; do
	total=$((total + 1))
	if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	    timeout 360 "$out/fuzz-mutate" "$file" > "$out/one.out" 2> "$out/one.err"; then
		count=$(awk '{print $3}' "$out/one.out")
		inputs=$((inputs + ${count:-0}))
	else
		failed=$((failed + 1))
		echo "FAILED $file" >> "$out/failures.log"
		tail -40 "$out/one.err" >> "$out/failures.log"
	fi
done
if [ "$failed" = 0 ]; then echo "mutate: PASS ($total files, $inputs inputs)"; exit 0; fi
grep '^FAILED' "$out/failures.log" | head -10
echo "mutate: FAIL ($failed of $total files; $out/failures.log)"
exit 1

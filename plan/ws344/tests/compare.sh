#!/bin/sh
# Compares libwebp-compat with libwebp through the interface, on the fixtures (make-fixtures.sh first): the driver
# webp-driver.c built with every library source (ASan and UBSan) reports each fixture, and its report must equal the
# one the same driver built against libwebp 1.5.0 wrote (expect/NAME.ref), line for line.
#   sh plan/ws344/tests/compare.sh [NAME...]        (default: every fixture)
# Last line: compare: PASS (<n> files) or compare: FAIL (<m> of <n> files differ; diffs under ...).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
fixtures=${WEBPC_FIXTURES:-build/tmp/ws344-fixtures}
if [ ! -d "$fixtures/expect" ]; then
	echo "compare: FAIL (no fixtures; run sh plan/ws344/tests/make-fixtures.sh)"
	exit 1
fi
out=build/tmp/ws344-compare
fresh_out "$out"
mkdir -p "$out/include/compat/webp" "$out/diff"
cp include/libc/compat/webp/*.h "$out/include/compat/webp/"
if ! cc -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$out/include" -Iuserland/desktop/libwebp-compat plan/ws344/tests/webp-driver.c \
    userland/desktop/libwebp-compat/*.c -o "$out/webp-driver" > "$out/build.log" 2>&1; then
	head -20 "$out/build.log"
	echo "compare: FAIL (build)"
	exit 1
fi

names="$*"
if [ -z "$names" ]; then
	names=$(ls "$fixtures/files" | sed 's/\.webp$//')
fi
total=0
failed=0
for name in $names; do
	total=$((total + 1))
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		timeout 300 "$out/webp-driver" "$fixtures/files/$name.webp" > "$out/$name.out" 2>&1
	if ! cmp -s "$out/$name.out" "$fixtures/expect/$name.ref"; then
		failed=$((failed + 1))
		diff "$fixtures/expect/$name.ref" "$out/$name.out" > "$out/diff/$name.diff"
	fi
done
if [ "$failed" = 0 ]; then echo "compare: PASS ($total files)"; exit 0; fi
ls "$out/diff" | head -10
echo "compare: FAIL ($failed of $total files differ; diffs under $out/diff)"
exit 1

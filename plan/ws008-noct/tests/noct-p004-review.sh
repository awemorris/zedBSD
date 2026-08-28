#!/usr/bin/env bash
# WS008 NOCT-T030--T034 maintainer-review acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
wrapper=$repo/userland/noct
source_dir=$wrapper/NoctLang
test_repository=${NOCT_TEST_REPOSITORY:-https://github.com/awemorris/NoctLang.git}
expected_revision=$(awk '/^NOCT_REVISION[[:space:]]*\?=/{print $3}' \
	"$wrapper/Makefile")

temp_root=$repo/plan/ws008-noct/temp
mkdir -p -- "$temp_root"
output=$(mktemp -d "$temp_root/q022-p004-review.XXXXXX")
delivery=$output/parent/userland/noct
mkdir -p -- "$delivery"
cp -- "$wrapper/Makefile" "$delivery/Makefile"

{
	printf 'test_repository=%s\n' "$test_repository"
	printf 'expected_revision=%s\n' "$expected_revision"
	printf 'canonical_source=%s\n' "$source_dir"
} >"$output/metadata.txt"

make -C "$delivery" checkout \
	NOCT_REPOSITORY="$test_repository" \
	NOCT_REVISION="$expected_revision" \
	ZEDBSD_SOURCE_DIR="$repo" >"$output/source-delivery.log" 2>&1
test "$(git -C "$delivery/NoctLang" rev-parse HEAD)" = "$expected_revision"
test -z "$(git -C "$delivery/NoctLang" symbolic-ref -q HEAD || true)"
make -C "$delivery" checkout \
	NOCT_REPOSITORY="$test_repository" \
	NOCT_REVISION="$expected_revision" \
	ZEDBSD_SOURCE_DIR="$repo" >>"$output/source-delivery.log" 2>&1
test -z "$(git -C "$delivery/NoctLang" status --porcelain=v1 \
	--untracked-files=no)"

if make -C "$delivery" checkout \
	NOCT_SOURCE_DIR=InvalidRepository \
	NOCT_REPOSITORY=file:///definitely/not/a/noct/repository \
	NOCT_REVISION="$expected_revision" \
	ZEDBSD_SOURCE_DIR="$repo" >"$output/invalid-repository.log" 2>&1; then
	echo "invalid Noct repository was accepted" >&2
	exit 1
fi
if make -C "$delivery" checkout \
	NOCT_REPOSITORY="$test_repository" \
	NOCT_REVISION=0000000000000000000000000000000000000000 \
	ZEDBSD_SOURCE_DIR="$repo" >"$output/invalid-revision.log" 2>&1; then
	echo "invalid Noct revision was accepted" >&2
	exit 1
fi

test -f "$source_dir/src/api/api-beui-zedbsd.c"
test -f "$source_dir/include/noct/beui.h"
test ! -e "$source_dir/src/api/beui-zedbsd-input.c"
test ! -e "$source_dir/src/api/beui-zedbsd-input.h"
test ! -e "$source_dir/cmake/modules/Platform/zedBSD.cmake"
test "$(grep -R -l 'noct_register_api_beui(env)' \
	"$source_dir/src/cli" | wc -l)" -eq 2
! grep -R -q 'noct_register_api_beui_\(zedbsd\|sdl2\|pc98dos\)(env)' \
	"$source_dir/src/cli"
make -C "$wrapper" >"$output/zedbsd-build.log" 2>&1
grep -q -- '-D__ZEDBSD__' \
	"$source_dir/build-zedbsd/CMakeFiles/noct.dir/flags.make"
! grep -q -- '-DNOCT_TARGET_ZEDBSD' \
	"$source_dir/build-zedbsd/CMakeFiles/noct.dir/flags.make"
grep -q 'defined(__ZEDBSD__)' "$source_dir/include/noct/c89compat.h"

(cd "$source_dir" && cmake --preset static --fresh) \
	>"$output/static-configure.log" 2>&1
(cd "$source_dir" && cmake --build --preset static --parallel 16) \
	>"$output/static-build.log" 2>&1
(cd "$source_dir" && tests/testcases/run-beui-zedbsd.sh "$repo") \
	>"$output/beui-zedbsd.log" 2>&1
(cd "$source_dir" && tests/test.sh jit-slab build-static) \
	>"$output/jit-slab.log" 2>&1
(cd "$source_dir" && tests/test.sh jit-branch \
	"$source_dir/build-static/noct") >"$output/jit-branch.log" 2>&1

for symbol in jit_build jit_commit jit_free; do
	grep -B1 "^$symbol(" "$source_dir/src/core/jit.h" | grep -q '^bool$'
	while IFS= read -r implementation; do
		grep -B1 "^$symbol(" "$implementation" | grep -q '^bool$' || {
			echo "$symbol is not Boolean in $implementation" >&2
			exit 1
		}
	done < <(grep -l "^$symbol(" "$source_dir/src/core"/jit*.c)
done

printf 'NOCT-T030--T034 host acceptance: PASS (%s)\n' "$output"

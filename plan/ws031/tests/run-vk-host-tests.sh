#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Builds and runs the WS031 Vulkan executor host fixtures, plainly and under
# ASan/UBSan.
#
# cmd, spirv, lower, eu and compile include the files they test.  res,
# resdispatch, pipe, cmdbuf and sync are linked with the executor as the
# kernel builds it: every file of render/ that decodes, records or emits, and
# the shader compiler, each its own translation unit.  They take the kernel
# services and the GPU runs from i915-vk-render-stubs.inc.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ws031-vk-host.XXXXXX")
trap "rm -rf -- \"$work\"" EXIT HUP INT TERM
compiler=${CC:-cc}
tests=${1:-"cmd spirv lower res resdispatch sync eu compile pipe cmdbuf"}
base="-std=gnu11 -Wall -Wextra -Werror -Wdeclaration-after-statement -DKERN_USER_ABI_LP64 -DVK_REPO=\"$repo\" -I$repo/include -I$repo -idirafter $repo/include/libc"

# The executor's objects: everything but draw.c and blit.c, which run work on the GPU.
driver=$repo/src/drivers/gpu/i915
executor=""
for part in codec object dispatch transport instance vulkan fence objects reply \
    memory image descriptor pipeline pipeline-prepare render-pass sync command \
    state batch math; do
    executor="$executor $driver/render/$part.c"
done
for part in spirv compile eu; do
    executor="$executor $driver/compiler/$part.c"
done

for name in $tests; do
    source="$repo/plan/ws031/tests/i915-vk-$name-test.c"
    case $name in
    res|resdispatch|pipe|cmdbuf|sync)
        sources="$source $executor"
        ;;
    *)
        sources=$source
        ;;
    esac
    "$compiler" $base -O2 $sources -o "$work/$name-ordinary" -lm
    "$work/$name-ordinary"
    "$compiler" $base -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $sources -o "$work/$name-sanitized" -lm
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/$name-sanitized"
done
echo "WS031 vk host fixtures PASS: $tests"

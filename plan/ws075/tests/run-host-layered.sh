#!/bin/sh
# ws075-p007b increment b3: builds and runs the layered rendering host test (host-layered.c) with the
# i915 Vulkan executor linked as the kernel builds it and plan/ws031/tests/i915-vk-render-stubs.inc,
# plainly and under ASan/UBSan, in a new directory under build/tmp (nothing is removed here; Q1's
# plan/tools/q1-clean.sh removes the runs build/tmp/ws075-layered does not point at).
#   sh plan/ws075/tests/run-host-layered.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws075-layered"
work=$fresh_dir
compiler=${CC:-cc}
driver=$repo/src/drivers/gpu/i915
executor=""
for part in codec object dispatch transport instance vulkan fence objects reply \
    memory image descriptor forget pipeline pipeline-prepare render-pass sync command \
    state batch math video video-mfx video-h264-tables; do
	executor="$executor $driver/render/$part.c"
done
executor="$executor $repo/src/drivers/gpu/compiler/spirv.c"
for part in compile eu; do
	executor="$executor $driver/compiler/$part.c"
done
base="-std=gnu11 -Wall -Wextra -Werror -Wdeclaration-after-statement -DKERN_USER_ABI_LP64 -DVK_REPO=\"$repo\" -I$repo/include -I$repo/src -I$repo -idirafter $repo/include/libc"
"$compiler" $base -O2 "$repo/plan/ws075/tests/host-layered.c" $executor -o "$work/ordinary" -lm
"$work/ordinary"
"$compiler" $base -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "$repo/plan/ws075/tests/host-layered.c" $executor -o "$work/sanitized" -lm
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/sanitized"
echo "WS075 p007b b3 layered host test PASS (plain, ASan/UBSan)"

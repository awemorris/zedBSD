#!/bin/sh
# ws083-p003b: the video round trip.  host-video-wire.c runs libvulkan's video.c and writes every
# stream it makes into a directory; host-video-executor.c feeds them to the i915 Vulkan executor
# (linked as the kernel builds it, with plan/ws031/tests/i915-vk-render-stubs.inc), plainly and
# under ASan/UBSan.  Everything goes to a new directory under build/tmp (nothing is removed here;
# Q1's plan/tools/q1-clean.sh removes the runs build/tmp/ws083-video-roundtrip does not point at).
# ws083-p004: the decodes that pass are written as MFX commands; the batch of the IDR and the P picture is
# read back through Mesa's genxml by genxml-decode.py (MESA-TREE defaults to build/mesa-tools/mesa-25.0.7).
#   sh plan/ws083/tests/run-host-video-roundtrip.sh [MESA-TREE]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
mesa=${1:-$repo/build/mesa-tools/mesa-25.0.7}
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws083-video-roundtrip"
work=$fresh_dir
compiler=${CC:-cc}
mkdir "$work/include" "$work/streams"
ln -s "$repo/include/libc/vulkan" "$work/include/vulkan"

# The libvulkan half: write the streams.
library="$repo/userland/desktop/libvulkan"
"$compiler" -std=gnu89 -Wdeclaration-after-statement -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g \
	-fsanitize=address,undefined -fno-sanitize-recover=all -ffunction-sections -fdata-sections \
	-I"$work/include" -I"$repo/include" -I"$library" \
	"$repo/plan/ws083/tests/host-video-wire.c" "$library/video.c" "$library/objects.c" \
	"$library/wire.c" "$library/codec.c" -Wl,--gc-sections -pthread -o "$work/wire"
"$work/wire" "$work/streams"

# The executor half, as plan/ws031/tests/run-vk-host-tests.sh links it.
driver=$repo/src/drivers/gpu/i915
executor=""
for part in codec object dispatch transport instance vulkan fence objects reply \
    memory image descriptor pipeline pipeline-prepare render-pass sync command \
    state batch math forget video video-mfx video-h264-tables; do
	executor="$executor $driver/render/$part.c"
done
for part in spirv compile eu; do
	executor="$executor $driver/compiler/$part.c"
done
base="-std=gnu11 -Wall -Wextra -Werror -Wdeclaration-after-statement -DKERN_USER_ABI_LP64 -DVK_REPO=\"$repo\" -I$repo/include -I$repo -idirafter $repo/include/libc"
"$compiler" $base -O2 "$repo/plan/ws083/tests/host-video-executor.c" $executor -o "$work/executor-ordinary" -lm
"$work/executor-ordinary" "$work/streams"
"$compiler" $base -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "$repo/plan/ws083/tests/host-video-executor.c" $executor -o "$work/executor-sanitized" -lm
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/executor-sanitized" "$work/streams"

# The batch the IDR and the P picture ran, read back through Mesa's genxml (ws083-p004, D24).
python3 -I "$repo/plan/ws083/tests/genxml-decode.py" --mesa "$mesa" --batch "$work/streams/idr.bin" --expect "$work/streams/idr.expect"
echo "WS083 video round trip PASS (libvulkan streams through the executor, plain and ASan/UBSan)"

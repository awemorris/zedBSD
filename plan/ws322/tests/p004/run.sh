#!/bin/sh
# Host checks of the H.264 decode path (ws322-p004): the parameter sets written back and the slots' ties
# (ps-slots-host-test.c), the Wave5 engine over a stand-in core (engine-host-test.c), and the executor's video wire
# read back from libvulkan's own encoders (wire-host-test.c, with plan/tools/rtx-video/wire-encode.c), then the
# p003 checks (state, executor, p009) as the regression of the rest of the executor.
#   plan/ws322/tests/p004/run.sh OUTDIR
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
root=$(cd -- "$(dirname -- "$0")/../../../.." && pwd)
here=$root/plan/ws322/tests/p004
out=$1
mkdir -p "$out/inc"
ln -sfn "$root/include/libc/vulkan" "$out/inc/vulkan"
san="-fsanitize=address,undefined -fno-sanitize=alignment"
cc -std=c11 -O1 -g -Wall -Wextra -Werror $san -I"$root/include" -I"$root/src" \
	"$root/src/drivers/vpu/vpu-h264-ps.c" "$root/src/drivers/vpu/vpu-bits.c" "$root/src/drivers/vpu/vpu-slots.c" "$root/src/drivers/platform/rpi4/rpi4-h264.c" \
	"$here/ps-slots-host-test.c" -o "$out/ps-slots-host-test"
"$out/ps-slots-host-test"
cc -std=c11 -O1 -g -Wall -Wextra -Werror $san -I"$root/include" -I"$root/src" \
	"$root/src/drivers/vpu/wave5-video.c" "$root/src/drivers/vpu/vpu-h264-ps.c" "$root/src/drivers/vpu/vpu-h265-ps.c" "$root/src/drivers/vpu/vpu-bits.c" "$root/src/drivers/vpu/vpu-slots.c" \
	"$here/engine-host-test.c" -o "$out/engine-host-test"
"$out/engine-host-test"
flags="-std=gnu11 -O1 -g -D_POSIX_C_SOURCE=200809L -DKERN_USER_ABI_LP64 -include time.h -Wall -Wextra -Werror $san -ffunction-sections -fdata-sections \
-I$out/inc -I$root/include -I$root/src -I$root"
cc $flags -Wno-unused-function -c -o "$out/encode.o" "$root/plan/tools/rtx-video/wire-encode.c"
cc $flags -c -o "$out/lv-wire.o" "$root/userland/desktop/libvulkan/wire.c"
cc $flags -c -o "$out/lv-codec.o" "$root/userland/desktop/libvulkan/codec.c"
cc $flags -Wno-unused-function -c -o "$out/wire.o" "$here/wire-host-test.c"
cc $flags -c -o "$out/i915-codec.o" "$root/src/drivers/gpu/i915/vk/vk-codec.c"
cc $flags -c -o "$out/image-state.o" "$root/src/drivers/gpu/powervr/image-state.c"
cc $flags -c -o "$out/vk-video-h265.o" "$root/src/drivers/gpu/powervr/vk/vk-video-h265.c"
cc $san -Wl,--gc-sections -o "$out/wire-host-test" "$out/encode.o" "$out/lv-wire.o" "$out/lv-codec.o" "$out/wire.o" "$out/i915-codec.o" "$out/image-state.o" "$out/vk-video-h265.o"
"$out/wire-host-test"
sh "$root/plan/ws322/tests/p003/run.sh" "$out/p003"

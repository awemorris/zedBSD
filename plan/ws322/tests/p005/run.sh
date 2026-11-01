#!/bin/sh
# Host checks of libvulkan's H.265 decode transfer (ws322-p005): the H.265 wire read back field by field from
# libvulkan's own encoders, the parameter set keys and the profile check (h265-wire-host-test.c), and the video
# gate of the capset bits and the family answers with the device extension list (video-gate-host-test.c).
#   plan/ws322/tests/p005/run.sh OUTDIR
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
root=$(cd -- "$(dirname -- "$0")/../../../.." && pwd)
here=$root/plan/ws322/tests/p005
out=$1
mkdir -p "$out/inc"
ln -sfn "$root/include/libc/vulkan" "$out/inc/vulkan"
san="-fsanitize=address,undefined -fno-sanitize=alignment"
flags="-std=gnu11 -O1 -g -D_POSIX_C_SOURCE=200809L -DKERN_USER_ABI_LP64 -include time.h -Wall -Wextra -Werror $san -ffunction-sections -fdata-sections \
-I$out/inc -I$root/include -I$root/src -I$root"
cc $flags -c -o "$out/lv-wire.o" "$root/userland/desktop/libvulkan/wire.c"
cc $flags -c -o "$out/lv-codec.o" "$root/userland/desktop/libvulkan/codec.c"
cc $flags -Wno-unused-function -c -o "$out/h265-wire.o" "$here/h265-wire-host-test.c"
cc $san -Wl,--gc-sections -o "$out/h265-wire-host-test" "$out/h265-wire.o" "$out/lv-wire.o" "$out/lv-codec.o"
"$out/h265-wire-host-test"
cc $flags -Wno-unused-function -c -o "$out/video-gate.o" "$here/video-gate-host-test.c"
cc $san -Wl,--gc-sections -o "$out/video-gate-host-test" "$out/video-gate.o" "$out/lv-wire.o" "$out/lv-codec.o"
"$out/video-gate-host-test"

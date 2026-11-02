#!/bin/sh
# Host test of libmedia's choice of pictures the GPU holds when the video decode family is not the transfer family
# (gpu-hold-host-test.c, which compiles userland/desktop/libmedia/vkvideo-runtime.c in; ASan and UBSan).
#   sh plan/ws319/tests/gpu-hold/run.sh OUTDIR
# Last line: gpu-hold: PASS or gpu-hold: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
root=$(cd -- "$(dirname -- "$0")/../../../.." && pwd)
out=${1:?usage: run.sh OUTDIR}
mkdir -p "$out/inc"
ln -sfn "$root/include/libc/vulkan" "$out/inc/vulkan"
timeout 300 cc -std=gnu11 -O1 -g -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-unused-function \
	-fsanitize=address,undefined -fno-sanitize-recover=all -ffunction-sections -fdata-sections -Wl,--gc-sections \
	-I"$out/inc" -I"$root" -I"$root/userland/desktop/libmedia" -I"$root/userland/desktop/include" \
	"$root/plan/ws319/tests/gpu-hold/gpu-hold-host-test.c" -lpthread -o "$out/gpu-hold-host-test"
timeout 60 "$out/gpu-hold-host-test"

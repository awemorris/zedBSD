#!/bin/sh
# Host test of the nvrtx H.265 decode's parts that touch no hardware: the H.265 picture structure's fields where the
# public structure puts them, the work buffers' sizes, the push buffer, the scaling list and tile buffers, the
# description, the references by surface and the initial lists, and the slice header's skipped bits.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: plan/ws324/tests/hevc-host-test.sh   (from anywhere)
# The binary goes in a new directory below build/tmp; nothing is removed.
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$repo/build/tmp"
out=$(mktemp -d "$repo/build/tmp/ws324-hevc.XXXXXX")
src=$repo/src/drivers/gpu/nvrtx
cc -std=gnu11 -O1 -g -DKERN_USER_ABI_LP64 -Wall -Wextra -Werror -Wdeclaration-after-statement \
	-fsanitize=address,undefined -fno-sanitize=alignment \
	-I"$repo/include" -I"$repo/src" -I"$repo" \
	-o "$out/hevc-host-test" \
	"$repo/plan/ws324/tests/hevc-host-test.c" \
	"$src/nvdec-records.c" "$src/vk/vk-video-h265.c" "$src/wire.c"
"$out/hevc-host-test"

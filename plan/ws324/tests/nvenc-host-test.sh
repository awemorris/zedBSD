#!/bin/sh
# Host test of the nvrtx video encoder's channel parts that touch no hardware: the classes, the bring-up's push
# buffers, the resource manager's parameters and the device table's pick of the encoder; then the video decoder's
# host test (plan/tools/rtx-video/nvdec-host-test.sh), whose device table pick the encoder's shares.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: plan/ws324/tests/nvenc-host-test.sh   (from anywhere)
# The binary goes in a new directory below build/tmp; nothing is removed.
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$repo/build/tmp"
out=$(mktemp -d "$repo/build/tmp/ws324-nvenc.XXXXXX")
src=$repo/src/drivers/gpu/nvrtx
cc -std=gnu11 -O1 -g -DKERN_USER_ABI_LP64 -Wall -Wextra -Werror -Wdeclaration-after-statement \
	-fsanitize=address,undefined -fno-sanitize=alignment \
	-I"$repo/include" -I"$repo/src" -I"$repo" \
	-o "$out/nvenc-host-test" \
	"$repo/plan/ws324/tests/nvenc-host-test.c" \
	"$src/nvenc-records.c" "$src/objects.c" "$src/wire.c"
"$out/nvenc-host-test"
sh "$repo/plan/tools/rtx-video/nvdec-host-test.sh"

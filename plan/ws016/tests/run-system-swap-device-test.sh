#!/bin/sh
# SWAP-T007/T008 production /dev/system swap-boundary host runner.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
binary=$(mktemp "${TMPDIR:-/tmp}/zedbsd-system-swap.XXXXXX")
object32=$(mktemp "${TMPDIR:-/tmp}/zedbsd-swap-uapi32.XXXXXX.o")
object64=$(mktemp "${TMPDIR:-/tmp}/zedbsd-swap-uapi64.XXXXXX.o")
trap 'rm -f "$binary" "$object32" "$object64"' EXIT HUP INT TERM

cd "$repo_dir"
"${CC:-cc}" -std=c11 -m32 -ffreestanding -Iinclude/uapi -Ilibc/include \
	-Wall -Wextra -Werror -c \
	plan/ws016/tests/swap-uapi-layout.c -o "$object32"
"${CC:-cc}" -std=c11 -m64 -ffreestanding -Iinclude/uapi -Ilibc/include \
	-Wall -Wextra -Werror -c \
	plan/ws016/tests/swap-uapi-layout.c -o "$object64"
"${CC:-cc}" -std=c11 -Iinclude -Iinclude/uapi -I. \
	-Wall -Wextra -Werror -ffunction-sections -fdata-sections \
	src/drivers/generic/system-device.c \
	plan/ws016/tests/system-swap-device-test.c \
	-Wl,--gc-sections -o "$binary"
"$binary"

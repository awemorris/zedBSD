#!/bin/sh
# SWAP-T005 production VM-drain host runner.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
binary=$(mktemp "${TMPDIR:-/tmp}/zedbsd-swap-drain.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM

cd "$repo_dir"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L \
	-Iinclude -Iinclude/uapi -Isrc \
	-Wall -Wextra -Werror -ffunction-sections -fdata-sections -pthread \
	src/kern/swap.c src/kern/vm-reclaim.c \
	plan/ws016-swap-control/tests/swap-drain-test.c \
	-Wl,--gc-sections -o "$binary"
"$binary"

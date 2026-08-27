#!/bin/sh
# SWAP-T006 production VM commitment resize host runner.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
binary=$(mktemp "${TMPDIR:-/tmp}/zedbsd-swap-commit.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM

cd "$repo_dir"
"${CC:-cc}" -std=c11 -Iinclude -Iinclude/uapi -I. \
	-Wall -Wextra -Werror -ffunction-sections -fdata-sections \
	src/kern/vm-commit.c \
	plan/ws016-swap-control/tests/swap-commit-resize-test.c \
	-Wl,--gc-sections -o "$binary"
"$binary"

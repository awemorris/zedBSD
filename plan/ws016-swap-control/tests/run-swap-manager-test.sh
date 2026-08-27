#!/bin/sh
# SWAP-T001/T002 production swap-manager host runner.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
noct=${NOCT:-$repo_dir/build/NoctLang/build-static/noct}
v1=$(mktemp "${TMPDIR:-/tmp}/zedbsd-swap-v1.XXXXXX")
v2=$(mktemp "${TMPDIR:-/tmp}/zedbsd-swap-v2.XXXXXX")
binary=$(mktemp "${TMPDIR:-/tmp}/zedbsd-swap-manager.XXXXXX")
trap 'rm -f "$v1" "$v2" "$binary"' EXIT HUP INT TERM

if [ ! -x "$noct" ]; then
	printf '%s\n' "SWAP-T001/T002: missing Noct tool: $noct" >&2
	exit 1
fi

cd "$repo_dir"
"$noct" --path=tools/build tools/build/make-swapfile.noct \
	--format v1 --size-mib 32 --output "$v1"
"$noct" --path=tools/build tools/build/make-swapfile.noct \
	--format v2 --size-mib 1 --uuid 0123456789ABCDEF \
	--label TESTSWAP --output "$v2"

"${CC:-cc}" -std=c11 -Iinclude -Iinclude/uapi -I. \
	-Wall -Wextra -Werror -ffunction-sections -fdata-sections -pthread \
	src/kern/swap.c src/kern/backing-claim.c src/kern/swap-source.c \
	plan/ws003-bringup/tests/swap-source-test.c \
	-Wl,--gc-sections -o "$binary"
"$binary" "$v1" "$v2"

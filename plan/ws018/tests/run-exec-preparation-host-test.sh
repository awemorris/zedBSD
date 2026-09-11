#!/bin/sh
# KA-T040 production exec-preparation host runner.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
source_file=${1:-${KA_T040_SOURCE:-src/kern/exec-prepare.c}}
binary=$(mktemp "${TMPDIR:-/tmp}/zedbsd-exec-preparation.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM

case "$source_file" in
/*) ;;
*) source_file=$repo_dir/$source_file ;;
esac

if [ ! -f "$source_file" ]; then
	echo "KA-T040: source not found: $source_file" >&2
	exit 2
fi

"${CC:-cc}" -std=c11 \
	-DZEDBSD_USER_ABI_LP64 \
	-I"$repo_dir/include" -I"$repo_dir/include/uapi" \
	-I"$repo_dir/src" -I"$repo_dir/libc/include" -I"$repo_dir" \
	-Wall -Wextra -Werror -ffunction-sections -fdata-sections \
	"$source_file" "$test_dir/exec-preparation-host-test.c" \
	-Wl,--gc-sections -o "$binary"
"$binary"

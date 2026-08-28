#!/bin/sh
# KA-T010 production disk-label parser regression runner.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-disklabel.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

parser_sources="
	$repo_dir/src/drivers/disklabel/mbr.c
	$repo_dir/src/drivers/disklabel/pc98.c
	$repo_dir/src/drivers/disklabel/pc98-auto.c
	$repo_dir/src/drivers/disklabel/sun.c
	$repo_dir/src/drivers/disklabel/x68k.c"

# shellcheck disable=SC2086
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
	-I"$repo_dir/include" -I"$repo_dir/include/uapi" -I"$repo_dir/src" \
	-I"$repo_dir/libc/include" \
	"$test_dir/disklabel-host-test.c" $parser_sources \
	-o "$temporary/disklabel-host-test"
"$temporary/disklabel-host-test"

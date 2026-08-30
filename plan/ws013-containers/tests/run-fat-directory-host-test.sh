#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/fat-directory-test.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
compiler=${HOSTCC:-cc}
common="-std=c11 -O2 -Wall -Wextra -Werror -I$repo"

$compiler $common \
	"$repo/bootloader/bios/fat-directory.c" \
	"$repo/plan/ws013-containers/tests/fat-directory-host-test.c" \
	-o "$temporary/test"
"$temporary/test"

$compiler $common -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined \
	"$repo/bootloader/bios/fat-directory.c" \
	"$repo/plan/ws013-containers/tests/fat-directory-host-test.c" \
	-o "$temporary/test-sanitize"
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$temporary/test-sanitize"

if $compiler -std=c11 -fanalyzer -x c -c /dev/null \
	-o "$temporary/analyzer-probe.o" 2>/dev/null; then
	$compiler $common -fanalyzer -c \
		"$repo/bootloader/bios/fat-directory.c" \
		-o "$temporary/analyzer.o"
fi

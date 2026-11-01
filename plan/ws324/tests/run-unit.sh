#!/bin/sh
# The host unit tests of libmedia's H.265 parse (h265sw-*, h265-dpb, h265-stream, h265-std): each test
# plan/ws324/tests/unit/test-<name>.c is built with every h265sw-*.c and h265-*.c (modules not implemented yet are
# skeletons that answer ENOSYS), vcodec-bitreader.c, vcodec-nal.c and bits.c, with the host's cc, -std=c89 -O1 -g
# -Wall -Wextra -Werror, ASan and UBSan, and run.  The include path is the tree's root, plan/tools/h264sw/unit
# (check.h, bitwriter.h) and plan/ws324/tests/unit (h265write.h); <vulkan/vulkan.h> is the host's.
# Each test's last line is "test-<name>: PASS (<n> checks)" or "test-<name>: FAIL (...)".
#   sh plan/ws324/tests/run-unit.sh <name>...     one or more tests
#   sh plan/ws324/tests/run-unit.sh all           every test present
# H265_UNIT_DIR names another directory of test-<name>.c (for checking this tool itself; default
# plan/ws324/tests/unit).
# Last line: run-unit: PASS or run-unit: FAIL.  Output under build/tmp/h265-unit (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/h265-unit
fresh_out "$out"
units=${H265_UNIT_DIR:-plan/ws324/tests/unit}

names="$*"
if [ "$names" = "all" ]; then
	names=""
	for file in "$units"/test-*.c; do
		[ -f "$file" ] || continue
		name=$(basename "$file" .c)
		names="$names ${name#test-}"
	done
fi
if [ -z "$names" ]; then
	echo "usage: sh plan/ws324/tests/run-unit.sh <name>... | all"
	exit 2
fi

lib=userland/desktop/libmedia
modules=""
for file in $lib/h265sw-*.c $lib/h265-*.c; do
	[ -f "$file" ] && modules="$modules $file"
done
modules="$modules $lib/vcodec-bitreader.c $lib/vcodec-nal.c $lib/bits.c"

status=0
for name in $names; do
	source=$units/test-$name.c
	if [ ! -f "$source" ]; then
		echo "test-$name: FAIL (no $source)"
		status=1
		continue
	fi
	if ! cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
	    -fno-sanitize-recover=all -fno-omit-frame-pointer -I. -Iplan/tools/h264sw/unit -Iplan/ws324/tests/unit \
	    "$source" $modules -lm -o "$out/test-$name" > "$out/test-$name.build.log" 2>&1; then
		echo "test-$name: FAIL (build)"
		head -20 "$out/test-$name.build.log"
		status=1
		continue
	fi
	if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	    timeout 600 "$out/test-$name" > "$out/test-$name.log" 2>&1; then
		last=$(tail -1 "$out/test-$name.log")
		case "$last" in
		"test-$name: PASS"*) echo "$last" ;;
		*)
			echo "test-$name: FAIL (exit 0 without a PASS line; log $out/test-$name.log)"
			status=1 ;;
		esac
	else
		echo "test-$name: FAIL (exit; log $out/test-$name.log)"
		grep -v '^ok ' "$out/test-$name.log" | head -20
		status=1
	fi
done

if [ "$status" = 0 ]; then echo "run-unit: PASS"; else echo "run-unit: FAIL"; fi
exit "$status"

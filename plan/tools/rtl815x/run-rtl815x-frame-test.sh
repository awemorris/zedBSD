#!/bin/sh
# Builds plan/tools/rtl815x/rtl815x-frame-test.c with src/drivers/usb/usb-rtl815x-frame.c on the host (HOSTCC, default cc;
# again with ASan and UBSan when the compiler has them) and runs it.  Outputs go to OUT (default build/rtl815x-frame,
# a new directory each run through plan/tools/fresh-out.sh).  The last line is "rtl815x-frame: PASS" from each run.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
fresh_out "${OUT:-build/rtl815x-frame}"
out=${OUT:-build/rtl815x-frame}
compiler=${HOSTCC:-cc}
"$compiler" -std=c11 -O2 -Wall -Wextra -Werror -Iinclude src/drivers/usb/usb-rtl815x-frame.c \
	plan/tools/rtl815x/rtl815x-frame-test.c -o "$out/rtl815x-frame-test"
"$out/rtl815x-frame-test"
if "$compiler" -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=address,undefined \
	-Iinclude src/drivers/usb/usb-rtl815x-frame.c plan/tools/rtl815x/rtl815x-frame-test.c \
	-o "$out/rtl815x-frame-test-sanitized" 2>/dev/null; then
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$out/rtl815x-frame-test-sanitized"
else
	echo "rtl815x-frame: sanitizer run skipped (compiler unavailable)"
fi

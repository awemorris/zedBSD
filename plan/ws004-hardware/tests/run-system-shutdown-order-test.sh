#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-shutdown-order.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -ffunction-sections \
	-fdata-sections -I"$repo/include" -I"$repo/include/uapi" \
	-I"$repo/src" \
	"$repo/src/kern/shutdown.c" \
	"$repo/plan/ws004-hardware/tests/system-shutdown-order-test.c" \
	-Wl,--gc-sections -o "$temporary/system-shutdown-order-test"
"$temporary/system-shutdown-order-test"

# Both externally reachable orderly-stop paths must cross the common boundary.
grep -A5 'case ZEDBSD_SYSTEM_HALT:' "$repo/src/kern/system-device.c" |
	grep -q 'system_shutdown_prepare();'
grep -A5 'case ZEDBSD_SYSTEM_REBOOT:' "$repo/src/kern/system-device.c" |
	grep -q 'system_shutdown_prepare();'
grep -A2 'streq(v\[0\], "halt")' "$repo/src/kern/shell.c" |
	grep -q 'system_shutdown_prepare();'
grep -A2 'streq(v\[0\], "reboot")' "$repo/src/kern/shell.c" |
	grep -q 'system_shutdown_prepare();'

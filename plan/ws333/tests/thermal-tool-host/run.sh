#!/bin/sh
# Builds and runs the host test of /sbin/thermal's CPU list (userland/base/thermal/main.c with open and ioctl
# replaced by a model of /dev/thermal).
# Usage: run.sh <out-dir>   Last line: thermal-tool-host: PASS or FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
out=${1:?out dir}
mkdir -p "$out"
cc=${CC:-cc}
$cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -I"$repo/include" -Dmain=thermal_main -Dopen=test_open -Dioctl=test_ioctl \
	-include fcntl.h -include sys/ioctl.h -c "$repo/userland/base/thermal/main.c" -o "$out/main.o"
$cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -I"$repo/include" -c "$here/thermal-tool-host-test.c" -o "$out/test.o"
$cc -o "$out/thermal-tool-host-test" "$out/main.o" "$out/test.o"
timeout 60 "$out/thermal-tool-host-test" "$out"

#!/bin/sh
# Builds and runs the host test of the x86 processor driver
# (src/drivers/platform/pcat/cpu-x86/cpu-x86*.c) twice: with the performance
# control (CONFIG_CPU_PERF=1, every scenario) and without it (the description
# only; the build links no RDMSR or WRMSR, so a reference to one fails it).
# Usage: run.sh <out-dir>   (CPU_X86_SRC=<dir> tests a copy of the driver, SANITIZE=<flags> adds sanitizers)
# Last line: cpu-x86-host: PASS or cpu-x86-host: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
out=${1:?out dir}
mkdir -p "$out"
cc=${CC:-cc}
src=${CPU_X86_SRC:-$repo/src/drivers/platform/pcat/cpu-x86}
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -DHAL_ARCH_AMD64 -I$repo/include -I$repo/src -I$src"
status=0

if ! $cc $flags -DCONFIG_CPU_PERF=1 ${SANITIZE:-} -o "$out/cpu-x86-host-y" \
	"$src/cpu-x86.c" "$src/cpu-x86-control.c" "$src"/cpux86*.c "$here/cpu-x86-host-test.c" \
	> "$out/build-y.log" 2>&1; then
	echo "build with the control: FAILED"; head -30 "$out/build-y.log"; status=1
elif ! timeout 60 "$out/cpu-x86-host-y"; then
	status=1
fi

if ! $cc $flags -DCONFIG_CPU_PERF=0 ${SANITIZE:-} -o "$out/cpu-x86-host-n" \
	"$src/cpu-x86.c" "$src/cpux86.c" "$src/cpux86-cpuid.c" "$here/cpu-x86-host-test.c" \
	> "$out/build-n.log" 2>&1; then
	echo "build without the control: FAILED"; head -30 "$out/build-n.log"; status=1
elif ! timeout 60 "$out/cpu-x86-host-n"; then
	status=1
fi

if [ "$status" = 0 ]; then echo "cpu-x86-host: PASS"; else echo "cpu-x86-host: FAIL"; fi
exit "$status"

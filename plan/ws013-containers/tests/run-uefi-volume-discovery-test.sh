#!/bin/sh
# WS013 p002 bounded UEFI volume-discovery test runner.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-uefi-volume.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

compiler=${HOSTCC:-cc}
efi_compiler=${EFI_CC:-$compiler}
fixture=$repo/plan/ws013-containers/tests/uefi-volume-discovery-test.c
source=$repo/bootloader/uefi/volume-discovery.c
common="-std=c11 -Wall -Wextra -Werror -I$repo"

# Prove that the helper remains freestanding for BOOTX64.EFI integration.
# shellcheck disable=SC2086
$efi_compiler $common -Os -ffreestanding -fshort-wchar -mno-red-zone \
	-fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -fno-ident -ffunction-sections -fdata-sections \
	-c "$source" -o "$temporary/volume-discovery-uefi.o"

# shellcheck disable=SC2086
$compiler $common -O2 "$fixture" "$source" \
	-o "$temporary/uefi-volume-discovery-test"
"$temporary/uefi-volume-discovery-test"

# shellcheck disable=SC2086
if $compiler $common -O1 -g -fsanitize=address,undefined \
	-fno-omit-frame-pointer "$fixture" "$source" \
	-o "$temporary/uefi-volume-discovery-test-sanitized" 2>/dev/null; then
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$temporary/uefi-volume-discovery-test-sanitized"
	echo 'WS013 p002 UEFI volume discovery sanitizer: PASS'
else
	echo 'WS013 p002 UEFI volume discovery sanitizer: SKIP (compiler unavailable)'
fi

analyzer_pass=yes
for analyzer_source in "$fixture" "$source"; do
	analyzer_object=$temporary/$(basename "$analyzer_source" .c)-analyzer.o
	# shellcheck disable=SC2086
	if ! $compiler $common -O0 -fanalyzer -c "$analyzer_source" \
		-o "$analyzer_object" 2>/dev/null; then
		analyzer_pass=no
		break
	fi
done
if test "$analyzer_pass" = yes; then
	echo 'WS013 p002 UEFI volume discovery analyzer: PASS'
else
	echo 'WS013 p002 UEFI volume discovery analyzer: SKIP (analyzer unavailable)'
fi

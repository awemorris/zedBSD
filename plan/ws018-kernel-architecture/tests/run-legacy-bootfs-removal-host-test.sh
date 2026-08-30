#!/bin/sh
# KA-T110: legacy bootfs/startup removal and kernel metadata ownership.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-legacy-removal.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

fail()
{
	echo "KA-T110: FAIL: $*" >&2
	exit 1
}

for removed in \
	include/kern/env.h \
	include/kern/fs.h \
	include/kern/namespace.h \
	src/kern/device.c \
	src/kern/env.c \
	src/kern/fs.c \
	src/kern/internal.h \
	src/kern/namespace.c \
	src/kern/shell.c \
	src/kern/startup.c
do
	[ ! -e "$repo_dir/$removed" ] || fail "retired file remains: $removed"
done

legacy_pattern='struct[[:space:]]+bootfs|struct[[:space:]]+boot_volume|enum[[:space:]]+bootfs_result|bootfs_[[:alnum:]_]*|boot_volume_(read|write)|kern/(fs|namespace|env)\.h|kern/internal\.h|ZEDBSD_M9_WRITE_TEST|M9_STAGE2_OBJS|stage2-m9|shell-m9-test|device-m9-test|src/kern/(fs|namespace|device|startup|shell|env)\.c'
if rg -n -e "$legacy_pattern" \
	"$repo_dir/Makefile" "$repo_dir/platform" "$repo_dir/src" \
	"$repo_dir/include" "$repo_dir/bootloader" "$repo_dir/userland" \
	"$repo_dir/libc" "$repo_dir/config"
then
	fail "legacy definition, include, object, or target remains active"
fi

# These maintained entry points formerly compiled or inspected the retired
# implementation.  Historical closed Queue books are intentionally outside
# this active-test audit.
if rg -n -e "$legacy_pattern" \
	"$test_dir/run-fat-consolidation-host-test.sh" \
	"$test_dir/run-filesystem-identity-host-test.sh" \
	"$repo_dir/plan/ws004-hardware/tests/run-system-shutdown-order-test.sh"
then
	fail "a maintained test still consumes retired source"
fi

grep -q '#include "kern/kernel.h"' "$repo_dir/src/kern/system-device.c" ||
	fail "/dev/system does not include the focused kernel metadata owner"
for accessor in kern_boot_bios_id kern_boot_device_count kern_boot_device_at
do
	grep -q "$accessor" "$repo_dir/include/kern/kernel.h" ||
		fail "kernel metadata declaration missing: $accessor"
	grep -q "$accessor" "$repo_dir/src/kern/main.c" ||
		fail "kernel metadata owner missing: $accessor"
	grep -q "$accessor" "$repo_dir/src/kern/system-device.c" ||
		fail "/dev/system consumer missing: $accessor"
done
grep -q 'static struct boot_device devices\[KERN_PLATFORM_MAX_DEVICES\]' \
	"$repo_dir/src/kern/entry.c" ||
	fail "the borrowed boot-device table does not have kernel lifetime"

common_flags="-std=c11 -DZEDBSD_USER_ABI_LP64 -Wall -Wextra -Werror \
-ffunction-sections -fdata-sections -I$repo_dir/include \
-I$repo_dir/include/uapi -I$repo_dir"

build_and_run()
{
	name=$1
	extra_flags=$2
	# shellcheck disable=SC2086
	"${CC:-cc}" $common_flags $extra_flags \
		"$repo_dir/src/kern/main.c" \
		"$test_dir/kernel-boot-metadata-host-test.c" \
		-Wl,--gc-sections -o "$temporary/ka-t110-$name"
	if [ "$name" = sanitize ]; then
		ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
			"$temporary/ka-t110-$name"
	else
		"$temporary/ka-t110-$name"
	fi
}

build_and_run ordinary "-O0"
build_and_run sanitize \
	"-O0 -g -fno-omit-frame-pointer -fsanitize=address,undefined"

echo "KA-T110: PASS (legacy source/build audit)"

#!/bin/sh
# Production-linked threaded regression; every failing cell fails the runner.
set -eu
python3 "$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)/plan/ws025/tests/prepare-driver-fragments.py"
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "$repo/plan/ws018/temp/ufs-audit.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
result=0
for version in 2; do
	for mode in ordinary sanitize; do
		extra=""
		if test "$mode" = sanitize; then
			extra="-fsanitize=address,undefined -fno-omit-frame-pointer --param asan-globals=0"
		fi
		${HOSTCC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror -pthread $extra \
			-c "$repo/plan/ws018/tests/mount-thread-host.c" \
			-o "$temporary/thread.o"
		# shellcheck disable=SC2086
		${HOSTCC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror \
			-ffunction-sections -fdata-sections -DZEDBSD_USER_ABI_LP64 \
			-DUFS_AUDIT_VERSION=$version $extra -I"$repo/include" \
			-I"$repo/include/uapi" -I"$repo/src" -I"$repo/libc/include" \
			"$repo/plan/ws018/tests/ufs-metadata-audit.c" \
			"$repo/plan/ws025/temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-endian.c" \
			"$repo/src/kern/quota.c" "$repo/src/kern/io-stats.c" \
			"$temporary/thread.o" -pthread -Wl,--gc-sections -o "$temporary/ufs$version-$mode"
		status=0
		ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
			timeout 45s "$temporary/ufs$version-$mode" || status=$?
		if test "$status" -gt 1; then exit "$status"; fi
		if test "$status" -eq 1; then result=1; fi
	done
done
exit "$result"

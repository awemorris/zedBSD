#!/bin/sh
# ws035-p035: the host/native switch of include/uapi (include/uapi/hosted.h).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: sh plan/ws035/tests/run-uapi-hosted-test.sh
# Environment: CC (default cc).  Each step is limited to 60 seconds.
set -u
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cc=${CC:-cc}
work=$(mktemp -d "${TMPDIR:-/tmp}/uapi-hosted.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
test_c=$repo/plan/ws035/tests/uapi-hosted-test.c
warn="-std=c11 -Wall -Wextra -Werror"
status=0

run()
{
	name=$1; shift
	if timeout 60 "$@" > "$work/$name.log" 2>&1; then
		echo "$name: PASS"
	else
		echo "$name: FAIL"; sed -n 1,20p "$work/$name.log"; status=1
	fi
}

# 1. host: host and uapi headers together; the standard names are the host's.
run host-build $cc $warn -D_POSIX_C_SOURCE=200809L -DUAPI_TEST_HOST -I"$repo/include" "$test_c" -o "$work/host"
run host-run "$work/host"
# 2. host with include/uapi itself on the path (fixtures that use -Iinclude/uapi):
#    <errno.h> etc. then name the uapi headers, which pass on to the host's.
run host-uapi-dir-build $cc $warn -D_POSIX_C_SOURCE=200809L -DUAPI_TEST_HOST -DUAPI_TEST_SHADOW -I"$repo/include" -I"$repo/include/uapi" "$test_c" -o "$work/host2"
run host-uapi-dir-run "$work/host2"
# 3. the same with -pedantic (the #include_next extension is kept quiet).
run host-pedantic $cc $warn -pedantic -D_POSIX_C_SOURCE=200809L -DUAPI_TEST_HOST -DUAPI_TEST_SHADOW -I"$repo/include" -I"$repo/include/uapi" -c "$test_c" -o "$work/host3.o"
# 4. native: zedBSD's values from the uapi headers alone.
run native $cc $warn -DKERN_UAPI_NATIVE -DKERN_USER_ABI_LP64 -I"$repo/include" -c "$test_c" -o "$work/native.o"
# 5. the zedBSD C library headers with KERN_UAPI_NATIVE.
run libc-native $cc $warn -DKERN_UAPI_NATIVE -DKERN_USER_ABI_LP64 -DUAPI_TEST_LIBC -I"$repo/include/libc" -I"$repo/include" -c "$test_c" -o "$work/libc.o"
# 6. the zedBSD C library headers without KERN_UAPI_NATIVE: the #error.
if timeout 60 $cc $warn -DKERN_USER_ABI_LP64 -DUAPI_TEST_LIBC -I"$repo/include/libc" -I"$repo/include" -c "$test_c" -o "$work/libc-host.o" > "$work/libc-host.log" 2>&1; then
	echo "libc-without-native: FAIL (compiled)"; status=1
elif grep -q 'define KERN_UAPI_NATIVE' "$work/libc-host.log"; then
	echo "libc-without-native: PASS (rejected: $(grep -m1 -o 'error: .*define KERN_UAPI_NATIVE' "$work/libc-host.log"))"
else
	echo "libc-without-native: FAIL (other error)"; sed -n 1,20p "$work/libc-host.log"; status=1
fi
if [ "$status" = 0 ]; then echo "uapi-hosted test: PASS"; else echo "uapi-hosted test: FAIL"; fi
exit "$status"

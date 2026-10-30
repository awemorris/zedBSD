#!/bin/sh
# libkeiland's threads and network on FreeBSD 15.1 (QEMU+KVM, plan/tools/keiland-freebsd/): the native build of
# Keiland (backend-test.sh: libkeiland, Settings, the input method and the rest, exit 0 and no warning), then the
# layers' own tests (thread-test.c, socket-test.c) built in the guest with its clang, linked with the built
# libkeiland, and with the layers' sources under AddressSanitizer and UndefinedBehaviorSanitizer.  Run from the
# worktree's root:
#   GUEST_DIR=DIR sh plan/tools/keiland-layers/freebsd-layers.sh OUT
# GUEST_DIR is a prepared guest (build-guest.sh; another checkout's, read only: a fresh overlay is made in
# GUEST_RUN).  GUEST_RUN (default build/ws327-freebsd-run) and SSH_PORT (default 2247) keep it apart from other
# guests.  The judgement is the guest's exit status and the tests' lines over SSH; no console is read.  Prints
# backend-test's last line, one line per test build, and "ws327-freebsd: PASS" or "FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: freebsd-layers.sh OUT}
: "${GUEST_DIR:?GUEST_DIR names the prepared guest}"
GUEST_RUN=${GUEST_RUN:-$PWD/build/ws327-freebsd-run}
SSH_PORT=${SSH_PORT:-2247}
export GUEST_DIR GUEST_RUN SSH_PORT
mkdir -p "$out"
g() { python3 -I plan/tools/keiland-freebsd/guest.py "$@"; }
failed=0

timeout 600 python3 -I plan/tools/keiland-freebsd/guest.py start > "$out/start.log" 2>&1 ||
	{ echo "ws327-freebsd: the guest did not start ($out/start.log)"; exit 1; }

# The native build and the backend's tests, with this workstream's tests copied too.
BACKEND_TEST_EXTRA_PATHS=plan/tools/keiland-layers timeout 5400 sh plan/tools/keiland-freebsd/backend-test.sh "$PWD/$out/backend" \
	> "$out/backend.log" 2>&1 || failed=1
echo "$(tail -n 1 "$out/backend.log")"

# The layers' tests: linked with the built libkeiland, and with the layers' sources under the sanitizers.
src=/root/keiland-src
flags="-std=gnu17 -Wall -Wextra -Werror -O1 -g -Iuserland/desktop/include"
layers="userland/desktop/libkeiland/platform/thread-posix.c userland/desktop/libkeiland/platform/socket-posix.c"
GUEST_COMMAND_TIMEOUT=60 g ssh "cd $src && find build/native -name 'lib*.so*' | sed 's#/[^/]*\$##' | sort -u" > "$out/lib.txt" 2>&1
# libkeiland.so is in build/native/lib (keiland-freebsd.mk).
lib=build/native/lib
# Every directory of the build's libraries (libkeiland needs libtruetype and the rest of the build's own).
path=$(sed "s#^#$src/#" "$out/lib.txt" | paste -s -d : -)
for test in thread socket; do
	for kind in lib asan; do
		case $kind in
		lib) build="cc $flags plan/tools/keiland-layers/$test-test.c -o /tmp/$test-$kind -L$lib -lkeiland -pthread -Wl,-rpath,$src/$lib" ;;
		asan) build="cc $flags -fsanitize=address,undefined plan/tools/keiland-layers/$test-test.c $layers -o /tmp/$test-$kind -pthread" ;;
		esac
		GUEST_COMMAND_TIMEOUT=400 g ssh "cd $src && $build && LD_LIBRARY_PATH=$path /tmp/$test-$kind > /tmp/$test-$kind.log 2>&1; echo status=\$?; cat /tmp/$test-$kind.log" \
			> "$out/$test-$kind.log" 2>&1
		status=$(sed -n 's/^status=//p' "$out/$test-$kind.log" | head -n 1)
		echo "$test-$kind: status=${status:-none} pass=$(grep -c '^PASS' "$out/$test-$kind.log") fail=$(grep -c '^FAIL' "$out/$test-$kind.log") not-done=$(grep -c '^NOT-DONE' "$out/$test-$kind.log")"
		[ "$status" = 0 ] || failed=1
		grep -q '^FAIL' "$out/$test-$kind.log" && failed=1
	done
done

GUEST_COMMAND_TIMEOUT=60 g ssh 'freebsd-version -ku; uname -m' > "$out/version.txt" 2>&1
timeout 120 python3 -I plan/tools/keiland-freebsd/guest.py stop > "$out/stop.log" 2>&1
[ $failed = 0 ] && echo "ws327-freebsd: PASS" || echo "ws327-freebsd: FAIL"
[ $failed = 0 ]

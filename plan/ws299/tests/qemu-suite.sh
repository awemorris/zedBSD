#!/bin/sh
# ws299: the QEMU regression of the buffer cache, the NVMe paths and the UFS writes, one test after another, each
# with its own image (a serial image, plan/tools/posix/config-amd64-serial.mk with /root/directio and /root/randread,
# and the same with a kernel of config-qemu-cache128.mk and config-qemu-cache32.mk in its boot FAT).
#   sh plan/ws299/tests/qemu-suite.sh SERIAL_IMAGE CACHE128_IMAGE CACHE32_IMAGE [TEST...]
# TEST: cache-stress readahead nvme-shapes directio cache32-write boot (default all).  Each test's own last line is
# printed; the last line is "qemu-suite: PASS|FAIL (failed: ...)".  WS299_RUNS (default /tmp/ws299-runs, a tmpfs, so
# that the tests' disks do not fill the build host's disk) is where each test's fresh run directory goes.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname "$0")/../../.."
SERIAL=${1:?serial image}
C128=${2:?cache128 image}
C32=${3:?cache32 image}
shift 3
TESTS=${*:-cache-stress readahead nvme-shapes directio cache32-write boot}
T=plan/ws299/tests
WS299_RUNS=${WS299_RUNS:-/tmp/ws299-runs}
export WS299_RUNS
failed=
for test in $TESTS; do
	case $test in
	cache-stress) line=$(timeout 1800 sh $T/qemu-cache-stress.sh "$C128" 2>&1 | tail -n 1) ;;
	readahead) line=$(timeout 1800 sh $T/qemu-readahead.sh "$SERIAL" 2>&1 | tail -n 1) ;;
	nvme-shapes) line=$(timeout 1800 sh $T/qemu-nvme-shapes.sh "$SERIAL" 2>&1 | tail -n 1) ;;
	directio) line=$(timeout 1800 sh $T/qemu-directio.sh "$SERIAL" 2>&1 | tail -n 1) ;;
	cache32-write) line=$(timeout 1800 sh $T/qemu-cache32-write.sh "$C32" "" 400 2>&1 | tail -n 1) ;;
	boot) line=$(OUTPUT=${BOOT_OUTPUT:-$WS299_RUNS/boot-test} timeout 900 bash plan/tools/boot-test.sh "$SERIAL" 2>&1 | tail -n 1) ;;
	*) line="$test: unknown" ;;
	esac
	echo "$test: $line"
	case $line in
	*PASS*) ;;
	*) failed="$failed $test" ;;
	esac
done
if [ -z "$failed" ]; then
	echo "qemu-suite: PASS"
else
	echo "qemu-suite: FAIL (failed:$failed)"
	exit 1
fi

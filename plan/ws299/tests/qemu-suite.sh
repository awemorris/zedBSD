#!/bin/sh
# ws299: the QEMU regression of the buffer cache, the NVMe paths and the UFS writes, one test after another.  One
# serial image (config-qemu-serial.mk, with /root/directio, /root/randread and /root/patwrite from build-randread.sh)
# and two kernels of the same tree with a 128 MiB and a 32 MiB buffer cache (config-qemu-cache128.mk,
# config-qemu-cache32.mk); the suite puts each kernel into its own copy of the image (image-kernel.sh).
#   sh plan/ws299/tests/qemu-suite.sh SERIAL_IMAGE CACHE128_VMUNIX CACHE32_VMUNIX [TEST...]
# TEST: cache-stress readahead nvme-shapes directio cache32-write grow-reboot ufs-crash boot (default all).  Each
# test's own last line is printed; the last line is "qemu-suite: PASS|FAIL (failed: ...)".  WS299_RUNS (default
# /tmp/ws299-runs, a tmpfs, so that the tests' disks do not fill the build host's disk) is where each test's fresh run
# directory and the image copy go.  WS299_NAME names the QEMU processes, GDB_PORT and QMP_PORT the TCP ports of the
# tests that open them; set them apart when two suites run at once.
# Build, from the tree's top (after make CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached and sysroot):
#   sh plan/ws299/tests/build-randread.sh OUT
#   plan/tools/guest/test-image.sh --no-harness plan/ws299/tests/config-qemu-serial.mk B/serial \
#     --file /root/directio=OUT/directio-zedbsd --mode /root/directio=755 (the same for randread and patwrite)
#   make CONFIGMK=plan/ws299/tests/config-qemu-cache128.mk BUILD=B/c128 B/c128/vmunix   (and cache32 into B/c32)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname "$0")/../../.."
SERIAL=${1:?serial image}
K128=${2:?cache128 vmunix}
K32=${3:?cache32 vmunix}
shift 3
TESTS=${*:-cache-stress readahead nvme-shapes directio cache32-write grow-reboot ufs-crash boot}
T=plan/ws299/tests
WS299_RUNS=${WS299_RUNS:-/tmp/ws299-runs}
export WS299_RUNS
mkdir -p "$WS299_RUNS"
C128=$WS299_RUNS/suite-cache128.img
case " $TESTS " in
*" cache-stress "*) sh $T/image-kernel.sh "$SERIAL" "$K128" "$C128" > /dev/null || exit 1 ;;
esac
failed=
for test in $TESTS; do
	case $test in
	cache-stress) line=$(timeout 2700 sh $T/qemu-cache-stress.sh "$C128" 2>&1 | tail -n 1) ;;
	readahead) line=$(timeout 1800 sh $T/qemu-readahead.sh "$SERIAL" 2>&1 | tail -n 1) ;;
	nvme-shapes) line=$(timeout 1800 sh $T/qemu-nvme-shapes.sh "$SERIAL" 2>&1 | tail -n 1) ;;
	directio) line=$(timeout 1800 sh $T/qemu-directio.sh "$SERIAL" 2>&1 | tail -n 1) ;;
	cache32-write) line=$(timeout 1800 sh $T/qemu-cache32-write.sh "$SERIAL" "$K32" 400 2>&1 | tail -n 1) ;;
	grow-reboot) line=$(timeout 1800 sh $T/qemu-grow-reboot.sh "$SERIAL" 3 2>&1 | tail -n 1) ;;
	ufs-crash) line=$(timeout 2700 sh $T/qemu-ufs-crash.sh "$SERIAL" 2 6 15 2>&1 | tail -n 1) ;;
	boot) line=$(OUTPUT=${BOOT_OUTPUT:-$WS299_RUNS/boot-test} timeout 900 bash plan/tools/boot-test.sh "$SERIAL" 2>&1 | tail -n 1) ;;
	*) line="$test: unknown" ;;
	esac
	echo "$test: $line"
	case $line in
	*PASS*) ;;
	*) failed="$failed $test" ;;
	esac
	sync
done
if [ -z "$failed" ]; then
	echo "qemu-suite: PASS"
else
	echo "qemu-suite: FAIL (failed:$failed)"
	exit 1
fi

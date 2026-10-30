#!/bin/sh
# The close of a loopback TCP pair in each order, in a QEMU guest (the accepted end first, then the connected end,
# with the listener left open; and the other way round), blocking and not: tcp-close-repro.c is
# built against the image's sysroot, copied in over SSH and run in each "order nonblocking", ROUNDS times (default 3), each under a timeout.
# A run passes when it prints "done" and its closes took less than a second in the guest (the repro times its steps).  Judged over SSH, never the console.
#   plan/tools/tcp-close/qemu-close.sh IMAGE [OUT]
# IMAGE is a plan/ws221/tests/config-amd64-tcp.mk image (plan/tools/guest/test-image.sh), its BUILD/rootfs the sysroot the repro is built against.
# The guest runs in OUT/run (default build/tcp-close).  The last line is "qemu-close: PASS" or "qemu-close: FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
image=${1:?usage: qemu-close.sh IMAGE [OUT]}
out=${2:-build/tcp-close}
sysroot=$(cd "$(dirname -- "$image")" && pwd)/rootfs
mkdir -p "$out"
[ -d "$sysroot/usr/include" ] || { echo "qemu-close: FAIL (no sysroot at $sysroot)"; exit 1; }
cc=build/llvm/bin/clang
$cc --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -nostdinc -isystem "$sysroot/usr/include" -DKERN_USER_ABI_LP64 \
    -DKERN_DYNAMIC_LIBC -m64 -O2 -fPIC -fno-stack-protector -c plan/tools/tcp-close/tcp-close-repro.c -o "$out/repro.o" &&
    $cc --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -fuse-ld=lld -nostdlib -pie -Wl,--dynamic-linker=/lib/ld.so \
    "$sysroot/usr/lib/crt1.o" "$out/repro.o" -L"$sysroot/lib" -l:libc.so -o "$out/tcp-close-repro" ||
    { echo "qemu-close: FAIL (build of the repro)"; exit 1; }
case $out in
/*) GUEST_RUNTIME=$out/run ;;
*) GUEST_RUNTIME=$(pwd)/$out/run ;;
esac
export GUEST_RUNTIME
guest() { timeout "${T:-120}" python3 plan/tools/guest/guest.py "$@"; }
trap 'guest stop > /dev/null 2>&1' EXIT
guest start "$image" || { echo "qemu-close: FAIL (start)"; exit 1; }
T=300 guest wait --timeout 240 || { echo "qemu-close: FAIL (no SSH)"; exit 1; }
guest put "$out/tcp-close-repro" /tmp/tcp-close-repro || { echo "qemu-close: FAIL (copy)"; exit 1; }
guest run 'chmod 755 /tmp/tcp-close-repro' > /dev/null 2>&1
status=0
for run in $(seq "${ROUNDS:-3}"); do
for args in "0 1" "0 0" "1 1" "1 0"; do
	name=$(echo "$args" | tr ' ' -)-$run
	begin=$(date +%s.%N)
	T=40 guest run "/tmp/tcp-close-repro $args" > "$out/run-$name.txt" 2>&1
	end=$(date +%s.%N)
	seconds=$(awk -v a="$begin" -v b="$end" 'BEGIN { printf "%.1f", b - a }')
	last=$(tr -d '\r' < "$out/run-$name.txt" | tail -1)
	# The closes' time in the guest: from the first close to the step after the listener's.
	closes=$(tr -d '\r' < "$out/run-$name.txt" | awk '/^exchanged / { a = $2 } /^closed / { b = $2 } END { if (a != "" && b != "") print b - a }')
	echo "order,nonblocking $args: ${seconds} s over SSH, closes ${closes:-?} ms in the guest, last line: $last"
	if [ "$last" != done ] || [ -z "$closes" ] || [ "$closes" -ge 1000 ]; then
		status=1
	fi
done
done
after=$(guest run 'uname -s' 2>&1 | tr -d '\r')
echo "$after" | grep -q zedBSD || { echo "qemu-close: SSH did not answer after the runs"; status=1; }
[ $status = 0 ] && echo "qemu-close: PASS" || echo "qemu-close: FAIL"
exit $status

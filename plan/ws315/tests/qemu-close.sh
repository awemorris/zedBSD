#!/bin/sh
# The close of a loopback TCP pair in each order, in a QEMU guest (the accepted end first, then the connected end,
# with the listener left open; and the other way round), blocking and not: tcp-close-repro.c (from WS327 p016) is
# built against the image's sysroot, copied in over SSH and run four times, each "order nonblocking" under a timeout.
# A run passes when it prints "done" within 5 s.  Judged over SSH, never the console.
#   plan/ws315/tests/qemu-close.sh IMAGE [OUT]
# IMAGE is a plan/ws221/tests/config-amd64-tcp.mk image (plan/tools/guest/test-image.sh), its BUILD/rootfs the sysroot the repro is built against.
# The guest runs in OUT/run (default build/bug367/qemu).  The last line is "qemu-close: PASS" or "qemu-close: FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
image=${1:?usage: qemu-close.sh IMAGE [OUT]}
out=${2:-build/bug367/qemu}
sysroot=$(cd "$(dirname -- "$image")" && pwd)/rootfs
mkdir -p "$out"
[ -d "$sysroot/usr/include" ] || { echo "qemu-close: FAIL (no sysroot at $sysroot)"; exit 1; }
cc=build/llvm/bin/clang
$cc --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -nostdinc -isystem "$sysroot/usr/include" -DKERN_USER_ABI_LP64 \
    -DKERN_DYNAMIC_LIBC -m64 -O2 -fPIC -fno-stack-protector -c plan/ws315/tests/tcp-close-repro.c -o "$out/repro.o" &&
    $cc --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -fuse-ld=lld -nostdlib -pie -Wl,--dynamic-linker=/lib/ld.so \
    "$sysroot/usr/lib/crt1.o" "$out/repro.o" -L"$sysroot/lib" -l:libc.so -o "$out/tcp-close-repro" ||
    { echo "qemu-close: FAIL (build of the repro)"; exit 1; }
GUEST_RUNTIME=$(pwd)/$out/run
export GUEST_RUNTIME
guest() { timeout "${T:-120}" python3 plan/tools/guest/guest.py "$@"; }
trap 'guest stop > /dev/null 2>&1' EXIT
guest start "$image" || { echo "qemu-close: FAIL (start)"; exit 1; }
T=300 guest wait --timeout 240 || { echo "qemu-close: FAIL (no SSH)"; exit 1; }
guest put "$out/tcp-close-repro" /tmp/tcp-close-repro || { echo "qemu-close: FAIL (copy)"; exit 1; }
guest run 'chmod 755 /tmp/tcp-close-repro' > /dev/null 2>&1
status=0
for args in "0 1" "0 0" "1 1" "1 0"; do
	name=$(echo "$args" | tr ' ' -)
	begin=$(date +%s.%N)
	T=40 guest run "/tmp/tcp-close-repro $args" > "$out/run-$name.txt" 2>&1
	end=$(date +%s.%N)
	seconds=$(awk -v a="$begin" -v b="$end" 'BEGIN { printf "%.1f", b - a }')
	last=$(tr -d '\r' < "$out/run-$name.txt" | tail -1)
	echo "order,nonblocking $args: ${seconds} s, last line: $last"
	if [ "$last" != done ] || ! awk -v s="$seconds" 'BEGIN { exit !(s < 5.0) }'; then
		status=1
		# A run cut off by the timeout leaves the program in the guest; the next runs use their own sockets.
	fi
done
after=$(guest run 'uname -s' 2>&1 | tr -d '\r')
echo "$after" | grep -q zedBSD || { echo "qemu-close: SSH did not answer after the runs"; status=1; }
[ $status = 0 ] && echo "qemu-close: PASS" || echo "qemu-close: FAIL"
exit $status

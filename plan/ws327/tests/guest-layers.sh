#!/bin/sh
# The layers' own tests on zedBSD (QEMU, no GPU): thread-test.c and socket-test.c built for zedBSD against the image's
# libkeiland, copied in over SSH and run there.  The image is built first:
#   plan/tools/guest/test-image.sh plan/ws327/tests/config-amd64-layers.mk IMGBUILD
#   usage: sh plan/ws327/tests/guest-layers.sh OUT IMGBUILD     (OUT under build/, a new directory each run)
# Prints each test's last line and "ws327-guest-layers: PASS" or "FAIL".  The QEMU console is not read: the tests'
# own output comes back over SSH.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: guest-layers.sh OUT IMGBUILD}
img=${2:?usage: guest-layers.sh OUT IMGBUILD}
top=$(pwd)
sysroot=$top/build/amd64/sysroot
tests="thread socket"
mkdir -p "$out"
for t in $tests; do
	build/llvm/bin/clang --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -nostdinc -I. -Iinclude \
	    -isystem "$sysroot/usr/include" -Iuserland/desktop/include -DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64 \
	    -DKERN_DYNAMIC_LIBC -m64 -march=x86-64 -mno-red-zone -O1 -ffreestanding -fPIC -fno-builtin \
	    -fno-stack-protector -Wall -Wextra -Werror -c "plan/ws327/tests/$t-test.c" -o "$out/$t-test.o" ||
	    { echo "ws327-guest-layers: compile $t failed"; exit 1; }
	build/llvm/bin/clang --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -m64 -nostdlib -pie -Wl,--no-relax \
	    -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
	    -Wl,--dynamic-linker=/lib/ld.so "$sysroot/usr/lib/crt1.o" "$out/$t-test.o" \
	    -L"$img/dynamic" -Wl,-rpath-link,"$img/dynamic" -l:libkeiland.so -l:libc.so \
	    -o "$out/$t-test" || { echo "ws327-guest-layers: link $t failed"; exit 1; }
done
GUEST_RUNTIME=$top/$out/runtime
export GUEST_RUNTIME
g() { timeout "${G_TIMEOUT:-120}" python3 -I plan/tools/guest/guest.py "$@"; }
failed=0
timeout 600 python3 -I plan/tools/guest/guest.py start "$img/hdd-image.img" --symbols "$img/vmunix" \
    --qemu "$top/plan/ws314/tests/qemu-unsafe.sh" > "$out/start.log" 2>&1 ||
    { echo "ws327-guest-layers: the guest did not start"; exit 1; }
G_TIMEOUT=600 g wait --timeout 590 > "$out/wait.log" 2>&1 || { echo "ws327-guest-layers: the guest did not answer"; failed=1; }
if [ $failed = 0 ]; then
	for t in $tests; do
		g put "$out/$t-test" "/tmp/$t-test" >> "$out/put.log" 2>&1 || failed=1
	done
	g run 'uname -a; cat /etc/hosts /etc/resolv.conf 2>&1' > "$out/uname.txt" 2>&1
	# zedBSD's image has no /etc/hosts and the DNS does not answer "localhost": socket-test resolves it, so the guest
	# (a copy thrown away after the run) is given the two loopback lines when it has none.
	g run 'grep -q localhost /etc/hosts 2>/dev/null || printf "127.0.0.1 localhost\n::1 localhost\n" >> /etc/hosts; echo hosts-added=$?' \
	    > "$out/hosts.txt" 2>&1
	for t in $tests; do
		G_TIMEOUT=400 g run "chmod 755 /tmp/$t-test; /tmp/$t-test > /tmp/$t.log 2>&1; echo status=\$? >> /tmp/$t.log" \
		    > "$out/$t.run" 2>&1
		g run "cat /tmp/$t.log" > "$out/$t.log" 2>&1
	done
fi
g stop > "$out/stop.log" 2>&1
for t in $tests; do
	echo "$t: $(grep -v '^status=' "$out/$t.log" 2>/dev/null | tail -1) $(grep '^status=' "$out/$t.log" 2>/dev/null)"
	echo "$t: pass=$(grep -c '^PASS' "$out/$t.log" 2>/dev/null) fail=$(grep -c '^FAIL' "$out/$t.log" 2>/dev/null) not-done=$(grep -c '^NOT-DONE' "$out/$t.log" 2>/dev/null)"
	grep -q '^status=0$' "$out/$t.log" 2>/dev/null || failed=1
	grep -q '^FAIL' "$out/$t.log" 2>/dev/null && failed=1
done
[ $failed = 0 ] && echo "ws327-guest-layers: PASS" || echo "ws327-guest-layers: FAIL"

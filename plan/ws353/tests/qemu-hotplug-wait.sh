#!/bin/sh
# libvulkan's hotplug fence and its waits on the QEMU Venus guest: builds the guest program against libvulkan and
# the image (plan/ws353/tests/config-amd64-hotplug.mk, the program inside), boots it with two Venus heads on QEMU's
# D-Bus display, starts the program once from the serial line, plugs and unplugs head 1 with
# plan/tools/guest/venus-head.sh when the program's marker files appear, and fetches the program's result file.
# The console's text is never read for the verdict: only the program's result file, read with commands on the serial
# line.  The kernel's own lines can interleave with a command's status on the serial line, so every serial command
# is short, bounded and retried; the program waits 3 s before it opens the GPU so that the start command has
# returned by the time the kernel logs the open.
#
#   sh plan/ws353/tests/qemu-hotplug-wait.sh [OUT]
#
# OUT (default build/ws353-p003-qemu) gets summary.txt, the result lines and the host's timings.  The guest's
# runtime is build/ws353-p003-run (P1's own).  The image is build/x86_64/hdd-image.img: the build keeps the x86_64
# sysroot in build/x86_64 whatever BUILD says, so the image is built there, one target after the other.
# Steps (hotplug-wait-test.c): idle-h, idle-f, submit-f, narrow-f, race-f, idle-h2, submit-h, narrow-h, race-h, limit,
# plug, unplug, lost.  The hotplug steps need a GPU driver whose topology number moves only at a real change (Venus
# from BUG-431's fix: before it, a renderer request signaled a pending hotplug fence).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws353-p003-qemu}
config=plan/ws353/tests/config-amd64-hotplug.mk
sysroot=$PWD/build/x86_64/sysroot
dynamic=$PWD/build/x86_64/dynamic
program_dir=build/ws353-p003-prog
image=build/x86_64/hdd-image.img
export GUEST_RUNTIME=$PWD/build/ws353-p003-run
# The host's Venus renderer: the strict-queue build plan/ws035/tests/zdesktop-guest.sh names (libvulkan enumerates no
# physical device on the stock Debian one), on Lavapipe with zink for QEMU's GL.  VENUS_RENDERER names another copy.
VENUS_RENDERER=${VENUS_RENDERER:-$PWD/build/ws035-sq-venus/install}
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export RENDER_SERVER_EXEC_PATH="${RENDER_SERVER_EXEC_PATH:-$VENUS_RENDERER/libexec/virgl_render_server}"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x "$RENDER_SERVER_EXEC_PATH" ]; then
	echo "qemu-hotplug-wait: FAIL no Venus renderer at $RENDER_SERVER_EXEC_PATH (see plan/ws035/tests/zdesktop-guest.sh)" >&2
	exit 1
fi
. plan/tools/guest/venus-hostmem.sh
. plan/tools/fresh-out.sh
fresh_out "$out"
summary=$out/summary.txt
: > "$summary"
guest="python3 plan/tools/guest/guest.py"

say() {
	echo "$*" | tee -a "$summary"
}

# The sysroot and libvulkan, then the program against them, then the image with the program in it.
timeout 3000 make -j4 BUILD_JOBS=4 CONFIGMK=$config sysroot > "$out/make-sysroot.txt" 2>&1
timeout 1800 make -j4 BUILD_JOBS=4 CONFIGMK=$config build/x86_64/packages/libvulkan/.stamp > "$out/make-libvulkan.txt" 2>&1
mkdir -p "$program_dir"
clang=build/llvm/bin/clang
$clang --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -m64 -march=x86-64 -DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64 \
    -nostdinc -isystem "$sysroot/usr/include" -I"$PWD/include" -ffreestanding -fno-stack-protector -fPIC -O2 \
    -Wall -Wextra -Werror -c plan/ws353/tests/hotplug-wait-test.c -o "$program_dir/hotplug-wait-test.o"
$clang --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -m64 -march=x86-64 -nostdlib -pie -Wl,--no-relax \
    -Wl,--gc-sections -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code -Wl,-z,stack-size=0x100000 \
    -Wl,--allow-shlib-undefined -Wl,--dynamic-linker=/lib/ld.so "$sysroot/usr/lib/crt1.o" \
    "$program_dir/hotplug-wait-test.o" -L"$dynamic" -Wl,-rpath-link,"$dynamic" -l:libvulkan.so -l:libm.so -l:libc.so \
    -o "$program_dir/hotplug-wait-test.new"
if ! cmp -s "$program_dir/hotplug-wait-test.new" "$program_dir/hotplug-wait-test"; then
	mv "$program_dir/hotplug-wait-test.new" "$program_dir/hotplug-wait-test"
fi
timeout 5400 make -j4 BUILD_JOBS=4 CONFIGMK=$config userland > "$out/make-userland.txt" 2>&1
timeout 3000 make -j4 BUILD_JOBS=4 CONFIGMK=$config disk > "$out/make-disk.txt" 2>&1
sync

# The guest: Venus with two heads on a private D-Bus display, the renderer the host has.
mkdir -p "$GUEST_RUNTIME"
bus=$(timeout 10 dbus-daemon --session --fork --print-address=1 --print-pid=1 --nopidfile)
printf '%s\n' "$bus" | sed -n 1p > "$GUEST_RUNTIME/dbus.address"
printf '%s\n' "$bus" | sed -n 2p > "$GUEST_RUNTIME/dbus.pid"
DBUS_SESSION_BUS_ADDRESS=$(cat "$GUEST_RUNTIME/dbus.address")
export DBUS_SESSION_BUS_ADDRESS
finish() {
	timeout 60 $guest stop >/dev/null 2>&1 || true
	kill "$(cat "$GUEST_RUNTIME/dbus.pid")" 2>/dev/null || true
	sync
}
trap finish EXIT
timeout 900 $guest start "$image" --cpus 4 --qemu-extra \
    "-object memory-backend-memfd,id=mem,size=8G,share=on -machine memory-backend=mem -device virtio-gpu-gl-pci,id=venus,venus=on,blob=on,hostmem=$VENUS_HOSTMEM,max_outputs=2 -display dbus,gl=on,rendernode=/dev/dri/renderD128" \
    > "$out/start.txt"

# One short command on the serial line (it logs in first when needed); a garbled status costs 20 s, not 120.
serial() {
	timeout 30 python3 plan/tools/guest/serial.py --socket "$GUEST_RUNTIME/serial.sock" --timeout 20 run "$1"
}
tries=0
until serial 'uname -a' > "$out/uname.txt" 2>&1; do
	tries=$((tries + 1))
	if [ "$tries" -ge 12 ]; then
		say "qemu-hotplug-wait: FAIL no shell on the serial line (see $out/uname.txt)"
		exit 1
	fi
	sleep 10
done
head1() {
	timeout 30 sh plan/tools/guest/venus-head.sh 1 "$1" >> "$out/heads.txt" 2>&1
}
now_ms() {
	date +%s%3N
}
# QEMU ignores a size equal to the last one, and an unplug of a head never given a size: give head 1 one first,
# then let that change settle before the program starts.
head1 1024x768
sleep 3
head1 off
sleep 3

# Waits until a guest file exists, at most LIMIT seconds; a failed or garbled check is tried again.
guest_file() {
	started=$(date +%s)
	while ! serial "test -e $1" >/dev/null 2>&1; do
		if [ $(( $(date +%s) - started )) -ge "$2" ]; then
			return 1
		fi
		sleep 1
	done
}

# The program, in the background; its output and results stay in /tmp/p3 on the guest.
serial "rm -rf /tmp/p3; mkdir -p /tmp/p3; /root/hotplug-wait-test /tmp/p3 > /tmp/p3/out.txt 2>&1 < /dev/null &" \
    > "$out/program-start.txt" 2>&1 || true
sleep 8

# plug: once the program waits, the host waits 2 s and plugs head 1; unplug the same way.
for step in plug unplug; do
	if ! guest_file "/tmp/p3/$step.armed" 400; then
		say "$step: host saw no marker"
		break
	fi
	seen=$(now_ms)
	sleep 2
	before=$(( $(now_ms) - seen ))
	if [ "$step" = plug ]; then
		head1 1024x768
	else
		head1 off
	fi
	after=$(( $(now_ms) - seen ))
	echo "$before" > "$out/$step.before"
	say "$step: host changed head 1 from ${before} to ${after} ms after it saw the marker"
done

# The end of the run, then its results.
guest_file /tmp/p3/done 120 || say "qemu-hotplug-wait: the program did not finish"
tries=0
until serial 'cat /tmp/p3/result.txt' > "$out/result-raw.txt" 2>&1; do
	tries=$((tries + 1))
	[ "$tries" -lt 5 ] || break
	sleep 2
done
grep -E '^[a-z0-9-]+: ' "$out/result-raw.txt" | grep -v '^serial:' | tee -a "$summary" || true
serial 'cat /tmp/p3/out.txt' > "$out/program-out.txt" 2>&1 || true
serial 'dmesg | grep -i -e venus -e gpu' > "$out/dmesg-gpu.txt" 2>&1 || true

# The wake after each change: the program's time from its marker, less the host's time from seeing the marker to
# starting the change, bounds the delay after the change began (the marker was made before the host saw it).
latency_failed=0
for step in plug unplug; do
	woke=$(sed -n "s/^$step: PASS ms=\([0-9]*\).*/\1/p" "$out/result-raw.txt" | tail -1)
	if [ -n "$woke" ] && [ -s "$out/$step.before" ]; then
		delay=$(( woke - $(cat "$out/$step.before") ))
		say "$step: woke ${woke} ms after the marker; delay after the change began <= ${delay} ms"
		if [ "$delay" -gt 1000 ]; then
			say "$step-latency: FAIL more than 1 s"
			latency_failed=1
		fi
	else
		say "$step-latency: FAIL no time"
		latency_failed=1
	fi
done

# The program's own summary line and the latencies decide.
if grep -q '^hotplug-wait-test: PASS' "$out/result-raw.txt" && [ "$latency_failed" -eq 0 ]; then
	say "qemu-hotplug-wait: PASS"
else
	say "qemu-hotplug-wait: FAIL"
	exit 1
fi

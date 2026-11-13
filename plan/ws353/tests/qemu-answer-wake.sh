#!/bin/sh
# The network link's and the Wayland WSI's waits on the QEMU Venus guest: builds the test's programs and the image
# (plan/ws353/tests/config-amd64-answer.mk), boots it, runs the programs from the serial line with their results in
# guest files, and judges by those files read with commands on the serial line (the console's text is never read for
# the verdict).
#
# Steps:
#   network-answer-stand-in (network-answer-test.c against a stand-in networkd): the link's thread wakes for the
#     answers of a join and of the scans' frames, not every 10 ms, and never for a POLLERR of the request's own
#     shutdown; the join's answer is taken within 300 ms of the stand-in's writing it.
#   network-answer-real: the same link against the guest's networkd, a scan request answered through the update.
#   wsi-visible: the compositor (/bin/wayland --testing) and wltest-polls (wltest with poll-count.c) presenting 120
#     FIFO frames: all done, and no poll of two descriptors (the WSI's sleep) ended by a short (<= 100 ms) timeout.
#   wsi-held: wltest-polls presenting 200 FIFO frames while the compositor is stopped (SIGSTOP) for 5 s, so that it
#     sends no frame callback: the presents go on at the 1 s pace (at least one wait of 900 ms or more ended by its
#     timeout), with no short timed-out wait (a 50 ms slice makes twenty a second), and all frames done after SIGCONT.
#   stop: SIGTERM, KWL EXIT and no KWL FAILED.
#
#   sh plan/ws353/tests/qemu-answer-wake.sh [OUT]
#
# OUT (default build/ws353-p015-qemu) gets summary.txt, the make logs and the guest's result files.  The guest's
# runtime is build/ws353-p015-run.  The image is build/x86_64/hdd-image.img.  IMAGE=<file> skips the builds.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws353-p015-qemu}
config=plan/ws353/tests/config-amd64-answer.mk
sysroot=$PWD/build/x86_64/sysroot
dynamic=$PWD/build/x86_64/dynamic
program_dir=build/ws353-p015-prog
export GUEST_RUNTIME=$PWD/build/ws353-p015-run
# The host's Venus renderer (plan/ws353/tests/build-venus-renderer.sh makes it), on Lavapipe with zink for QEMU's GL.
VENUS_RENDERER=${VENUS_RENDERER:-$PWD/build/ws035-sq-venus/install}
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export RENDER_SERVER_EXEC_PATH="${RENDER_SERVER_EXEC_PATH:-$VENUS_RENDERER/libexec/virgl_render_server}"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x "$RENDER_SERVER_EXEC_PATH" ]; then
	echo "qemu-answer-wake: FAIL no Venus renderer at $RENDER_SERVER_EXEC_PATH (plan/ws353/tests/build-venus-renderer.sh)" >&2
	exit 1
fi
. plan/tools/guest/venus-hostmem.sh
. plan/tools/fresh-out.sh
fresh_out "$out"
summary=$out/summary.txt
: > "$summary"
guest="python3 plan/tools/guest/guest.py"
failed=0

say() {
	echo "$*" | tee -a "$summary"
}
fail_step() {
	say "$1: FAIL $2"
	failed=1
}

# The programs against the sysroot's libraries: the network link's test twice (a stand-in networkd's socket in the
# test's folder, and networkd's own), its polls wrapped; wltest with the poll counter first in the loader's search.
build_programs() {
	clang=build/llvm/bin/clang
	cflags="--target=x86_64-unknown-zedbsd --sysroot=$sysroot -m64 -march=x86-64 -DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64
		-nostdinc -isystem $sysroot/usr/include -I$PWD -I$PWD/include -Iuserland/desktop/include
		-ffreestanding -fno-stack-protector -fPIC -O2 -Wall -Wextra -Werror"
	ldflags="--target=x86_64-unknown-zedbsd --sysroot=$sysroot -m64 -march=x86-64 -nostdlib -pie -Wl,--no-relax
		-Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code -Wl,-z,stack-size=0x100000 -Wl,--allow-shlib-undefined
		-Wl,--dynamic-linker=/lib/ld.so $sysroot/usr/lib/crt1.o"
	mkdir -p "$program_dir"
	for mode in stand-in real; do
		objects=""
		set --
		if [ "$mode" = stand-in ]; then
			set -- '-DNETWORKD_SOCKET="/tmp/p15/networkd.sock"'
		fi
		for source in plan/ws353/tests/network-answer-test.c userland/desktop/libkeiland-backend-zedbsd/network-zedbsd.c \
		    userland/desktop/libkeiland-backend-zedbsd/daemon-link-zedbsd.c userland/base/net/protocol.c; do
			object=$program_dir/$mode-$(basename "$source" .c).o
			# shellcheck disable=SC2086
			$clang $cflags "$@" -c "$source" -o "$object"
			objects="$objects $object"
		done
		# shellcheck disable=SC2086
		$clang $ldflags $objects -Wl,--wrap=poll -L"$dynamic" -Wl,-rpath-link,"$dynamic" -l:libc.so \
		    -o "$program_dir/network-answer-$mode.new"
		if ! cmp -s "$program_dir/network-answer-$mode.new" "$program_dir/network-answer-$mode"; then
			mv "$program_dir/network-answer-$mode.new" "$program_dir/network-answer-$mode"
		fi
	done
	objects=""
	for source in userland/tests/wltest/main.c userland/tests/wltest/window.c userland/tests/wltest/renderer.c \
	    plan/ws353/tests/poll-count.c; do
		object=$program_dir/wl-$(basename "$source" .c).o
		# shellcheck disable=SC2086
		$clang $cflags -I"$sysroot/usr/include/wayland" -c "$source" -o "$object"
		objects="$objects $object"
	done
	# shellcheck disable=SC2086
	$clang $ldflags $objects -L"$dynamic" -Wl,-rpath-link,"$dynamic" -l:libvulkan.so -l:libwayland-client.so \
	    -l:libm.so -l:libc.so -o "$program_dir/wltest-polls.new"
	if ! cmp -s "$program_dir/wltest-polls.new" "$program_dir/wltest-polls"; then
		mv "$program_dir/wltest-polls.new" "$program_dir/wltest-polls"
	fi
}

# The image, unless one is given; its build's warnings outside noct (the toolchain's) are a failure.
image=${IMAGE:-}
if [ -z "$image" ]; then
	timeout 3000 make -j4 BUILD_JOBS=4 CONFIGMK=$config sysroot > "$out/make-sysroot.txt" 2>&1
	timeout 3000 make -j4 BUILD_JOBS=4 CONFIGMK=$config build/x86_64/packages/libvulkan/.stamp \
	    build/x86_64/packages/libwayland-client/.stamp > "$out/make-libraries.txt" 2>&1
	build_programs > "$out/make-programs.txt" 2>&1
	timeout 5400 make -j4 BUILD_JOBS=4 CONFIGMK=$config userland > "$out/make-userland.txt" 2>&1
	timeout 3000 make -j4 BUILD_JOBS=4 CONFIGMK=$config disk > "$out/make-disk.txt" 2>&1
	sync
	warnings=$(cat "$out"/make-*.txt | grep 'warning:' | grep -vc 'userland/base/noct/' || true)
	say "build: warnings outside noct ${warnings}"
	[ "$warnings" -eq 0 ] || failed=1
	image=build/x86_64/hdd-image.img
fi

# The guest: Venus with one head on a private D-Bus display, the renderer the host has.
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
    "-object memory-backend-memfd,id=mem,size=8G,share=on -machine memory-backend=mem -device virtio-gpu-gl-pci,id=venus,venus=on,blob=on,hostmem=$VENUS_HOSTMEM,max_outputs=1 -display dbus,gl=on,rendernode=/dev/dri/renderD128" \
    > "$out/start.txt"

# One short command on the serial line (it logs in first when needed); a garbled status costs 20 s, not 120.
serial() {
	timeout 30 python3 plan/tools/guest/serial.py --socket "$GUEST_RUNTIME/serial.sock" --timeout 20 run "$1"
}
tries=0
until serial 'uname -a' > "$out/uname.txt" 2>&1; do
	tries=$((tries + 1))
	if [ "$tries" -ge 12 ]; then
		say "qemu-answer-wake: FAIL no shell on the serial line (see $out/uname.txt)"
		exit 1
	fi
	sleep 10
done

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

# Waits until at least MIN lines of a guest file match, at most LIMIT seconds.
wait_in() {
	started=$(date +%s)
	while ! serial "test \$(grep -c '$1' $4) -ge $2" >/dev/null 2>&1; do
		if [ $(( $(date +%s) - started )) -ge "$3" ]; then
			return 1
		fi
		sleep 1
	done
}

# The lines of a guest file that match a pattern, kept in OUT (tried again when the serial line garbles the answer):
# the lines that are the file's own (a kernel record mixed into the serial output does not match).
fetch() {
	tries=0
	: > "$out/$3"
	while [ "$tries" -lt 4 ]; do
		serial "cat $1" 2>/dev/null | tr -d '\r' | grep -E "$2" > "$out/$3" || true
		if [ -s "$out/$3" ]; then
			return 0
		fi
		tries=$((tries + 1))
		sleep 1
	done
}

# One number from the counter's POLLS line in an OUT file.
polls() {
	sed -n "s/.* $1=\([0-9]*\).*/\1/p" "$out/$2" | sed -n '$p'
}

dir=/tmp/p15
serial "rm -rf $dir; mkdir -p $dir/home" > /dev/null 2>&1 || true

# The network link against the stand-in networkd, then against networkd.
for mode in stand-in real; do
	serial "(/root/network-answer-$mode $mode $dir/net-$mode.txt > $dir/net-$mode.out 2>&1; touch $dir/net-$mode.done) < /dev/null > /dev/null 2>&1 &" \
	    > /dev/null 2>&1 || true
	if ! guest_file "$dir/net-$mode.done" 90; then
		fail_step "network-answer-$mode" "the program did not finish within 90 s"
		continue
	fi
	fetch "$dir/net-$mode.txt" '^[a-z-]+: (PASS|FAIL)' "net-$mode.txt"
	sed "s/^/  /" "$out/net-$mode.txt" | tee -a "$summary"
	if grep -q '^network-answer-test: PASS' "$out/net-$mode.txt"; then
		say "network-answer-$mode: PASS"
	else
		fail_step "network-answer-$mode" "see $out/net-$mode.txt"
	fi
done

# The compositor, in the background, its log in a guest file.
log=$dir/kwl.log
serial "XDG_RUNTIME_DIR=$dir HOME=$dir/home /bin/wayland --testing --timeout=900 > $log 2>&1 < /dev/null &" \
    > "$out/compositor-start.txt" 2>&1 || true
if wait_in 'KWL READY' 1 120 $log && wait_in 'KWL MODE window' 1 120 $log; then
	say "start: PASS the compositor ready in window mode"
else
	fail_step start "no KWL READY or window mode (see $out/kwl.log)"
fi
fetch "$log" '^KWL READY' ready.txt
pid=$(sed -n 's/.* pid=\([0-9]*\) .*/\1/p' "$out/ready.txt" | sed -n 1p)
sleep 5

# Runs wltest-polls in the background: TOKEN, then its options; its output and the end marker in the folder.
wltest() {
	token=$1
	shift
	serial "(WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir /root/wltest-polls --display=/tmp/wayland-0 --token=$token $* > $dir/wl-$token.txt 2>&1; touch $dir/wl-$token.done) < /dev/null > /dev/null 2>&1 &" \
	    > /dev/null 2>&1 || true
}

# Visible: FIFO frames paced by the compositor's frame callbacks.
wltest visible --mode=fifo --frames=120 --delay-ms=0
if guest_file "$dir/wl-visible.done" 180; then
	fetch "$dir/wl-visible.txt" '^(WLTEST (START|PRESENT|DONE)|POLLS) ' wl-visible.txt
	sed "s/^/  /" "$out/wl-visible.txt" | tee -a "$summary"
	short=$(polls two_fd_short_timed_out wl-visible.txt)
	if grep -q '^WLTEST DONE run=visible frames=120' "$out/wl-visible.txt" && [ "${short:-1}" -eq 0 ]; then
		say "wsi-visible: PASS 120 frames, short timed-out waits ${short}"
	else
		fail_step wsi-visible "frames not all done or short timed-out waits ${short:-?} (wanted 0)"
	fi
else
	fail_step wsi-visible "wltest-polls did not finish within 180 s"
fi

# Held: the compositor stopped for 5 s while the client presents FIFO frames.
wltest held --mode=fifo --frames=200 --delay-ms=0
if wait_in 'WLTEST START' 1 60 "$dir/wl-held.txt" && [ -n "$pid" ]; then
	sleep 4
	serial "kill -STOP $pid" > "$out/stop.txt" 2>&1 || true
	stopped_at=$(date +%s%3N)
	sleep 5
	serial "kill -CONT $pid" > "$out/cont.txt" 2>&1 || true
	say "wsi-held: the compositor (pid $pid) stopped for $(( $(date +%s%3N) - stopped_at )) ms"
fi
if guest_file "$dir/wl-held.done" 240; then
	fetch "$dir/wl-held.txt" '^(WLTEST (START|PRESENT|DONE)|POLLS) ' wl-held.txt
	sed "s/^/  /" "$out/wl-held.txt" | tee -a "$summary"
	short=$(polls two_fd_short_timed_out wl-held.txt)
	long=$(polls two_fd_long_timed_out wl-held.txt)
	if grep -q '^WLTEST DONE run=held frames=200' "$out/wl-held.txt" && [ "${short:-1}" -eq 0 ] && [ "${long:-0}" -ge 1 ]; then
		say "wsi-held: PASS 200 frames, paced waits ended by the 1 s deadline ${long}, short timed-out waits ${short}"
	else
		fail_step wsi-held "frames not all done, paced timeouts ${long:-?} (wanted >= 1) or short timed-out waits ${short:-?} (wanted 0)"
	fi
else
	fail_step wsi-held "wltest-polls did not finish within 240 s"
fi

# The end: SIGTERM, the compositor gone (its KWL EXIT line looked for during 60 s, so that a garbled serial answer
# is asked again), no failure logged.
serial "kill $pid" > "$out/kill.txt" 2>&1 || true
if wait_in 'KWL EXIT' 1 60 $log; then
	say "stop: PASS the compositor ended"
else
	fail_step stop "no KWL EXIT within 60 s"
fi
tries=0
failures=
while [ "$tries" -lt 4 ] && [ -z "$failures" ]; do
	failures=$(serial "grep -c 'KWL FAILED' $log" 2>/dev/null | tr -d '\r' | grep -E '^[0-9]+$' | sed -n '$p' || true)
	tries=$((tries + 1))
done
if [ "${failures:-1}" -eq 0 ]; then
	say "failed: PASS no KWL FAILED"
else
	fail_step failed "${failures:-?} KWL FAILED lines"
fi

# The guest's log for the record (read over the serial line: kernel records may be mixed in; not used for the verdict).
serial "grep -e 'KWL READY' -e 'KWL MODE' -e 'KWL EXIT' -e 'KWL FAILED' -e 'KWL MAP' -e 'KWL UNMAP' $log" > "$out/kwl.log" 2>&1 || true

if [ "$failed" -eq 0 ]; then
	say "qemu-answer-wake: PASS"
else
	say "qemu-answer-wake: FAIL"
	exit 1
fi

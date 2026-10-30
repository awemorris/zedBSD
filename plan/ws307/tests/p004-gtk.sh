#!/bin/sh
# WS307 p004: a GTK 4 application built on a zedBSD the installer installed.  Boots IMAGE (make installer-image with
# plan/ws307/tests/config-amd64-p004.mk and the harness's files) from USB with one empty NVMe disk of 512-byte blocks,
# installs the base, the desktop, the firmware, OpenSSH, OpenSSL, Clang (with pkgconf) and GTK4 on the whole disk
# through install-admin (plan/ws307/tests/p004-guest-install.sh), and writes, renames and syncs a file on the installed
# root left mounted.  With the guest stopped, the host reads that file through the loader's UFS reader (the j3 rule on
# a disk of 512-byte blocks).  Then the installed disk is started alone (its user's desktop by the automatic login):
# as that user over SSH, plan/ws307/tests/p004-guest-run.sh builds plan/ws307/tests/p004-hello.c with the system's
# clang and pkg-config, runs it in the session (its window is photographed over QMP) and builds and runs a C++
# program.  The summary is the steps' lines and "p004-gtk: PASS|FAIL OUT".  Output goes to a new
# build/ws307-test/p004.*.
#   sh plan/ws307/tests/p004-gtk.sh IMAGE
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname "$0")/../../.."
image=${1:?usage: p004-gtk.sh IMAGE}
mkdir -p build/ws307-test
out=$(mktemp -d build/ws307-test/p004.XXXXXX)
truncate -s 24G "$out/nvme.img"
cp --sparse=always "$image" "$out/boot.img"
g() { timeout "${G_TIMEOUT:-1800}" python3 plan/tools/guest/guest.py "$@"; }
status=0

# The install, in the live system.
export GUEST_RUNTIME="$(pwd)/$out/guest-live"
extra="-drive file=$out/nvme.img,if=none,id=p4n,format=raw,cache=unsafe -device nvme,serial=p004n,drive=p4n"
g start "$out/boot.img" --disk usb --qemu-extra "$extra" > "$out/live-start.txt" 2>&1 || true
if g wait --timeout 900 > "$out/live-wait.txt" 2>&1; then
	g put plan/ws307/tests/p004-guest-install.sh /tmp/p004-install.sh > /dev/null 2>&1 || true
	G_TIMEOUT=5400 g run sh /tmp/p004-install.sh > "$out/install.txt" 2>&1 || true
else
	g screenshot "$out/live-no-ssh.png" > /dev/null 2>&1 || true
	echo "p004: the live system did not answer" > "$out/install.txt"
fi
g stop > /dev/null 2>&1 || true
grep -E '^(STEP|OUT|INSTALLED|UNITS|PC|SYNC|P004)' "$out/install.txt" || true
grep -q '^P004 DONE' "$out/install.txt" || status=1
grep -q '^STEP .* fail' "$out/install.txt" && status=1

# The synced file, read on the host through the loader's reader (512-byte blocks).
cc -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -o "$out/ufs-cat" plan/ws307/tests/host/ufs-cat.c bootloader/common/ufs-read.c
start=$(awk '$1 == "SYNCROOT" { print $3 }' "$out/install.txt")
want=$(awk '$1 == "SYNCSUM" { print $3 " " $4 }' "$out/install.txt")
if [ -n "$start" ] && [ -n "$want" ]; then
	host=$("$out/ufs-cat" "$out/nvme.img" $((start * 512)) 512 /boot/p004-sync.bin 2> "$out/ufs-cat.txt" | cksum) || host=
	if [ "$host" = "$want" ]; then
		echo "HOST j3-sync ok"
	else
		echo "HOST j3-sync fail ($host, want $want)"; status=1
	fi
else
	echo "HOST j3-sync fail (no sum)"; status=1
fi

# The installed disk alone: the build and the window, as the installed user.
export GUEST_RUNTIME="$(pwd)/$out/guest-installed"
g start "$out/nvme.img" --disk nvme > "$out/installed-start.txt" 2>&1 || true
port=$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["ssh_port"])' "$GUEST_RUNTIME/session.json" 2>/dev/null || echo 0)
ssh_tester() {
	timeout "${SSH_TIMEOUT:-60}" ssh -i plan/tmp/guest/id_ed25519 -p "$port" -o BatchMode=yes -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR tester@127.0.0.1 "$@"
}
n=0
up=
while [ $n -lt 120 ]; do
	if ssh_tester 'test -S /run/user/$(id -u)/wayland-0' 2> /dev/null; then up=1; break; fi
	sleep 5
	n=$((n + 1))
done
if [ -n "$up" ]; then
	echo "SSH installed ok"
	dir=/home/tester/p004
	ssh_tester "mkdir -p $dir" > /dev/null 2>&1 || true
	for file in p004-hello.c p004-guest-run.sh; do
		ssh_tester "cat > $dir/$file" < plan/ws307/tests/$file || true
	done
	# The window is photographed once the program says it is on the screen (it ends itself 20 seconds after it
	# started).
	SSH_TIMEOUT=900 ssh_tester "sh $dir/p004-guest-run.sh $dir" > "$out/run.txt" 2>&1 &
	run=$!
	n=0
	while [ $n -lt 300 ] && kill -0 $run 2> /dev/null; do
		if ssh_tester "grep -q '^P004 MAPPED' $dir/hello.out" 2> /dev/null; then
			sleep 2
			g screenshot "$out/hello.png" > /dev/null 2>&1 || true
			echo "SHOT $out/hello.png"
			break
		fi
		sleep 2
		n=$((n + 1))
	done
	wait $run || true
	grep -E '^(STEP|PKGCONF|CLANG|CFLAGS|LIBS|HELLO|CXX|P004)' "$out/run.txt" || true
	grep -q '^P004 RUN DONE' "$out/run.txt" || status=1
	grep -q '^STEP .* fail' "$out/run.txt" && status=1
else
	g screenshot "$out/installed-no-ssh.png" > /dev/null 2>&1 || true
	echo "SSH installed fail (no session socket over SSH within 600 s)"
	status=1
fi
g stop > /dev/null 2>&1 || true
[ $status -eq 0 ] && echo "p004-gtk: PASS $out" || echo "p004-gtk: FAIL $out"
exit $status

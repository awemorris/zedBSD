#!/bin/sh
# ws352: poll(2)'s wakes under QEMU+KVM (amd64), end to end; the summary is OUT/summary.txt, its last line
# "qemu-poll: done ..." or "qemu-poll: FAIL ...".
#   sh plan/ws352/tests/qemu-poll.sh OUT [PORT]
# OUT is a new directory, PORT the host's SSH port (default 2352).  The image is this tree's build/x86_64 (the
# disk built with CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y and without the firmware packages) with
# its root tree copied, plus the guest harness's network (plan/tools/guest/net.conf) and /bin/polltool
# (plan/ws352/tests/polltool, static-tool.sh), made into a disk the way the build does.  The console is not read:
# the guest is driven over SSH as user/user.
#  1. polltool: each change wakes the poll that waits for it (pipes, unix stream and datagram sockets, a
#     listener), and a poll of an idle pipe is not woken by another pipe's changes.
#  2. The same with four polltool at once, and with every CPU busy (four shells spinning).
#  3. SSH's own traffic (sshd and the shell poll their sockets and pipes) and a pipeline work.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
OUT=${1:?OUT}
PORT=${2:-2352}
cd "$(dirname "$0")/../../.."
mkdir -p "$OUT"
SUMMARY=$OUT/summary.txt
: > "$SUMMARY"
say() {
	echo "qemu-poll: $*" | tee -a "$SUMMARY"
}
fail() {
	say "FAIL $*"
	exit 1
}
B=build/x86_64
for f in $B/vmunix $B/rootfs/bin/sh $B/zedbsd.cfg $B/uefi/BOOTX64.EFI $B/native-swap-1024m.img; do
	[ -e "$f" ] || fail "no $f"
done

# The image: the root tree, the network and the tool.
sh plan/ws333/tests/hw5330/static-tool.sh plan/ws352/tests/polltool "$OUT/static" > "$OUT/static.log" 2>&1 ||
	fail "polltool build"
[ ! -e "$OUT/rootfs" ] || fail "$OUT/rootfs is there from an earlier run: give a new OUT"
mkdir -p "$OUT/rootfs"
cp -a "$B/rootfs/." "$OUT/rootfs/"
cp plan/tools/guest/net.conf "$OUT/rootfs/etc/net.conf"
cp "$OUT/static/polltool" "$OUT/rootfs/bin/polltool"
build/NoctLang/build/noct --path=mk/tools:mk/old-tools/build mk/tools/make-rootfs.noct --backend "$B/zedimage-host" \
	--force --profile amd64 --output "$OUT/root.ufs" --tree "$OUT/rootfs" --size-mib 1024 --min-inodes 65536 \
	> "$OUT/image.log" 2>&1 || fail "root file system"
python3 mk/platform-x86_64/tools/make-x86_64-hdd-image.py --force --variant uefi --kernel $B/vmunix \
	--zedbsd-config $B/zedbsd.cfg --ufs-root "$OUT/root.ufs" --swap-image $B/native-swap-1024m.img \
	--bootx64 $B/uefi/BOOTX64.EFI --fat-size-mib 256 "$OUT/disk.img" >> "$OUT/image.log" 2>&1 || fail "disk image"
sync
say "image kernel $(sha256sum $B/vmunix | cut -c1-16)"

cp /usr/share/OVMF/OVMF_VARS_4M.fd "$OUT/vars.fd"
timeout 900 qemu-system-x86_64 -machine q35 -m 8G -smp 4 -accel kvm -cpu host \
	-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	-drive if=pflash,format=raw,file="$OUT/vars.fd" \
	-device qemu-xhci,id=xhci,p2=8,p3=8 \
	-drive if=none,id=boot,file="$OUT/disk.img",format=raw \
	-device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
	-netdev "user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dhcpstart=10.0.2.15,hostfwd=tcp:127.0.0.1:$PORT-:22" \
	-device usb-net,bus=xhci.0,port=2,id=ecm,netdev=net0,mac=52:54:00:33:52:01,msos-desc=on \
	-vga std -display none -no-reboot \
	-monitor unix:"$OUT/m.sock",server,nowait > "$OUT/qemu.out" 2>&1 &
QEMU=$!
cleanup() {
	kill "$QEMU" 2> /dev/null
	wait "$QEMU" 2> /dev/null
	sync
}
trap cleanup EXIT
hmp() {
	printf '%b' "$1" | timeout 5 socat -t 1 - UNIX-CONNECT:"$OUT/m.sock" 2> /dev/null
}
zb() {
	timeout "${TO:-60}" sshpass -p user ssh -n -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
		-o ConnectTimeout=5 -o LogLevel=ERROR -p "$PORT" user@127.0.0.1 "$1"
}
start=$(date +%s)
until TO=10 zb true 2> /dev/null; do
	if [ $(($(date +%s) - start)) -ge 300 ]; then
		hmp "screendump $OUT/no-ssh.ppm\n" > /dev/null
		fail "no SSH in 300 s (screen in $OUT/no-ssh.ppm)"
	fi
	sleep 5
done
say "ssh up after $(($(date +%s) - start)) s"
sleep 10

# 1.
TO=60 zb "/bin/polltool" > "$OUT/one.txt" 2>&1
say "one: $(tail -n 1 "$OUT/one.txt") ($(grep -c ': ok' "$OUT/one.txt") ok)"

# 2.
TO=90 zb "for i in 1 2 3 4; do /bin/polltool > /tmp/polltool.\$i 2>&1 & done; wait; cat /tmp/polltool.1 /tmp/polltool.2 /tmp/polltool.3 /tmp/polltool.4" \
	> "$OUT/four.txt" 2>&1
say "four at once: $(grep -c 'polltool: PASS' "$OUT/four.txt") of 4 PASS ($(grep -c FAIL "$OUT/four.txt") FAIL lines)"
TO=120 zb "for i in 1 2 3 4; do sh -c 'i=0; while [ \$i -lt 400000 ]; do i=\$((i + 1)); done' & done; /bin/polltool; wait" \
	> "$OUT/busy.txt" 2>&1
say "busy: $(grep 'polltool:' "$OUT/busy.txt") ($(grep -c ': ok' "$OUT/busy.txt") ok)"

# 3.
lines=$(TO=60 zb "seq 1 20000 | sort -n | tail -n 1")
say "pipeline: $lines"
if grep -q 'polltool: PASS' "$OUT/one.txt" && [ "$(grep -c 'polltool: PASS' "$OUT/four.txt")" = 4 ] &&
	grep -q 'polltool: PASS' "$OUT/busy.txt" && [ "$lines" = 20000 ]; then
	say "done PASS"
else
	say "FAIL (see $OUT)"
fi

#!/bin/sh
# Standard output at the end of a program on the x86_64 QEMU guest: builds the sysroot, the test's programs
# (build.sh) and the image (config-x86_64-flush.mk, the programs in /root), boots it, runs /root/run.sh
# (guest-run.sh) on the serial line (plan/tools/guest/serial.py), fetches the guest's result file with cat and
# writes OUT/summary.txt.  The console's text is not the verdict: only the result file's lines are.
#   sh plan/bugs/BUG-432/qemu-flush.sh [OUT]      (OUT default build/bug432/qemu; the last line is PASS or FAIL)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
out=${1:-build/bug432/qemu}
config=plan/bugs/BUG-432/config-x86_64-flush.mk
image=build/x86_64/hdd-image.img
fresh_out "$out"
summary=$out/summary.txt
: > "$summary"
say() {
	echo "$*" | tee -a "$summary"
}
make_step() {
	timeout "$1" make -j4 BUILD_JOBS=4 CONFIGMK=$config "$2" > "$out/make-$2.txt" 2>&1 ||
		{ say "qemu-flush: FAIL make $2 (see $out/make-$2.txt)"; exit 1; }
}
make_step 3000 sysroot
timeout 600 sh plan/bugs/BUG-432/build.sh build/bug432/prog > "$out/build.txt" 2>&1 ||
	{ say "qemu-flush: FAIL build.sh (see $out/build.txt)"; exit 1; }
make_step 5400 userland
make_step 3000 disk
sync
say "image $(sha256sum "$image" | cut -c1-16) tree $(git rev-parse --short HEAD)$(git diff --quiet HEAD -- src/libc || echo +dirty)"

cp /usr/share/OVMF/OVMF_VARS_4M.fd "$out/vars.fd"
# shellcheck disable=SC2046
timeout 900 qemu-system-x86_64 -name p2-bug432 -machine q35 -m 1G -smp 2 $(qemu_accel_args max) \
	-drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
	-drive "if=pflash,format=raw,file=$out/vars.fd" \
	-drive "if=none,id=boot,file=$image,format=raw,snapshot=on" \
	-device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
	-vga std -display none -no-reboot \
	-serial "unix:$out/serial.sock,server=on,wait=off" > "$out/qemu.txt" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null; sync' EXIT
i=0
while [ ! -S "$out/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
serial() {
	timeout 200 /usr/bin/python3 plan/tools/guest/serial.py --socket "$out/serial.sock" --timeout 180 "$@"
}
serial expect 'login: ' > "$out/login.txt" 2>&1 || { say "qemu-flush: FAIL no login prompt"; exit 1; }
serial login >> "$out/login.txt" 2>&1
serial run 'sh /root/run.sh' > "$out/run.txt" 2>&1 || true
tries=0
until grep -q '^done ' "$out/result.txt" 2> /dev/null; do
	tries=$((tries + 1))
	[ $tries -le 3 ] || break
	serial run 'cat /tmp/bug432/result.txt' > "$out/cat.txt" 2>&1 || true
	tr -d '\r' < "$out/cat.txt" | grep -E '^[a-z]+-[a-z]+-[a-z]+ (PASS|FAIL) |^done ' > "$out/result.txt"
done
cat "$out/result.txt" | tee -a "$summary"
if grep -q '^done ' "$out/result.txt" && ! grep -q ' FAIL ' "$out/result.txt" &&
	[ "$(grep -c ' PASS ' "$out/result.txt")" -eq 19 ]; then
	say "qemu-flush: PASS"
else
	say "qemu-flush: FAIL"
fi

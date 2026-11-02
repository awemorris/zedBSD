#!/bin/sh
# The interop test of libssl-compat.so on zedBSD (amd64, QEMU): builds both compat libraries and libc with
# config-guest.mk, builds tls-client.c for zedBSD against them, makes the test PKI (run-interop.sh --make-pki),
# builds the image with /root/tls-client and /root/ca.pem in it, boots it with QEMU's user network on a USB
# adapter (the guest's ue0, DHCP; the host's 127.0.0.1 is 10.0.2.2 there, nothing of the guest is forwarded),
# logs in on the serial console (a unix socket, plan/tools/guest/serial.py) and runs the cases of
# run-interop.sh --guest: the host's s_server on 127.0.0.1, the client in the guest.
#   sh plan/ws335/tests/run-guest.sh [BUILD]      (BUILD default build/ws335-guest/image)
# The QEMU run is named ws335-p1, its directory build/ws335-guest/run (a fresh run directory); the disk is opened
# with snapshot=on.  The result is read from the client's own output over the serial line, not from a console log.
# The last line is run-guest: PASS or run-guest: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
PATH=/usr/bin:/bin:$PATH
export PATH
build=${1:-build/ws335-guest/image}
. plan/tools/fresh-out.sh
out=build/ws335-guest/run
fresh_out "$out"
config=plan/ws335/tests/config-guest.mk
python=/usr/bin/python3
fail() {
	echo "run-guest: FAIL ($1)"
	exit 1
}

# 1. The libraries.
timeout 1800 make ZEDBSD_CONFIG=$config BUILD="$build" "$build/dynamic/libcrypto-compat.so" \
    "$build/dynamic/libssl-compat.so" "$build/dynamic/libc.so" > "$out/make-library.txt" 2>&1 ||
    fail "library build, $out/make-library.txt"
if grep -qi 'warning' "$out/make-library.txt"; then
	fail "warnings in the library build, $out/make-library.txt"
fi
sh plan/ws335/tests/check-exports.sh "$build/dynamic/libcrypto-compat.so" "$build/dynamic/libssl-compat.so" \
    > "$out/check-exports.txt" 2>&1 || fail "check-exports, $out/check-exports.txt"
tail -1 "$out/check-exports.txt"

# 2. tls-client for zedBSD, linked against the two libraries (the dynamic program of the image).
sysroot=$top/build/amd64/sysroot
cc="$top/build/llvm/bin/clang --target=x86_64-unknown-zedbsd --sysroot=$sysroot"
[ -f "$sysroot/usr/lib/crt1.o" ] || fail "no $sysroot/usr/lib/crt1.o"
if ! $cc -nostdinc -I. -Iinclude -isystem "$sysroot/usr/include" -DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64 \
    -DKERN_DYNAMIC_LIBC -m64 -march=x86-64 -mno-red-zone -O2 -ffreestanding -fPIE -fno-builtin -fno-stack-protector \
    -Wall -Wextra -Werror -c plan/ws335/tests/tls-client.c -o "$out/tls-client.o" > "$out/build-client.txt" 2>&1 ||
    ! $cc -m64 -nostdlib -pie -Wl,--no-relax -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
    -Wl,-z,stack-size=0x100000 -Wl,--dynamic-linker=/lib/ld.so "$sysroot/usr/lib/crt1.o" "$out/tls-client.o" \
    -L"$build/dynamic" -Wl,-rpath-link,"$build/dynamic" -l:libssl-compat.so -l:libcrypto-compat.so -l:libc.so \
    -o "$out/tls-client" >> "$out/build-client.txt" 2>&1 || [ -s "$out/build-client.txt" ]; then
	head -20 "$out/build-client.txt"
	fail "tls-client build, $out/build-client.txt"
fi
"$python" tools/build/check-dynamic-elf.py --machine amd64 --role application --needed libssl-compat.so \
    --needed libcrypto-compat.so --needed libc.so "$out/tls-client" > "$out/check-client.txt" 2>&1 ||
    fail "check-dynamic-elf of tls-client, $out/check-client.txt"
echo "run-guest: tls-client built for zedBSD (warning 0), needs libssl-compat.so, libcrypto-compat.so, libc.so"

# 3. The test PKI and the image.
sh plan/ws335/tests/run-interop.sh --make-pki "$top/$out/pki" > "$out/pki.txt" 2>&1 || fail "test PKI, $out/pki.txt"
timeout 3000 sh plan/tools/guest/test-image.sh --no-harness $config "$build" \
    --file /root/tls-client="$out/tls-client" --mode /root/tls-client=0755 \
    --file /root/ca.pem="$out/pki/ca.pem" --file /etc/net.conf=plan/tools/guest/net.conf \
    > "$out/make-image.txt" 2>&1 || fail "image build, $out/make-image.txt"
sync

# 4. The guest: UEFI, NVMe (snapshot), the serial console on a unix socket, the user network on a USB adapter.
. plan/tools/guest/qemu-accel.sh
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
cp "${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}" "$out/vars.fd"
socket=$top/$out/serial.sock
timeout 1800 qemu-system-x86_64 -name ws335-p1 -machine q35 -m 8G -smp 4 $(qemu_accel_args max) \
    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
    -drive "if=pflash,format=raw,file=$out/vars.fd" \
    -drive "if=none,id=boot,file=$build/hdd-image.img,format=raw,snapshot=on" \
    -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
    -device qemu-xhci,id=xhci \
    -netdev user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dhcpstart=10.0.2.15,dns=10.0.2.3 \
    -device usb-net,bus=xhci.0,port=1,id=ecm,netdev=net0,mac=52:54:00:33:50:01,msos-desc=on \
    -vga std -display none -no-reboot \
    -serial "unix:$socket,server=on,wait=off" -monitor none > "$out/qemu.txt" 2>&1 &
qemu=$!
trap 'kill "$qemu" 2> /dev/null; wait "$qemu" 2> /dev/null; sync' EXIT
tries=0
while [ ! -S "$socket" ] && [ $tries -lt 600 ]; do
	kill -0 "$qemu" 2> /dev/null || fail "QEMU ended, $out/qemu.txt"
	sleep 0.1
	tries=$((tries + 1))
done
serial="$python plan/tools/guest/serial.py --socket $socket --timeout 240"
$serial expect 'login: ' > /dev/null || fail "no login prompt"
$serial login > /dev/null || fail "login"
echo "run-guest: logged in on the serial console"

# 5. The network: ue0 has its DHCP address.
tries=0
while [ $tries -lt 30 ]; do
	if $serial run 'ifconfig ue0' > "$out/ifconfig.txt" 2>&1 && grep -q 'inet 10\.0\.2\.' "$out/ifconfig.txt"; then
		break
	fi
	sleep 2
	tries=$((tries + 1))
done
grep -q 'inet 10\.0\.2\.' "$out/ifconfig.txt" || fail "no DHCP address on ue0, $out/ifconfig.txt"
echo "run-guest: ue0 $(grep -o 'inet 10\.0\.2\.[0-9]*' "$out/ifconfig.txt")"
$serial run 'uname -a; ls -l /lib/libssl-compat.so /lib/libcrypto-compat.so /root/tls-client' > "$out/guest-files.txt" 2>&1 ||
    fail "the files in the guest, $out/guest-files.txt"
cat "$out/guest-files.txt"

# 6. The cases.
sh plan/ws335/tests/run-interop.sh --guest "$socket" "$top/$out/pki" > "$out/interop.txt" 2>&1
code=$?
cat "$out/interop.txt"
[ $code -eq 0 ] || fail "run-interop.sh --guest, $out/interop.txt"
echo "run-guest: PASS"

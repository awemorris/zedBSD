#!/bin/sh
# ws004-p022 disposable QEMU NVMe Identify acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 AMD64-HDD-IMAGE OUTPUT-DIRECTORY" >&2
	exit 2
fi

source_image=$1
output=$2
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
timeout_seconds=${BOOT_TIMEOUT_SECONDS:-90}

test -f "$source_image" || {
	echo "image not found: $source_image" >&2
	exit 2
}
case $timeout_seconds in
	'' | *[!0-9]* | 0) echo "invalid BOOT_TIMEOUT_SECONDS" >&2; exit 2 ;;
esac
command -v "$qemu" >/dev/null
command -v rg >/dev/null
command -v sha256sum >/dev/null
test -f "$ovmf_code" || { echo "OVMF code not found: $ovmf_code" >&2; exit 2; }
test -f "$ovmf_vars" || { echo "OVMF vars not found: $ovmf_vars" >&2; exit 2; }

mkdir -p "$output"
boot_image=$output/boot.img
nvme_image=$output/nvme.img
guest_log=$output/guest.log
qemu_log=$output/qemu.log
metadata=$output/metadata.txt
vars=$output/OVMF_VARS.fd
qemu_pid=

cleanup()
{
	if [ -n "$qemu_pid" ]; then
		kill "$qemu_pid" 2>/dev/null || true
		wait "$qemu_pid" 2>/dev/null || true
	fi
}
trap cleanup EXIT HUP INT TERM

cp --reflink=auto --sparse=always "$source_image" "$boot_image"
cp "$ovmf_vars" "$vars"
truncate -s 32M "$nvme_image"
source_digest=$(sha256sum "$source_image" | awk '{print $1}')
namespace_digest=$(sha256sum "$nvme_image" | awk '{print $1}')
: >"$guest_log"
: >"$qemu_log"

{
	echo "qemu=$($qemu --version | sed -n '1p')"
	echo "source_image=$source_image"
	echo "source_sha256=$source_digest"
	echo "namespace_image=$nvme_image"
	echo "namespace_sha256_before=$namespace_digest"
	echo "topology=OVMF q35 piix3-ide root plus one standard PCI NVMe namespace"
} >"$metadata"

"$qemu" \
	-machine q35 \
	-m 512 \
	-smp 4 \
	-drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
	-drive if=pflash,format=raw,file="$vars" \
	-device piix3-ide,id=legacyide \
	-drive if=none,id=bootdisk,file="$boot_image",format=raw \
	-device ide-hd,bus=legacyide.0,drive=bootdisk,bootindex=1 \
	-drive if=none,id=nvmedisk,file="$nvme_image",format=raw \
	-device nvme,id=nvme0,drive=nvmedisk,serial=ZEDBSD-P022 \
	-display none \
	-monitor none \
	-serial none \
	-debugcon file:"$guest_log" \
	-no-reboot >"$qemu_log" 2>&1 &
qemu_pid=$!

deadline=$(($(date +%s) + timeout_seconds))
result=timeout
failure=
failure_pattern='fatal:|FATAL:|kernel panic|panic:|amd64 fault v=|nvme: .*failed|nvme: .*timed out|nvme: .*quarantined|VFS initialization failed'
while [ "$(date +%s)" -lt "$deadline" ]; do
	if rg -a -q -- "$failure_pattern" "$guest_log" 2>/dev/null; then
		result=guest-failure
		failure=$(rg -a -m 1 -- "$failure_pattern" "$guest_log" |
		    tr '\t\r\n' '   ')
		break
	fi
	if rg -a -q 'login:' "$guest_log" 2>/dev/null; then
		result=pass
		break
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		result=early-qemu-exit
		failure='QEMU exited before login'
		break
	fi
	sleep 0.1
done

kill "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true
qemu_pid=

if [ "$result" = pass ]; then
	for marker in 'nvme: PCI controller' \
	    'nvme: /dev/nvme0n1 namespace=1 blocks=00000000:00010000 block-size=512 read-only' \
	    'login:'; do
		if ! rg -a -F -q -- "$marker" "$guest_log"; then
			result=missing-marker
			failure="missing $marker"
			break
		fi
	done
fi
if [ "$result" = pass ] &&
    [ "$(rg -a -F -c -- 'nvme: /dev/nvme' "$guest_log")" -ne 1 ]; then
	result=namespace-count
	failure='expected exactly one published NVMe namespace'
fi
if [ "$result" = pass ] &&
    rg -a -q -- "$failure_pattern" "$guest_log"; then
	result=guest-failure
	failure=$(rg -a -m 1 -- "$failure_pattern" "$guest_log" |
	    tr '\t\r\n' '   ')
fi
if [ "$(sha256sum "$source_image" | awk '{print $1}')" != \
    "$source_digest" ]; then
	result=source-image-mutated
	failure='source system image changed'
fi
namespace_after=$(sha256sum "$nvme_image" | awk '{print $1}')
if [ "$namespace_after" != "$namespace_digest" ]; then
	result=namespace-written
	failure='p022 changed the read-only discovery namespace'
fi

{
	echo "namespace_sha256_after=$namespace_after"
	echo "result=$result"
	echo "failure=$failure"
} >>"$metadata"

if [ "$result" != pass ]; then
	echo "HW-T20 QEMU NVMe admin: FAIL ($result: $failure)" >&2
	exit 1
fi
echo "HW-T20 QEMU NVMe admin: PASS"

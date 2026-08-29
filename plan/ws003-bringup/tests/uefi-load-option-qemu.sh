#!/usr/bin/env bash
# BR-T48: exercise production efi_main with Dell-style whole LoadOptions.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

if (($# != 2)); then
	echo "usage: $0 IMAGE OUTPUT-DIRECTORY" >&2
	exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
image=$1
output=$2
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
efi_cc=${EFI_CC:-x86_64-w64-mingw32-gcc}
efi_ld=${EFI_LD:-x86_64-w64-mingw32-ld}
efi_nm=${EFI_NM:-x86_64-w64-mingw32-nm}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
settle_seconds=${SETTLE_SECONDS:-1}
memory_mib=${MEMORY_MIB:-4096}
smp_cpus=${SMP_CPUS:-4}
pid=

usage_error()
{
	echo "$1" >&2
	exit 2
}

case $boot_timeout:$settle_seconds:$memory_mib:$smp_cpus in
*[!0-9:]*|0:*|*:0:*|*:0:*|*:0)
	usage_error "timeouts, MEMORY_MIB, and SMP_CPUS must be positive integers"
	;;
esac

test -f "$image" || usage_error "image not found: $image"
test -f "$ovmf_code" || usage_error "OVMF code not found: $ovmf_code"
test -f "$ovmf_vars" || usage_error "OVMF variables not found: $ovmf_vars"
for command in "$qemu" "$efi_cc" "$efi_ld" "$efi_nm" cp grep kill \
    mcopy mkdir rg sha256sum; do
	command -v "$command" >/dev/null || usage_error "command not found: $command"
done

cleanup()
{
	if [[ -n ${pid:-} ]]; then
		kill "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

mkdir -p -- "$output/loader"
loader_dir=$output/loader
test_loader=$loader_dir/BOOTX64.EFI
run_image=$output/load-option.img
vars_copy=$output/OVMF_VARS.fd
guest_log=$output/guest.log
qemu_log=$output/qemu.log
base_digest=$(sha256sum "$image" | awk '{print $1}')

efi_cflags=(-std=c11 -ffreestanding -fshort-wchar -mno-red-zone
	-fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables
	-fno-unwind-tables -fno-ident -ffunction-sections -fdata-sections
	-Os -Wall -Wextra -Werror -I"$repo")

"$efi_cc" "${efi_cflags[@]}" -Defi_main=zedbsd_loader_main \
	-c "$repo/bootloader/uefi/bootx64.c" -o "$loader_dir/bootx64.o"
"$efi_cc" "${efi_cflags[@]}" \
	-c "$repo/bootloader/uefi/load-options.c" \
	-o "$loader_dir/load-options.o"
"$efi_cc" "${efi_cflags[@]}" -c "$repo/bootloader/uefi/elf64.c" \
	-o "$loader_dir/elf64.o"
"$efi_cc" "${efi_cflags[@]}" -c "$repo/bootloader/uefi/memory-map.c" \
	-o "$loader_dir/memory-map.o"
"$efi_cc" -m64 -mno-red-zone -c "$repo/bootloader/uefi/transition.S" \
	-o "$loader_dir/transition.o"
"$efi_cc" "${efi_cflags[@]}" -c "$script_dir/uefi-load-option-wrapper.c" \
	-o "$loader_dir/wrapper.o"
"$efi_ld" -mi386pep --subsystem 10 --entry efi_main --image-base 0 \
	--gc-sections --enable-reloc-section --no-insert-timestamp \
	"$loader_dir/wrapper.o" "$loader_dir/bootx64.o" \
	"$loader_dir/elf64.o" "$loader_dir/memory-map.o" \
	"$loader_dir/load-options.o" "$loader_dir/transition.o" \
	-o "$test_loader"
if "$efi_nm" -u "$test_loader" | grep -Ev \
    ' (__bss_start__|__bss_end__|__end__|___tls_start__|___tls_end__)$' | \
    grep -q .; then
	"$efi_nm" -u "$test_loader" >&2
	exit 1
fi

cp --reflink=auto --sparse=always "$image" "$run_image"
cp "$ovmf_vars" "$vars_copy"
mcopy -o -i "$run_image@@1048576" "$test_loader" \
	::/EFI/BOOT/BOOTX64.EFI
: >"$guest_log"
: >"$qemu_log"

{
	echo "base_image=$image"
	echo "base_sha256=$base_digest"
	echo "test_loader=$test_loader"
	echo "test_loader_sha256=$(sha256sum "$test_loader" | awk '{print $1}')"
	echo "qemu=$($qemu --version | sed -n '1p')"
	echo "memory_mib=$memory_mib"
	echo "smp_cpus=$smp_cpus"
	echo "boot_timeout_seconds=$boot_timeout"
	echo "injected=complete EFI_LOAD_OPTION, empty OptionalData"
} >"$output/metadata.txt"

"$qemu" \
	-machine q35 \
	-m "$memory_mib" \
	-smp "$smp_cpus" \
	-drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
	-drive if=pflash,format=raw,file="$vars_copy" \
	-device qemu-xhci,id=xhci \
	-drive if=none,id=usbboot,file="$run_image",format=raw \
	-device usb-storage,bus=xhci.0,drive=usbboot,id=bootstick,bootindex=1 \
	-netdev user,id=net0 \
	-device ne2k_isa,netdev=net0,iobase=0x300,irq=10 \
	-display none \
	-monitor none \
	-serial none \
	-debugcon file:"$guest_log" \
	-no-reboot >"$qemu_log" 2>&1 &
pid=$!

failure_pattern='UEFI LoadOptions rejected:|Validate LoadOptions:|Prepare LoadOptions:|fatal:|kernel panic|panic:|amd64 fault v=|loop1: write .*error=[1-9]|usb-storage: BOT .*error=[1-9]|usb-storage: sda op=2a .*error=[1-9]|xhci: transfer completion=|xhci: control |xhci: command [0-9][0-9]* failed'
start=$(date +%s)
deadline=$((start + boot_timeout))
login_time=0
result=
while :; do
	now=$(date +%s)
	if rg -a -q "$failure_pattern" "$guest_log"; then
		result=boot-failure
		break
	fi
	if ((login_time == 0)) && rg -a -q 'login:' "$guest_log"; then
		login_time=$now
	fi
	if ((login_time != 0 && now >= login_time + settle_seconds)); then
		result=pass
		break
	fi
	if ((now >= deadline)); then
		result=boot-timeout
		break
	fi
	if ! kill -0 "$pid" 2>/dev/null; then
		result=early-qemu-exit
		break
	fi
	sleep 0.1
done
cleanup
pid=

if [[ $result == pass ]]; then
	for marker in \
	    'BR-T48 EFI_LOAD_OPTION injected optional-data=0' \
	    'A64 UEFI ENTRY' \
	    'UEFI LoadOptions descriptor: using OptionalData' \
	    'A64 UEFI ELF' \
	    'A64 UEFI BOOT SERVICES EXITED' \
	    'A64 UEFI EXIT' \
	    'A64 ENTRY PASS' \
	    'login:'; do
		if ! rg -a -F -q "$marker" "$guest_log"; then
			result="missing-marker: $marker"
			break
		fi
	done
fi
if [[ $result == pass ]] && rg -a -q "$failure_pattern" "$guest_log"; then
	result=boot-failure
fi
if [[ $(sha256sum "$image" | awk '{print $1}') != "$base_digest" ]]; then
	echo "BR-T48 FAIL: pristine input image changed" >&2
	exit 1
fi
if [[ $result != pass ]]; then
	echo "BR-T48 FAIL: $result" >&2
	rg -a -m 1 "$failure_pattern" "$guest_log" >&2 || true
	tail -n 80 "$guest_log" >&2
	exit 1
fi

echo "BR-T48 PASS: whole EFI_LOAD_OPTION with empty OptionalData reached login"

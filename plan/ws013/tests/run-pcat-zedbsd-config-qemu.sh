#!/usr/bin/env bash
# PC/AT BIOS/UEFI zedbsd.cfg production-loader acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
qemu_i386=${QEMU_SYSTEM_I386:-qemu-system-i386}
qemu_x86_64=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
amd64_image=${AMD64_IMAGE:-$repo/build/amd64/bios-hdd-image.img}
i386_overlay=${I386_OVERLAY_IMAGE:-$repo/build/pcat/bios-hdd-image.img}
i386_native=${I386_NATIVE_IMAGE:-$repo/build/pcat/ufs-root-hdd-image.img}
output=${1:-}

if [[ -z $output ]]; then
	echo "usage: $0 OUTPUT" >&2
	exit 2
fi
output=$(realpath -m -- "$output")
if [[ -e $output && -n $(find "$output" -mindepth 1 -print -quit) ]]; then
	echo "output directory is not empty: $output" >&2
	exit 2
fi
mkdir -p -- "$output"

for command in cmp cp dd find head mcopy mdel mmd od realpath rg sed sleep \
    timeout tr; do
	command -v "$command" >/dev/null || {
		echo "required host command not found: $command" >&2
		exit 2
	}
done
command -v "$qemu_i386" >/dev/null
command -v "$qemu_x86_64" >/dev/null
[[ -f $ovmf_code && -f $ovmf_vars && -f $amd64_image &&
    -f $i386_overlay && -f $i386_native ]]

temporary=$(mktemp -d /tmp/ws013-p005-pcat.XXXXXX)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
payload_offset=68157440
esp_offset=1048576

run_bios()
{
	local name=$1 source=$2 machine=$3 seconds=$4 expected=$5
	local image=$temporary/$name.img log=$output/$name-debugcon.log status
	cp --reflink=auto --sparse=always "$source" "$image"
	set +e
	timeout "$seconds" "$machine" -machine pc -m 512 -smp 1 \
	    -drive "file=$image,format=raw,if=ide,index=0" \
	    -display none -serial none -debugcon "file:$log" \
	    -monitor none -no-reboot >"$output/$name-qemu.log" 2>&1
	status=$?
	set -e
	[[ $status -eq 124 ]]
	rg -a -U --multiline-dotall -q "$expected" "$log"
	printf '%s\tPASS\n' "$name" >>"$output/results.tsv"
}

run_usb_bios()
{
	local image=$temporary/amd64-usb-bios.img
	local log=$output/amd64-usb-bios-debugcon.log status
	cp --reflink=auto --sparse=always "$amd64_image" "$image"
	set +e
	timeout 75 "$qemu_x86_64" -machine q35 -m 512 -smp 4 \
	    -device qemu-xhci,id=xhci \
	    -drive "if=none,id=boot,file=$image,format=raw" \
	    -device usb-storage,bus=xhci.0,drive=boot,id=bootstick,bootindex=1 \
	    -display none -serial none -debugcon "file:$log" \
	    -monitor none -no-reboot >"$output/amd64-usb-bios-qemu.log" 2>&1
	status=$?
	set -e
	[[ $status -eq 124 ]]
	rg -a -q '^123S4 MB P1 CF UUID=' "$log"
	rg -a -q '^KF E6 LD GO$' "$log"
	rg -a -q '^usb-storage: sda blocks=' "$log"
	rg -a -q '(^|[[:blank:]])login:[[:blank:]]*$' "$log"
	printf 'amd64-usb-bios\tPASS\n' >>"$output/results.tsv"
}

run_uefi()
{
	local image=$temporary/amd64-uefi.img vars=$temporary/OVMF_VARS.fd
	local log=$output/amd64-uefi-debugcon.log status
	cp --reflink=auto --sparse=always "$amd64_image" "$image"
	cp "$ovmf_vars" "$vars"
	set +e
	timeout 100 "$qemu_x86_64" -machine q35 -m 512 -smp 4 \
	    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
	    -drive "if=pflash,format=raw,file=$vars" \
	    -device qemu-xhci,id=xhci \
	    -drive "if=none,id=boot,file=$image,format=raw" \
	    -device usb-storage,bus=xhci.0,drive=boot,id=bootstick,bootindex=1 \
	    -display none -serial none -debugcon "file:$log" \
	    -monitor none -no-reboot >"$output/amd64-uefi-qemu.log" 2>&1
	status=$?
	set -e
	[[ $status -eq 124 ]]
	rg -a -q '^A64 CFG SELECTED ' "$log"
	rg -a -q '^A64 UEFI EXIT$' "$log"
	rg -a -q '(^|[[:blank:]])login:[[:blank:]]*$' "$log"
	printf 'amd64-uefi\tPASS\n' >>"$output/results.tsv"
}

run_negative()
{
	local name=$1 image=$2 expected=$3 machine=${4:-$qemu_x86_64}
	local log=$output/$name-debugcon.log status
	set +e
	timeout 6 "$machine" -machine pc -m 128 -smp 1 \
	    -drive "file=$image,format=raw,if=ide,index=0" \
	    -display none -serial none -debugcon "file:$log" \
	    -monitor none -no-reboot >"$output/$name-qemu.log" 2>&1
	status=$?
	set -e
	[[ $status -eq 124 ]]
	rg -a -U --multiline-dotall -q "$expected" "$log"
	if rg -a -q ' GO|A64 ENTRY PASS|boot: parameters:' "$log"; then
		echo "$name: loader continued after fatal media rejection" >&2
		return 1
	fi
	printf '%s\tPASS\n' "$name" >>"$output/results.tsv"
}

find_root_short_entry()
{
	local image=$1 short_name=$2 index=0 entry
	local reserved fats fat_sectors spc root_cluster data_offset root_offset
	reserved=$(od -An -tu2 -j $((payload_offset + 14)) -N2 "$image" | tr -d ' ')
	fats=$(od -An -tu1 -j $((payload_offset + 16)) -N1 "$image" | tr -d ' ')
	fat_sectors=$(od -An -tu4 -j $((payload_offset + 36)) -N4 "$image" | tr -d ' ')
	spc=$(od -An -tu1 -j $((payload_offset + 13)) -N1 "$image" | tr -d ' ')
	root_cluster=$(od -An -tu4 -j $((payload_offset + 44)) -N4 "$image" | tr -d ' ')
	root_cluster=$((root_cluster & 0x0fffffff))
	data_offset=$((payload_offset + (reserved + fats * fat_sectors) * 512))
	root_offset=$((data_offset + (root_cluster - 2) * spc * 512))
	while [[ $index -lt $((spc * 16)) ]]; do
		entry=$((root_offset + index * 32))
		if cmp -s \
		    <(dd if="$image" bs=1 skip="$entry" count=11 status=none) \
		    <(printf '%s' "$short_name"); then
			printf '%s %s %s\n' "$entry" "$data_offset" "$spc"
			return 0
		fi
		index=$((index + 1))
	done
	return 1
}

file_data_offset()
{
	local image=$1 short_name=$2 entry data_offset spc low high cluster
	read -r entry data_offset spc < <(find_root_short_entry "$image" "$short_name")
	low=$(od -An -tu2 -j $((entry + 26)) -N2 "$image" | tr -d ' ')
	high=$(od -An -tu2 -j $((entry + 20)) -N2 "$image" | tr -d ' ')
	cluster=$(((high << 16 | low) & 0x0fffffff))
	[[ $cluster -ge 2 ]]
	printf '%s\n' $((data_offset + (cluster - 2) * spc * 512))
}

printf 'case\tresult\n' >"$output/results.tsv"
run_bios i386-overlay "$i386_overlay" "$qemu_i386" 40 \
	'^123S4 MB P1 CF UUID=.* GO.*login:[[:blank:]]*'
run_bios i386-native "$i386_native" "$qemu_i386" 40 \
	'^123S4 MB P1 CF UUID=.* GO.*vfs: rootpart selector /dev/sda2 resolved to /dev/sda2.*init: system running'
run_bios amd64-bios "$amd64_image" "$qemu_x86_64" 50 \
	'^123S4 MB P1 CF UUID=.* GO.*login:[[:blank:]]*'
run_usb_bios
run_uefi

bios_uuid=$(sed -n 's/^123S4 MB P1 CF UUID=\([^ ]*\).*/\1/p' \
    "$output/amd64-bios-debugcon.log" | head -n 1)
uefi_uuid=$(sed -n 's/^A64 CFG UUID //p' \
    "$output/amd64-uefi-debugcon.log" | head -n 1)
[[ -n $bios_uuid && $bios_uuid == "$uefi_uuid" ]]
bios_parameters=$(sed -n 's/^boot: parameters: //p' \
    "$output/amd64-bios-debugcon.log" | head -n 1)
uefi_parameters=$(sed -n 's/^A64 PARAMS //p' \
    "$output/amd64-uefi-debugcon.log" | head -n 1)
[[ -n $bios_parameters && $bios_parameters == "$uefi_parameters" ]]

i386_missing_config=$temporary/i386-missing-config.img
cp --reflink=auto --sparse=always "$i386_overlay" "$i386_missing_config"
mdel -i "$i386_missing_config@@$esp_offset" ::/ZEDBSD.CFG
run_negative i386-missing-config "$i386_missing_config" \
    '^123S4 MB P1 ERR C$' "$qemu_i386"

i386_invalid_config=$temporary/i386-invalid-config.img
cp --reflink=auto --sparse=always "$i386_overlay" "$i386_invalid_config"
mcopy -o -i "$i386_invalid_config@@$esp_offset" \
    "$script_dir/pcat-invalid.cfg" ::/ZEDBSD.CFG
run_negative i386-invalid-config "$i386_invalid_config" \
    '^123S4 MB P1 ERR C$' "$qemu_i386"

i386_missing_kernel=$temporary/i386-missing-kernel.img
cp --reflink=auto --sparse=always "$i386_overlay" "$i386_missing_kernel"
mcopy -o -i "$i386_missing_kernel@@$esp_offset" \
    "$script_dir/pcat-missing-kernel.cfg" ::/ZEDBSD.CFG
run_negative i386-missing-kernel "$i386_missing_kernel" \
    '^123S4 MB P1 CF UUID=.*ERR V$' "$qemu_i386"

i386_bad_elf=$temporary/i386-bad-vmunix
cp "$repo/build/pcat/vmunix" "$i386_bad_elf"
printf '\377\377\377\377' | dd of="$i386_bad_elf" bs=1 seek=68 \
    conv=notrunc status=none
i386_elf_bounds=$temporary/i386-elf-bounds.img
cp --reflink=auto --sparse=always "$i386_overlay" "$i386_elf_bounds"
mcopy -o -i "$i386_elf_bounds@@$esp_offset" "$i386_bad_elf" ::/VMUNIX
run_negative i386-elf-bounds "$i386_elf_bounds" \
    '^123S4 MB P1 CF UUID=.*KF ERR E$' "$qemu_i386"

i386_bad_destination=$temporary/i386-bad-destination-vmunix
cp "$repo/build/pcat/vmunix" "$i386_bad_destination"
printf '\000\000\010\000' | dd of="$i386_bad_destination" bs=1 seek=64 \
    conv=notrunc status=none
i386_elf_destination=$temporary/i386-elf-destination.img
cp --reflink=auto --sparse=always "$i386_overlay" "$i386_elf_destination"
mcopy -o -i "$i386_elf_destination@@$esp_offset" \
    "$i386_bad_destination" ::/VMUNIX
run_negative i386-elf-destination "$i386_elf_destination" \
    '^123S4 MB P1 CF UUID=.*KF ERR E$' "$qemu_i386"

i386_lfn=$temporary/i386-lfn.img
cp --reflink=auto --sparse=always "$i386_overlay" "$i386_lfn"
mmd -i "$i386_lfn@@$esp_offset" ::/configured-kernels
mcopy -i "$i386_lfn@@$esp_offset" "$repo/build/pcat/vmunix" \
    ::/configured-kernels/zedBSD-i386-kernel-v1.elf
mcopy -o -i "$i386_lfn@@$esp_offset" "$script_dir/pcat-lfn-i386.cfg" \
    ::/ZEDBSD.CFG
run_bios i386-lfn-subdir "$i386_lfn" "$qemu_i386" 40 \
    '^123S4 MB P1 CF UUID=.*KERNEL=configured-kernels/zedBSD-i386-kernel-v1.elf.* GO.*login:[[:blank:]]*'

missing_config=$temporary/missing-config.img
cp --reflink=auto --sparse=always "$amd64_image" "$missing_config"
mdel -i "$missing_config@@$payload_offset" ::/zedbsd.cfg
run_negative missing-config "$missing_config" '^123S4 MB P1 ERR C$'

esp_only=$temporary/esp-only-config.img
cp --reflink=auto --sparse=always "$amd64_image" "$esp_only"
mdel -i "$esp_only@@$payload_offset" ::/zedbsd.cfg
mcopy -o -i "$esp_only@@$esp_offset" "$repo/platform/amd64/zedbsd.cfg" \
    ::/zedbsd.cfg
run_negative esp-only-config "$esp_only" '^123S4 MB P1 ERR C$'

invalid_config=$temporary/invalid-config.img
cp --reflink=auto --sparse=always "$amd64_image" "$invalid_config"
mcopy -o -i "$invalid_config@@$payload_offset" "$script_dir/pcat-invalid.cfg" \
    ::/zedbsd.cfg
run_negative invalid-config "$invalid_config" '^123S4 MB P1 ERR C$'

missing_kernel=$temporary/missing-kernel.img
cp --reflink=auto --sparse=always "$amd64_image" "$missing_kernel"
mcopy -o -i "$missing_kernel@@$payload_offset" \
    "$script_dir/pcat-missing-kernel.cfg" ::/zedbsd.cfg
run_negative missing-kernel "$missing_kernel" '^123S4 MB P1 CF UUID=.*ERR V$'

pbr_checksum=$temporary/pbr-checksum.img
cp --reflink=auto --sparse=always "$amd64_image" "$pbr_checksum"
pbr_data=$(file_data_offset "$pbr_checksum" BOOTZBSDEXE)
[[ $(od -An -tu1 -j $((pbr_data + 100)) -N1 "$pbr_checksum" | tr -d ' ') -ne 90 ]]
printf '\132' | dd of="$pbr_checksum" bs=1 seek=$((pbr_data + 100)) \
    conv=notrunc status=none
run_negative pbr-checksum "$pbr_checksum" '^123P$'

malformed_bpb=$temporary/malformed-bpb.img
cp --reflink=auto --sparse=always "$amd64_image" "$malformed_bpb"
printf '\001\000' | dd of="$malformed_bpb" bs=1 \
    seek=$((payload_offset + 14)) conv=notrunc status=none
run_negative malformed-bpb "$malformed_bpb" '^123P$'

elf_bounds=$temporary/elf-bounds.img
cp --reflink=auto --sparse=always "$amd64_image" "$elf_bounds"
kernel_data=$(file_data_offset "$elf_bounds" 'VMUNIX     ')
printf '\377\377\377\377\377\377\377\377' | dd of="$elf_bounds" bs=1 \
    seek=$((kernel_data + 96)) conv=notrunc status=none
run_negative elf-bounds "$elf_bounds" '^123S4 MB P1 CF UUID=.*KF ERR 6$'

bad_destination=$temporary/bad-destination-vmunix
cp "$repo/build/amd64/vmunix" "$bad_destination"
printf '\000\000\000\100' | dd of="$bad_destination" bs=1 seek=88 \
    conv=notrunc status=none
elf_destination=$temporary/elf-destination.img
cp --reflink=auto --sparse=always "$amd64_image" "$elf_destination"
mcopy -o -i "$elf_destination@@$payload_offset" "$bad_destination" ::/vmunix
run_negative elf-destination "$elf_destination" \
    '^123S4 MB P1 CF UUID=.*KF ERR 6$'

lfn=$temporary/lfn.img
cp --reflink=auto --sparse=always "$amd64_image" "$lfn"
mmd -i "$lfn@@$payload_offset" ::/configured-kernels
mcopy -i "$lfn@@$payload_offset" "$repo/build/amd64/vmunix" \
    ::/configured-kernels/zedBSD-amd64-kernel-v1.elf
mcopy -o -i "$lfn@@$payload_offset" "$script_dir/pcat-lfn.cfg" \
    ::/zedbsd.cfg
run_bios lfn-subdir "$lfn" "$qemu_x86_64" 50 \
	'^123S4 MB P1 CF UUID=.*KERNEL=configured-kernels/zedBSD-amd64-kernel-v1.elf.* GO.*login:[[:blank:]]*'

echo "PC/AT zedbsd.cfg acceptance: PASS (20/20)"

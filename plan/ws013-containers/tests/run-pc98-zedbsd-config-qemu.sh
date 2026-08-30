#!/usr/bin/env bash
# PC-98 BOOTZBSD.CFG production-loader acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
qemu=${QEMU_PC98:-$repo/build/qemu-pc98/build/qemu-system-i386}
overlay=${PC98_OVERLAY_IMAGE:-$repo/build/pc98/bios-hdd-image.img}
native=${PC98_NATIVE_IMAGE:-$repo/build/pc98/ufs-root-hdd-image.img}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-40}
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

for command in cc cmp cp dd find mcopy mdel mmd mktemp od realpath rg sleep timeout tr; do
	command -v "$command" >/dev/null || {
		echo "required host command not found: $command" >&2
		exit 2
	}
done

find_root_short_entry()
{
	local image=$1 short_name=$2 index=0 entry
	while [[ $index -lt $root_entries ]]; do
		entry=$((root_offset + index * 32))
		if cmp -s \
			<(dd if="$image" bs=1 skip="$entry" count=11 status=none) \
			<(printf '%s' "$short_name"); then
			printf '%s\n' "$entry"
			return 0
		fi
		index=$((index + 1))
	done
	return 1
}
[[ -x $qemu ]] || { echo "PC-98 QEMU not found: $qemu" >&2; exit 2; }
[[ -f $overlay ]] || { echo "overlay image not found: $overlay" >&2; exit 2; }
[[ -f $native ]] || { echo "native image not found: $native" >&2; exit 2; }

temporary=$(mktemp -d /tmp/ws013-p006-pc98.XXXXXX)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
decoder=$temporary/boot-parameter-image-tool
cc -std=c11 -Wall -Wextra -Werror \
	"$repo/plan/ws003-bringup/tests/boot-parameter-image-tool.c" \
	-o "$decoder"

qemu_common=(
	-M pc9821,pegc=off,coregraph=on -cpu 486 -smp 1 -m 64M
	-display none -serial none -no-reboot
)

run_positive()
{
	local name=$1 source=$2 parameter_pattern=$3 screen_pattern=$4
	local image=$temporary/$name.img log=$output/$name-debugcon.log
	local monitor=$output/$name-monitor.log vram=$temporary/$name-vram.bin
	local screen=$output/$name-screen.log
	cp --reflink=auto --sparse=always "$source" "$image"
	{
		sleep "$boot_timeout"
		printf 'pmemsave 0xa0000 0x2000 "%s"\n' "$vram"
		sleep 1
		printf 'quit\n'
	} | "$qemu" "${qemu_common[@]}" \
		-drive "if=ide,bus=0,unit=0,format=raw,file=$image" \
		-debugcon "file:$log" -monitor stdio >"$monitor" 2>&1
	"$decoder" decode-pc98-vram "$vram" >"$screen"
	rg -q 'S4 P1 CF CR CP VF E3 LD GO' "$log"
	rg -q "$parameter_pattern" "$log"
	rg -q "$screen_pattern" "$screen"
	if rg -q 'ERR |fatal:|kernel panic|panic:' "$log" "$screen"; then
		echo "$name: unexpected fatal output" >&2
		return 1
	fi
	printf '%s\tPASS\n' "$name" >>"$output/results.tsv"
}

run_negative()
{
	local name=$1 image=$2 expected=$3
	local log=$output/$name-debugcon.log status
	set +e
	timeout 5 "$qemu" "${qemu_common[@]}" \
		-drive "if=ide,bus=0,unit=0,format=raw,file=$image" \
		-debugcon "file:$log" -monitor none >"$output/$name-qemu.log" 2>&1
	status=$?
	set -e
	[[ $status -eq 124 ]]
	rg -q "$expected" "$log"
	if rg -q ' GO' "$log"; then
		echo "$name: loader fell through after its fatal error" >&2
		return 1
	fi
	printf '%s\tPASS\n' "$name" >>"$output/results.tsv"
}

printf 'case\tresult\n' >"$output/results.tsv"
run_positive overlay "$overlay" \
	'^boot: parameters: boot0=UUID=[0-9A-F]{4}-[0-9A-F]{4} overlay-root=boot0:rootfs.img overlay-data=boot0:data.img swap0=boot0:swapfile$' \
	'(^|[[:blank:]])login:[[:blank:]]*$'
run_positive native "$native" \
	'^boot: parameters: boot0=UUID=[0-9A-F]{4}-[0-9A-F]{4} rootpart=/dev/sda2$' \
	'init: system running'

# The production PC-98 FAT uses 1024-byte logical sectors.  Fill root entries
# 7..14, then place a two-entry LFN at entry 15 so its second entry starts in
# the following 512-byte physical half.  A successful boot proves that the
# retained directory state spans both physical reads of one logical sector.
lfn_image=$temporary/lfn-1024-source.img
cp --reflink=auto --sparse=always "$overlay" "$lfn_image"
fat_offset=1048576
bps=$(od -An -tu2 -j $((fat_offset + 11)) -N2 "$lfn_image" | tr -d ' ')
spc=$(od -An -tu1 -j $((fat_offset + 13)) -N1 "$lfn_image" | tr -d ' ')
reserved=$(od -An -tu2 -j $((fat_offset + 14)) -N2 "$lfn_image" | tr -d ' ')
fat_count=$(od -An -tu1 -j $((fat_offset + 16)) -N1 "$lfn_image" | tr -d ' ')
fat_sectors=$(od -An -tu2 -j $((fat_offset + 22)) -N2 "$lfn_image" | tr -d ' ')
root_entries=$(od -An -tu2 -j $((fat_offset + 17)) -N2 "$lfn_image" | tr -d ' ')
total_sectors=$(od -An -tu2 -j $((fat_offset + 19)) -N2 "$lfn_image" | tr -d ' ')
if [[ $total_sectors -eq 0 ]]; then
	total_sectors=$(od -An -tu4 -j $((fat_offset + 32)) -N4 \
		"$lfn_image" | tr -d ' ')
fi
[[ $bps -eq 1024 ]]
root_logical_sectors=$(((root_entries * 32 + bps - 1) / bps))
fat_cluster_count=$(((total_sectors - reserved - fat_count * fat_sectors - root_logical_sectors) / spc))
[[ $fat_cluster_count -gt 0 && $fat_cluster_count -le $((0xefff - 2)) ]]
root_offset=$((fat_offset + (reserved + fat_count * fat_sectors) * bps))
[[ $(od -An -tu1 -j $((root_offset + 7 * 32)) -N1 "$lfn_image" | tr -d ' ') -eq 0 ]]
for name in DUM000.BIN DUM001.BIN DUM002.BIN DUM003.BIN \
	DUM004.BIN DUM005.BIN DUM006.BIN DUM007.BIN; do
	mcopy -i "$lfn_image@@$fat_offset" "$script_dir/pc98-invalid.cfg" \
		"::/$name"
done
mcopy -i "$lfn_image@@$fat_offset" "$repo/build/pc98/vmunix" \
	::/configured-kernels-v1.elf
mcopy -o -i "$lfn_image@@$fat_offset" "$script_dir/pc98-lfn.cfg" \
	::/BOOTZBSD.CFG
[[ $(od -An -tu1 -j $((root_offset + 15 * 32)) -N1 "$lfn_image" | tr -d ' ') -eq 66 ]]
[[ $(od -An -tu1 -j $((root_offset + 15 * 32 + 11)) -N1 "$lfn_image" | tr -d ' ') -eq 15 ]]
[[ $(od -An -tu1 -j $((root_offset + 16 * 32)) -N1 "$lfn_image" | tr -d ' ') -eq 1 ]]
[[ $(od -An -tu1 -j $((root_offset + 16 * 32 + 11)) -N1 "$lfn_image" | tr -d ' ') -eq 15 ]]
run_positive lfn-1024 "$lfn_image" \
	'^boot: parameters: boot0=UUID=[0-9A-F]{4}-[0-9A-F]{4} overlay-root=boot0:rootfs.img overlay-data=boot0:data.img swap0=boot0:swapfile$' \
	'(^|[[:blank:]])login:[[:blank:]]*$'

missing_config=$temporary/missing-config.img
cp --reflink=auto --sparse=always "$overlay" "$missing_config"
mdel -i "$missing_config@@$fat_offset" ::/BOOTZBSD.CFG
run_negative missing-config "$missing_config" '^S4 P1 ERR Q$'

invalid_config=$temporary/invalid-config.img
cp --reflink=auto --sparse=always "$overlay" "$invalid_config"
mcopy -o -i "$invalid_config@@$fat_offset" "$script_dir/pc98-invalid.cfg" \
	::/BOOTZBSD.CFG
run_negative invalid-config "$invalid_config" '^S4 P1 CF CR ERR G$'

missing_kernel=$temporary/missing-kernel.img
cp --reflink=auto --sparse=always "$overlay" "$missing_kernel"
mcopy -o -i "$missing_kernel@@$fat_offset" \
	"$script_dir/pc98-missing-kernel.cfg" ::/BOOTZBSD.CFG
run_negative missing-kernel "$missing_kernel" '^S4 P1 CF CR CP ERR V$'

invalid_kernel=$temporary/invalid-kernel.img
cp --reflink=auto --sparse=always "$overlay" "$invalid_kernel"
mcopy -o -i "$invalid_kernel@@$fat_offset" \
	"$script_dir/pc98-invalid-kernel.cfg" ::/BOOTZBSD.CFG
run_negative invalid-kernel "$invalid_kernel" '^S4 P1 CF CR CP VF ERR E$'

# A globally addressable entry still must belong to an executable PT_LOAD.
entry_outside_kernel=$temporary/entry-outside-vmunix
cp "$repo/build/pc98/vmunix" "$entry_outside_kernel"
printf '\000\000\340\000' | dd of="$entry_outside_kernel" bs=1 seek=24 \
	conv=notrunc status=none
entry_outside=$temporary/entry-outside.img
cp --reflink=auto --sparse=always "$overlay" "$entry_outside"
mcopy -o -i "$entry_outside@@$fat_offset" "$entry_outside_kernel" \
	::/ENTRY.ELF
mcopy -o -i "$entry_outside@@$fat_offset" \
	"$script_dir/pc98-entry-outside.cfg" ::/BOOTZBSD.CFG
run_negative entry-outside "$entry_outside" \
	'^S4 P1 CF CR CP VF ERR E$'

# The PBR must reject a BOOTZBSD.EXE cluster that resolves outside the
# BPB-declared volume before issuing the first loader read.
malformed_bootzbsd_cluster=$temporary/malformed-bootzbsd-cluster.img
cp --reflink=auto --sparse=always "$overlay" "$malformed_bootzbsd_cluster"
bootzbsd_entry=$(find_root_short_entry \
	"$malformed_bootzbsd_cluster" BOOTZBSDEXE)
printf '\360\377' | dd of="$malformed_bootzbsd_cluster" bs=1 \
	seek=$((bootzbsd_entry + 26)) conv=notrunc status=none
run_negative malformed-bootzbsd-cluster "$malformed_bootzbsd_cluster" '^P$'

# The first 512-byte PBR sector must reject a BPB whose logical reserved area
# cannot contain the remaining three physical continuation sectors.  This
# must stop before reading sector 1 as executable code.
short_reserved=$temporary/short-reserved.img
cp --reflink=auto --sparse=always "$overlay" "$short_reserved"
printf '\001\000' | dd of="$short_reserved" bs=1 \
	seek=$((fat_offset + 14)) conv=notrunc status=none
run_negative short-reserved "$short_reserved" '^P$'

# The reserved count alone is insufficient if the BPB claims that the whole
# volume ends before all three physical continuation sectors.  A 1024-byte
# logical total of one describes only two physical sectors.
short_total=$temporary/short-total.img
cp --reflink=auto --sparse=always "$overlay" "$short_total"
printf '\001\000' | dd of="$short_total" bs=1 \
	seek=$((fat_offset + 19)) conv=notrunc status=none
run_negative short-total "$short_total" '^P$'

# The absolute partition start plus the three continuation sectors must not
# wrap the 32-bit BIOS-LBA domain.
wrapped_start=$temporary/wrapped-partition-start.img
cp --reflink=auto --sparse=always "$overlay" "$wrapped_start"
printf '\376\377\377\377' | dd of="$wrapped_start" bs=1 \
	seek=$((fat_offset + 28)) conv=notrunc status=none
run_negative wrapped-partition-start "$wrapped_start" '^P$'

# A directory entry whose first cluster is in FAT16's reserved range must be
# rejected before the loader derives or reads a volume-external LBA from it.
malformed_cluster=$temporary/malformed-cluster.img
cp --reflink=auto --sparse=always "$overlay" "$malformed_cluster"
mmd -i "$malformed_cluster@@$fat_offset" ::/escape
malformed_entry=$((root_offset + 7 * 32))
[[ $(od -An -tu1 -j "$malformed_entry" -N1 "$malformed_cluster" | tr -d ' ') -eq 69 ]]
[[ $(od -An -tu1 -j $((malformed_entry + 11)) -N1 "$malformed_cluster" | tr -d ' ') -eq 16 ]]
printf '\360\377' | dd of="$malformed_cluster" bs=1 \
	seek=$((malformed_entry + 26)) conv=notrunc status=none
mcopy -o -i "$malformed_cluster@@$fat_offset" \
	"$script_dir/pc98-malformed-cluster.cfg" ::/BOOTZBSD.CFG
run_negative malformed-cluster "$malformed_cluster" '^S4 P1 CF CR CP ERR V$'

# A reserved first cluster on BOOTZBSD.CFG must be rejected before any
# configuration bytes are read from an LBA derived from that entry.
malformed_config_cluster=$temporary/malformed-config-cluster.img
cp --reflink=auto --sparse=always "$overlay" "$malformed_config_cluster"
config_entry=$(find_root_short_entry "$malformed_config_cluster" BOOTZBSDCFG)
printf '\360\377' | dd of="$malformed_config_cluster" bs=1 \
	seek=$((config_entry + 26)) conv=notrunc status=none
run_negative malformed-config-cluster "$malformed_config_cluster" \
	'^S4 P1 ERR K$'

# The configured kernel spans multiple clusters.  Replace its first FAT16
# successor with a non-reserved but volume-external cluster number.  The
# loader must reject the chain instead of deriving an external disk LBA.
malformed_kernel_chain=$temporary/malformed-kernel-chain.img
cp --reflink=auto --sparse=always "$overlay" "$malformed_kernel_chain"
kernel_entry=$(find_root_short_entry "$malformed_kernel_chain" 'VMUNIX     ')
kernel_cluster=$(od -An -tu2 -j $((kernel_entry + 26)) -N2 \
	"$malformed_kernel_chain" | tr -d ' ')
fat_start=$((fat_offset + reserved * bps))
printf '\377\357' | dd of="$malformed_kernel_chain" bs=1 \
	seek=$((fat_start + kernel_cluster * 2)) conv=notrunc status=none
run_negative malformed-kernel-chain "$malformed_kernel_chain" \
	'^S4 P1 CF CR CP VF E3 ERR K$'

# Force the configured kernel into the second cluster of a subdirectory,
# then replace that directory's first FAT successor with a volume-external
# cluster.  The path walk must stop before converting the successor to an LBA.
malformed_subdir_chain=$temporary/malformed-subdir-chain.img
cp --reflink=auto --sparse=always "$overlay" "$malformed_subdir_chain"
mmd -i "$malformed_subdir_chain@@$fat_offset" ::/chain
directory_entries_per_cluster=$((bps * spc / 32))
[[ $directory_entries_per_cluster -gt 2 ]]
index=0
while [[ $index -lt $((directory_entries_per_cluster - 2)) ]]; do
	printf -v dummy_name 'D%06d.BIN' "$index"
	mcopy -i "$malformed_subdir_chain@@$fat_offset" \
		"$script_dir/pc98-invalid.cfg" "::/chain/$dummy_name"
	index=$((index + 1))
done
mcopy -i "$malformed_subdir_chain@@$fat_offset" \
	"$repo/build/pc98/vmunix" ::/chain/VMUNIX
mcopy -o -i "$malformed_subdir_chain@@$fat_offset" \
	"$script_dir/pc98-malformed-subdir-chain.cfg" ::/BOOTZBSD.CFG
chain_entry=$(find_root_short_entry "$malformed_subdir_chain" 'CHAIN      ')
chain_cluster=$(od -An -tu2 -j $((chain_entry + 26)) -N2 \
	"$malformed_subdir_chain" | tr -d ' ')
chain_successor=$(od -An -tu2 -j $((fat_start + chain_cluster * 2)) -N2 \
	"$malformed_subdir_chain" | tr -d ' ')
[[ $chain_successor -ge 2 && $chain_successor -lt 65520 ]]
printf '\377\357' | dd of="$malformed_subdir_chain" bs=1 \
	seek=$((fat_start + chain_cluster * 2)) conv=notrunc status=none
run_negative malformed-subdir-chain "$malformed_subdir_chain" \
	'^S4 P1 CF CR CP ERR V$'

echo "PC-98 BOOTZBSD.CFG acceptance: PASS (16/16)"

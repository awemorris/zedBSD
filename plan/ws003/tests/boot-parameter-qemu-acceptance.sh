#!/usr/bin/env bash
# BR-T46: four-production-loader kernel-parameter acceptance matrix.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
qemu_i386=${QEMU_SYSTEM_I386:-qemu-system-i386}
qemu_x86_64=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
qemu_pc98=${QEMU_PC98:-$repo/build/qemu-pc98/build/qemu-system-i386}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-600}
settle_seconds=${SETTLE_SECONDS:-1}
platform_filter=all
case_filter=all
mode=run
output=
noct=$repo/build/NoctLang/build-static/noct

usage()
{
	cat <<EOF
usage: $0 [--list | --dry-run] [--platform NAME] [--case NAME] OUTPUT

Platforms: all, pcat, pc98, amd64-bios, amd64-uefi
Cases:     all, default, shell, native, file-swap, raw-swap, invalid,
           mixed-swap, cross-boot, partuuid-reorder, root-swap-alias

The complete run contains 24 common cells, six amd64-only cells, and one
PC/AT alias-rejection cell.  It uses temporary build configurations, normal
build/{pcat,pc98,amd64} output directories, and QEMU-writable copies below
OUTPUT.  config.mk is never used as an output and is verified unchanged at
the end.
EOF
}

while (($#)); do
	case $1 in
	--list)
		mode=list
		shift
		;;
	--dry-run)
		mode=dry-run
		shift
		;;
	--platform)
		(($# >= 2)) || { usage >&2; exit 2; }
		platform_filter=$2
		shift 2
		;;
	--case)
		(($# >= 2)) || { usage >&2; exit 2; }
		case_filter=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	--*)
		echo "unknown option: $1" >&2
		usage >&2
		exit 2
		;;
	*)
		[[ -z $output ]] || { usage >&2; exit 2; }
		output=$1
		shift
		;;
	esac
done

case $platform_filter in
all|pcat|pc98|amd64-bios|amd64-uefi) ;;
*) echo "invalid platform: $platform_filter" >&2; exit 2 ;;
esac
case $case_filter in
all|default|shell|native|file-swap|raw-swap|invalid|mixed-swap|cross-boot|partuuid-reorder|root-swap-alias) ;;
*) echo "invalid case: $case_filter" >&2; exit 2 ;;
esac
case $boot_timeout:$command_timeout:$settle_seconds in
*[!0-9:]*|0:*|*:0:*|*:0)
	echo "timeouts and settle interval must be positive integers" >&2
	exit 2
	;;
esac

common_cases=(default shell native file-swap raw-swap invalid)
pcat_cases=(default shell native file-swap raw-swap invalid root-swap-alias)
amd64_cases=(default shell native file-swap raw-swap invalid mixed-swap \
    cross-boot partuuid-reorder)

selected()
{
	local platform=$1 case_name=$2
	[[ $platform_filter == all || $platform_filter == "$platform" ]] &&
	    [[ $case_filter == all || $case_filter == "$case_name" ]]
}

list_cells()
{
	local platform case_name count=0
	for case_name in "${pcat_cases[@]}"; do
		if selected pcat "$case_name"; then
			printf 'pcat\t%s\n' "$case_name"
			((count += 1))
		fi
	done
	for case_name in "${common_cases[@]}"; do
		if selected pc98 "$case_name"; then
			printf 'pc98\t%s\n' "$case_name"
			((count += 1))
		fi
	done
	for platform in amd64-bios amd64-uefi; do
		for case_name in "${amd64_cases[@]}"; do
			if selected "$platform" "$case_name"; then
				printf '%s\t%s\n' "$platform" "$case_name"
				((count += 1))
			fi
		done
	done
	if [[ $platform_filter == all && $case_filter == all && $count -ne 31 ]]; then
		echo "internal error: expected 31 cells, found $count" >&2
		return 1
	fi
}

if [[ $mode == list ]]; then
	list_cells
	exit 0
fi
if [[ -z $output ]]; then
	if [[ $mode == dry-run ]]; then
		output=$(mktemp -d /tmp/ws003-br-t46-dry-run.XXXXXX)
		trap 'rm -rf -- "$output"' EXIT
	else
		usage >&2
		exit 2
	fi
fi
output=$(realpath -m -- "$output")
if [[ -e $output && -n $(find "$output" -mindepth 1 -maxdepth 1 -print -quit) ]]; then
	echo "output directory is not empty: $output" >&2
	exit 2
fi
mkdir -p -- "$output/host" "$output/config" "$output/build-logs" \
    "$output/cells"

for command in awk cc chmod cp date find make mktemp realpath rg rm sed \
	sha256sum sleep tar tee touch wc; do
	command -v "$command" >/dev/null || {
		echo "required host command not found: $command" >&2
		exit 2
	}
done

[[ -x $noct ]] || {
	echo "Noct toolchain missing; run make toolchain first" >&2
	exit 2
}
image_tool=$output/host/boot-parameter-image-tool
cc -std=c11 -Wall -Wextra -Werror \
    "$script_dir/boot-parameter-image-tool.c" -o "$image_tool"
"$image_tool" self-test

if [[ $mode == dry-run ]]; then
	list_cells | tee "$output/cells.tsv"
	count=$(wc -l <"$output/cells.tsv")
	if [[ $count -eq 0 ]]; then
		echo "the platform/case filters select no BR-T46 cells" >&2
		exit 2
	fi
	echo "BR-T46 dry-run PASS: image helper and $count selected cells"
	exit 0
fi

for command in "$qemu_i386" "$qemu_x86_64" "$qemu_pc98"; do
	[[ -x $command ]] || command -v "$command" >/dev/null || {
		echo "QEMU binary not found: $command" >&2
		exit 2
	}
done
[[ -f $ovmf_code ]] || { echo "OVMF code not found: $ovmf_code" >&2; exit 2; }
[[ -f $ovmf_vars ]] || { echo "OVMF vars not found: $ovmf_vars" >&2; exit 2; }
config_hash_before=absent
if [[ -f $repo/config.mk ]]; then
	config_hash_before=$(sha256sum "$repo/config.mk" | awk '{print $1}')
fi

write_config()
{
	local target=$1 path=$2 platform architecture board lgy
	case $target in
	pcat) platform=i386; architecture=i386; board=pcat; lgy=n ;;
	pc98) platform=pc98; architecture=i386; board=pc98; lgy=y ;;
	amd64) platform=amd64; architecture=amd64; board=pcat; lgy=n ;;
	*) return 2 ;;
	esac
	cat >"$path" <<EOF
# Generated only for BR-T46; config.mk is not modified.
ZEDBSD_MENU_VERSION := 2
ZEDBSD_PLATFORM := $platform
ZEDBSD_ARCHITECTURE := $architecture
ZEDBSD_BOARD := $board

CONFIG_KERNEL_TEST_CHECKPOINTS := n
CONFIG_BUF_CACHE_KIB := 0
CONFIG_DRIVER_NE2000 := y
CONFIG_DRIVER_PCI_UHCI := y
CONFIG_DRIVER_PCI_EHCI := y
CONFIG_DRIVER_PCI_XHCI := y
CONFIG_DRIVER_USB_STORAGE := y
CONFIG_DRIVER_GRAPHICS := y
CONFIG_DRIVER_LGY98 := $lgy

ZEDBSD_USER_PROGRAMS := dd login rm top
EOF
}

write_config pcat "$output/config/pcat.mk"
write_config pc98 "$output/config/pc98.mk"
write_config amd64 "$output/config/amd64.mk"

qemu_version()
{
	"$1" --version | sed -n '1p'
}

{
	echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "repository=$repo"
	echo "config_mk_sha256_before=$config_hash_before"
	echo "make_parallelism=16"
	echo "boot_timeout_seconds=$boot_timeout"
	echo "command_timeout_seconds=$command_timeout"
	echo "pcat_qemu=$(qemu_version "$qemu_i386")"
	echo "pc98_qemu=$(qemu_version "$qemu_pc98")"
	echo "amd64_qemu=$(qemu_version "$qemu_x86_64")"
	echo "ovmf_code=$ovmf_code"
	echo "ovmf_code_sha256=$(sha256sum "$ovmf_code" | awk '{print $1}')"
	echo "ovmf_vars=$ovmf_vars"
	echo "ovmf_vars_sha256=$(sha256sum "$ovmf_vars" | awk '{print $1}')"
} >"$output/metadata.txt"
list_cells >"$output/cells.tsv"
if [[ $(wc -l <"$output/cells.tsv") -eq 0 ]]; then
	echo "the platform/case filters select no BR-T46 cells" >&2
	exit 2
fi
printf 'platform\tcase\tstatus\timage_sha256\tparameters_sha256\telapsed_seconds\n' \
    >"$output/results.tsv"

default_parameters='overlay-root=boot0:rootfs.img overlay-data=boot0:data.img swap0=boot0:swapfile'
uefi_fat_uuid=6740-911D

parameter_text()
{
	local group=$1 case_name=$2 raw_device native_device
	if [[ $group == amd64 ]]; then
		# GPT entries 1..3 are ESP, payload FAT, and BIOS loader.
		raw_device=/dev/sda4
		native_device=/dev/sda4
	else
		raw_device=/dev/sda2
		native_device=/dev/sda2
	fi
	case $case_name in
	default) printf '%s\n' "$default_parameters" ;;
	shell) printf '%s init=/bin/sh\n' "$default_parameters" ;;
	native) printf 'rootpart=%s\n' "$native_device" ;;
	file-swap) printf '%s init=/bin/sh\n' "$default_parameters" ;;
	raw-swap)
		printf 'overlay-root=boot0:rootfs.img overlay-data=boot0:data.img swap0=%s init=/bin/sh\n' "$raw_device"
		;;
	invalid)
		printf 'rootpart=/dev/sda2 overlay-root=boot0:rootfs.img overlay-data=boot0:data.img\n'
		;;
	mixed-swap)
		printf '%s swap1=%s init=/bin/sh\n' "$default_parameters" "$raw_device"
		;;
	cross-boot)
		printf 'boot1=UUID=A1B2-C3D4 overlay-root=boot0:rootfs.img overlay-data=boot1:data.img swap0=boot0:swapfile\n'
		;;
	partuuid-reorder)
		printf 'boot1=PARTUUID=fbf09090-01 overlay-root=boot0:rootfs.img overlay-data=boot1:data.img swap0=boot0:swapfile\n'
		;;
	root-swap-alias)
		[[ $group == pcat ]] || return 2
		printf 'rootpart=/dev/sda2 swap0=/dev/sda2\n'
		;;
	*) return 2 ;;
	esac
}

write_zedbsd_config()
{
	local parameters=$1 destination=$2 bytes lines longest assembled
	local -a tokens=()
	read -r -a tokens <<<"$parameters"
	((${#tokens[@]} > 0)) || {
		echo 'cannot create zedbsd.cfg from an empty parameter record' >&2
		return 1
	}
	{
		printf 'kernel=vmunix\n'
		printf '%s\n' "${tokens[@]}"
	} >"$destination"
	bytes=$(wc -c <"$destination")
	lines=$(wc -l <"$destination")
	longest=$(awk '{ if (length > maximum) maximum=length }
	    END { print maximum+0 }' "$destination")
	assembled=$(normalized_parameter_text "$parameters")
	if ((bytes > 4096 || lines > 64 || longest > 511 ||
	    ${#assembled} > 3071)); then
		echo "generated zedbsd.cfg exceeds parser bounds: $destination" >&2
		return 1
	fi
}

normalized_parameter_text()
{
	local parameters=$1 token have_boot0=0
	local -a tokens=()
	read -r -a tokens <<<"$parameters"
	for token in "${tokens[@]}"; do
		if [[ $token == boot0=* ]]; then
			have_boot0=1
			break
		fi
	done
	if ((have_boot0)); then
		printf '%s\n' "$parameters"
	else
		printf 'boot0=UUID=%s %s\n' "$uefi_fat_uuid" "$parameters"
	fi
}

build_group_once()
{
	local group=$1 config=$2 log=$3 group_cases=$4 case_name
	local need_native=0 need_swap=0
	local -n build_cases_ref=$group_cases
	local targets=(disk-image)
	for case_name in "${build_cases_ref[@]}"; do
		local any_selected=0
		if [[ $group == amd64 ]]; then
			selected amd64-bios "$case_name" && any_selected=1
			selected amd64-uefi "$case_name" && any_selected=1
		else
			selected "$group" "$case_name" && any_selected=1
		fi
		((any_selected)) || continue
		case $case_name in
		native|root-swap-alias) need_native=1 ;;
		file-swap|raw-swap|mixed-swap) need_swap=1 ;;
		esac
	done
	if ((need_native)); then
		targets+=("build/$group/ufs-root.img")
	fi
	if ((need_swap)); then
		targets+=("build/$group/rootfs.tar.gz"
		    "build/$group/BR-T46-SWAP.ELF")
	fi
	if ! make -C "$repo" -j16 -f Makefile \
	    -f "$script_dir/boot-parameter-acceptance.mk" \
	    ZEDBSD_CONFIG="$config" ARCH_IMAGE_DIR="build/$group/arch-images" \
	    "${targets[@]}" >"$log" 2>&1; then
		tail -n 100 "$log" >&2
		return 1
	fi
}

production_loader_paths()
{
	local group=$1 build_dir=$repo/build/$group
	local stage2_bin=$build_dir/bootloader/stage2-chain.bin
	[[ $group == pc98 ]] && stage2_bin=$build_dir/bootloader/stage2.bin
	printf '%s\n' \
	    "$build_dir/bootloader/stage1.bin" \
	    "$stage2_bin" \
	    "$build_dir/bootloader/partition-pbr.bin" \
	    "$build_dir/bootloader/bootzbsd.raw" \
	    "$build_dir/bootloader/bootzbsd.bin" \
	    "$build_dir/bootloader/BOOTZBSD.EXE"
	if [[ $group == amd64 ]]; then
		printf '%s\n' "$build_dir/bootloader/stage2-chain.raw" \
		    "$build_dir/uefi/BOOTX64.EFI"
	fi
}

hash_production_loaders()
{
	local group=$1 destination=$2 artifact
	: >"$destination"
	while IFS= read -r artifact; do
		[[ -f $artifact ]] || {
			echo "production loader artifact missing: $artifact" >&2
			return 1
		}
		sha256sum "$artifact" >>"$destination"
	done < <(production_loader_paths "$group")
}

prepare_case_loaders()
{
	local group=$1 parameters=$2 destination=$3
	local build_dir=$repo/build/$group
	local stage2_bin=$build_dir/bootloader/stage2-chain.bin
	[[ $group == pc98 ]] && stage2_bin=$build_dir/bootloader/stage2.bin
	mkdir -p -- "$destination"
	# Loader code is invariant across cases.  Each case now supplies the
	# authoritative on-disk configuration instead of patching an embedded BPR1.
	cp "$stage2_bin" "$destination/stage2.bin"
	cp "$build_dir/bootloader/bootzbsd.raw" "$destination/bootzbsd.raw"
	cp "$build_dir/bootloader/bootzbsd.bin" "$destination/bootzbsd.bin"
	cp "$build_dir/bootloader/BOOTZBSD.EXE" "$destination/BOOTZBSD.EXE"
	write_zedbsd_config "$parameters" "$destination/zedbsd.cfg"

	if [[ $group == amd64 ]]; then
		cp "$build_dir/bootloader/stage2-chain.raw" "$destination/stage2.raw"
		cp "$build_dir/uefi/BOOTX64.EFI" "$destination/BOOTX64.EFI"
		"$noct" --path="$repo/tools/build" \
		    "$repo/platform/amd64/tools/check-bootx64.noct" \
		    "$destination/BOOTX64.EFI"
	fi
}

build_extended_image()
{
	local group=$1 case_name=$2 destination=$3 loaders=$4
	local acceptance_rootfs=${5:-}
	local machine build_dir partition_index= kind= payload=
	machine=pcat
	build_dir=$repo/build/$group
	if [[ $group == pc98 ]]; then
		machine=pc98
	fi
	local command=("$repo/build/zedimage-host" disk --machine "$machine"
	    --stage1 "$build_dir/bootloader/stage1.bin"
	    --stage2 "$loaders/stage2.bin"
	    --partition-pbr "$build_dir/bootloader/partition-pbr.bin"
	    --bootzbsd "$loaders/BOOTZBSD.EXE"
	    --kernel "$build_dir/vmunix" --size-mib 201 --fat-size-mib 128
	    --zedbsd-config "$loaders/zedbsd.cfg")
	if [[ $group == amd64 ]]; then
		# Keep aligned room after the shifted payload FAT for an optional
		# acceptance-only native-root or raw-swap GPT partition.
		command+=(--size-mib 260)
		command+=(--gpt --bootx64 "$loaders/BOOTX64.EFI"
		    )
	fi
	case $case_name in
	default|shell|invalid|cross-boot|partuuid-reorder)
		local arch_profile=i386
		[[ $group == amd64 ]] && arch_profile=amd64
		command+=(--arch-image \
		    "$build_dir/arch-images/$arch_profile.ufs"
		    --data-image "$repo/build/data.img"
		    --swapfile "$repo/build/swapfile")
		;;
	native|root-swap-alias)
		partition_index=$([[ $group == amd64 ]] && echo 4 || echo 2)
		kind=ufs
		payload=$build_dir/ufs-root.img
		;;
	file-swap|raw-swap|mixed-swap)
		[[ -n $acceptance_rootfs ]] || return 2
		command+=(--arch-image "$acceptance_rootfs"
		    --data-image "$repo/build/data.img")
		if [[ $case_name == file-swap || $case_name == mixed-swap ]]; then
			command+=(--swapfile "$repo/build/swapfile")
		fi
		if [[ $case_name != file-swap ]]; then
			partition_index=$([[ $group == amd64 ]] && echo 4 || echo 2)
			kind=swap
			payload=$repo/build/swapfile
		fi
		;;
	*) return 2 ;;
	esac
	command+=("$destination")
	"${command[@]}"
	if [[ -n $partition_index ]]; then
		local partition_start=264192
		[[ $group == amd64 ]] && partition_start=495616
		"$image_tool" add-partition --machine "$machine" --kind "$kind" \
		    --index "$partition_index" --start-lba "$partition_start" \
		    --payload "$payload" "$destination"
	fi
	if [[ $case_name == root-swap-alias ]]; then
		"$image_tool" stamp-swap-v2 --partition-index "$partition_index" \
		    "$destination"
	fi
}

build_acceptance_rootfs()
{
	local group=$1 destination=$2 staging=$3
	mkdir -p -- "$staging"
	tar -xzf "$repo/build/$group/rootfs.tar.gz" -C "$staging"
	cp "$repo/build/$group/BR-T46-SWAP.ELF" \
	    "$staging/bin/brt46-swap"
	chmod 0755 "$staging/bin/brt46-swap"
	"$repo/build/zedimage-host" ufs 16777216 "$staging" "$destination"
}

build_auxiliary_image()
{
	local destination=$1 build_dir=$repo/build/amd64
	"$repo/build/zedimage-host" disk --machine pcat \
	    --stage1 "$build_dir/bootloader/stage1.bin" \
	    --stage2 "$build_dir/bootloader/stage2-chain.bin" \
	    --partition-pbr "$build_dir/bootloader/partition-pbr.bin" \
	    --bootzbsd "$build_dir/bootloader/BOOTZBSD.EXE" \
	    --kernel "$build_dir/vmunix" \
	    --data-image "$repo/build/data.img" "$destination"
	"$image_tool" set-fat-uuid --partition-lba 2048 \
	    --uuid A1B2-C3D4 "$destination"
}

marker_count()
{
	local pattern=$1 file=$2 result
	result=$(rg -a -c -- "$pattern" "$file" 2>/dev/null || true)
	printf '%s\n' "${result:-0}"
}

capture_pc98_screen()
{
	local _capture_try
	[[ ${controller_platform:-} == pc98 ]] || return 0
	rm -f -- "$controller_vram"
	printf 'pmemsave 0xa0000 0x2000 "%s"\n' "$controller_vram"
	for ((_capture_try = 0; _capture_try < 50; _capture_try++)); do
		[[ -s $controller_vram ]] && break
		sleep 0.01
	done
	if [[ -s $controller_vram ]]; then
		"$image_tool" decode-pc98-vram "$controller_vram" \
		    >>"$controller_guest_log"
	fi
}

wait_for_pattern()
{
	local pattern=$1 file=$2 timeout=$3 minimum=${4:-1}
	local deadline count
	deadline=$(( $(date +%s) + timeout ))
	while :; do
		capture_pc98_screen
		count=$(marker_count "$pattern" "$file")
		if ((count >= minimum)); then
			return 0
		fi
		if (( $(date +%s) >= deadline )); then
			return 1
		fi
		sleep 0.1
	done
}

wait_for_swap_exercise()
{
	local file=$1 timeout=$2 deadline
	deadline=$(( $(date +%s) + timeout ))
	while :; do
		capture_pc98_screen
		if (( $(marker_count 'BR-T46-SWAP-EXERCISE PASS ' "$file") > 0 )); then
			return 0
		fi
		if (( $(marker_count \
		    'BR-T46-SWAP-EXERCISE (FAIL|SIGNAL) |Segmentation fault|kernel panic|panic:| fault v=' \
		    "$file") > 0 )); then
			return 1
		fi
		if (( $(date +%s) >= deadline )); then
			return 1
		fi
		sleep 0.1
	done
}

send_text()
{
	local text=$1 character key index
	for ((index = 0; index < ${#text}; index++)); do
		character=${text:index:1}
		case $character in
		' ') key=spc ;;
		/) key=slash ;;
		=) key=equal ;;
		-) key=minus ;;
		.) key=dot ;;
		_) key=shift-minus ;;
		[a-z0-9]) key=$character ;;
		*) echo "unsupported sendkey character: $character" >&3; return 1 ;;
		esac
		printf 'sendkey %s\n' "$key"
		sleep 0.015
	done
	printf 'sendkey ret\n'
}

monitor_controller()
{
	local platform=$1 case_name=$2 guest_log=$3 result=$4 vram=$5
	# Serial logs retain the prompt's trailing blank; decoded PC-98 text rows
	# deliberately trim it.  Accept either representation.
	local prompt='root@[^[:space:]]*:[^$]*\$([[:blank:]]|$)'
	local login_prompt='(^|[[:blank:]])login:[[:blank:]]*$'
	controller_platform=$platform
	controller_guest_log=$guest_log
	controller_vram=$vram
	exec 3>>"${result%.txt}-controller.log"
	case $case_name in
	default|cross-boot|partuuid-reorder)
		if wait_for_pattern "$login_prompt" "$guest_log" "$boot_timeout"; then
			sleep "$settle_seconds"
			echo pass >"$result"
		else
			echo 'fail: login timeout' >"$result"
		fi
		;;
	native)
		if wait_for_pattern 'init: system running' "$guest_log" \
		    "$boot_timeout"; then
			sleep "$settle_seconds"
			echo pass >"$result"
		else
			echo 'fail: native-root init timeout' >"$result"
		fi
		;;
	invalid)
		if wait_for_pattern 'vfs: select root mode failed \(error 3\)' \
		    "$guest_log" "$boot_timeout" &&
		    wait_for_pattern 'VFS initialization failed \(3\); entering idle\.' \
		    "$guest_log" 5; then
			sleep "$settle_seconds"
			echo pass >"$result"
		else
			echo 'fail: expected root-mode rejection was not observed' >"$result"
		fi
		;;
	root-swap-alias)
		if wait_for_pattern \
		    'vfs: validate rootpart swap alias failed \(error 16\)' \
		    "$guest_log" "$boot_timeout" &&
		    wait_for_pattern \
		    'VFS initialization failed \(16\); entering idle\.' \
		    "$guest_log" 5; then
			sleep "$settle_seconds"
			echo pass >"$result"
		else
			echo 'fail: expected root/swap alias rejection was not observed' \
			    >"$result"
		fi
		;;
	shell)
		if ! wait_for_pattern "$prompt" "$guest_log" "$boot_timeout"; then
			echo 'fail: interactive shell prompt timeout' >"$result"
		else
			send_text 'echo brt46-shell'
			if wait_for_pattern '^brt46-shell\r?$' "$guest_log" "$command_timeout"; then
				echo pass >"$result"
			else
				echo 'fail: shell input/output marker timeout' >"$result"
			fi
		fi
		;;
	file-swap|raw-swap|mixed-swap)
		if ! wait_for_pattern "$prompt" "$guest_log" "$boot_timeout"; then
			echo 'fail: swap exercise shell prompt timeout' >"$result"
		else
			send_text '/bin/brt46-swap'
			if wait_for_swap_exercise "$guest_log" "$command_timeout"; then
				echo pass >"$result"
			else
				echo 'fail: swap exerciser did not pass' >"$result"
			fi
		fi
		;;
	*) echo 'fail: unknown controller case' >"$result" ;;
	esac
	printf 'quit\n'
}

validate_log()
{
	local platform=$1 case_name=$2 parameters=$3 guest_log=$4
	local raw_device expected_sources boot0_source=UUID=$uefi_fat_uuid
	raw_device=$([[ $platform == amd64-* ]] && echo /dev/sda4 || echo /dev/sda2)
	if ! rg -a -F -q -- "boot: parameters: $parameters" "$guest_log"; then
		echo "missing exact parameter marker" >&2
		return 1
	fi
	case $case_name in
	default)
		rg -a -F -q 'vfs: root=overlay lower=boot0:rootfs.img upper=boot0:data.img' "$guest_log" &&
		rg -a -F -q 'swap: swap0 source=boot0:swapfile slots=' "$guest_log" &&
		rg -a -q 'swap: active sources=1 total=[1-9][0-9]* free=[1-9][0-9]*' "$guest_log" &&
		rg -a -F -q 'boot: starting init /sbin/init' "$guest_log" &&
		rg -a -q '(^|[[:blank:]])login:[[:blank:]]*$' "$guest_log"
		;;
	shell)
		rg -a -F -q 'boot: starting init /bin/sh' "$guest_log" &&
		rg -a -q '^brt46-shell\r?$' "$guest_log"
		;;
	native)
		rg -a -F -q "vfs: rootpart selector $raw_device resolved to /dev/" "$guest_log" &&
		! rg -a -F -q 'vfs: root=overlay' "$guest_log" &&
		rg -a -F -q 'init: system running' "$guest_log"
		;;
	file-swap)
		rg -a -F -q 'swap: swap0 source=boot0:swapfile slots=' "$guest_log" &&
		rg -a -q 'swap: active sources=1 total=[1-9][0-9]* free=[1-9][0-9]*' "$guest_log" &&
		rg -a -q 'BR-T46-SYSTEM-METADATA PASS bios=[0-9]+ devices=[0-9]+ partitions=[0-9]+' "$guest_log" &&
		rg -a -q 'BR-T46-SWAP-EXERCISE OBJECT-SHARED PASS free=[0-9][0-9]* page-in=[0-9][0-9]* page-out=[1-9][0-9]*' "$guest_log" &&
		rg -a -q 'BR-T46-SWAP-EXERCISE PASS bytes=[1-9][0-9]* page-in=[1-9][0-9]* page-out=[1-9][0-9]* swapped=[0-9][0-9]*' "$guest_log"
		;;
	raw-swap)
		rg -a -F -q "swap: swap0 source=$raw_device slots=" "$guest_log" &&
		rg -a -q 'swap: active sources=1 total=[1-9][0-9]* free=[1-9][0-9]*' "$guest_log" &&
		rg -a -q 'BR-T46-SWAP-EXERCISE OBJECT-SHARED PASS free=[0-9][0-9]* page-in=[0-9][0-9]* page-out=[1-9][0-9]*' "$guest_log" &&
		rg -a -q 'BR-T46-SWAP-EXERCISE PASS bytes=[1-9][0-9]* page-in=[1-9][0-9]* page-out=[1-9][0-9]* swapped=[0-9][0-9]*' "$guest_log"
		;;
	invalid)
		rg -a -q 'vfs: select root mode failed \(error 3\)' "$guest_log" &&
		rg -a -q 'VFS initialization failed \(3\); entering idle\.' "$guest_log" &&
		! rg -a -q 'vfs: root=' "$guest_log" &&
		! rg -a -q 'init: system running' "$guest_log"
		;;
	root-swap-alias)
		rg -a -q \
		    'vfs: validate rootpart swap alias failed \(error 16\)' \
		    "$guest_log" &&
		rg -a -q 'VFS initialization failed \(16\); entering idle\.' \
		    "$guest_log" &&
		! rg -a -q 'swap: active sources=' "$guest_log" &&
		! rg -a -q 'vfs: root=' "$guest_log" &&
		! rg -a -q 'init: system running' "$guest_log"
		;;
	mixed-swap)
		rg -a -F -q 'swap: swap0 source=boot0:swapfile slots=' "$guest_log" &&
		rg -a -F -q "swap: swap1 source=$raw_device slots=" "$guest_log" &&
		rg -a -q 'swap: active sources=2 total=[1-9][0-9]* free=[1-9][0-9]*' "$guest_log" &&
		rg -a -q 'BR-T46-SWAP-EXERCISE OBJECT-SHARED PASS free=[0-9][0-9]* page-in=[0-9][0-9]* page-out=[1-9][0-9]*' "$guest_log" &&
		rg -a -q 'BR-T46-SWAP-EXERCISE PASS bytes=[1-9][0-9]* page-in=[1-9][0-9]* page-out=[1-9][0-9]* swapped=[0-9][0-9]*' "$guest_log"
		;;
	cross-boot)
		rg -a -F -q "vfs: boot0 $boot0_source -> /dev/sdb2" "$guest_log" &&
		rg -a -F -q 'vfs: boot1 UUID=A1B2-C3D4 -> /dev/sda1' "$guest_log" &&
		rg -a -F -q 'vfs: root=overlay lower=boot0:rootfs.img upper=boot1:data.img' "$guest_log" &&
		rg -a -q '(^|[[:blank:]])login:[[:blank:]]*$' "$guest_log"
		;;
	partuuid-reorder)
		rg -a -F -q "vfs: boot0 $boot0_source -> /dev/sdb2" "$guest_log" &&
		rg -a -F -q 'vfs: boot1 PARTUUID=fbf09090-01 -> /dev/sda1' "$guest_log" &&
		rg -a -F -q 'vfs: root=overlay lower=boot0:rootfs.img upper=boot1:data.img' "$guest_log" &&
		rg -a -q '(^|[[:blank:]])login:[[:blank:]]*$' "$guest_log"
		;;
	esac
}

validate_no_fatal_log()
{
	local case_name=$1 logical_log=$2
	local universal_pattern positive_pattern
	universal_pattern='fatal:|FATAL:|kernel panic|panic:| fault v=|swap: swap[0-3] .* failed|swap: activate .* failed|loop[0-9]+: .*error=[1-9]'
	positive_pattern='VFS initialization failed'
	if rg -a -q "$universal_pattern" "$logical_log"; then
		rg -a -m 1 "$universal_pattern" "$logical_log" >&2
		return 1
	fi
	if [[ $case_name != invalid && $case_name != root-swap-alias ]] &&
	    rg -a -q "$positive_pattern" "$logical_log"; then
		rg -a -m 1 "$positive_pattern" "$logical_log" >&2
		return 1
	fi
	return 0
}

validator_self_test()
{
	local complete missing normalized_default
	complete=$(mktemp /tmp/ws003-br-t46-validator-complete.XXXXXX)
	missing=$(mktemp /tmp/ws003-br-t46-validator-missing.XXXXXX)
	normalized_default=$(normalized_parameter_text "$default_parameters")
	{
		printf 'boot: parameters: %s\n' "$normalized_default"
		printf '%s\n' \
		    'vfs: root=overlay lower=boot0:rootfs.img upper=boot0:data.img' \
		    'swap: swap0 source=boot0:swapfile slots=16' \
		    'swap: active sources=1 total=16 free=16' \
		    'boot: starting init /sbin/init' \
		    'login:'
	} >"$complete"
	if ! validate_log pcat default "$normalized_default" "$complete"; then
		rm -f -- "$complete" "$missing"
		echo 'BR-T46 validator self-test rejected a complete log' >&2
		return 1
	fi
	awk '!/^login:$/' "$complete" >"$missing"
	if validate_log pcat default "$normalized_default" "$missing"; then
		rm -f -- "$complete" "$missing"
		echo 'BR-T46 validator self-test accepted a missing marker' >&2
		return 1
	fi
	rm -f -- "$complete" "$missing"
	echo 'BR-T46 validator self-test: PASS'
}

run_qemu_cell()
{
	local platform=$1 case_name=$2 base_image=$3 auxiliary_image=$4
	local parameters=$5 cell_dir=$6
	local guest_log=$cell_dir/guest.log qemu_log=$cell_dir/qemu.log
	local logical_log=$cell_dir/guest-logical.log
	local controller_result=$cell_dir/controller-result.txt
	local run_image=$cell_dir/run.img base_hash final_hash memory=512
	local auxiliary_hash= auxiliary_final_hash=
	local qemu qemu_status start elapsed vars_copy= vram_snapshot=
	local command=()
	cp --reflink=auto --sparse=always "$base_image" "$run_image"
	base_hash=$(sha256sum "$base_image" | awk '{print $1}')
	if [[ -n $auxiliary_image ]]; then
		auxiliary_hash=$(sha256sum "$auxiliary_image" | awk '{print $1}')
	fi
	: >"$guest_log"
	: >"$qemu_log"
	: >"$controller_result"
	case $case_name in file-swap|raw-swap|mixed-swap) memory=128 ;; esac
	case $platform in
	pcat)
		qemu=$qemu_i386
		command=("$qemu" -machine pc -m "$memory" -smp 1
		    -drive "file=$run_image,format=raw,if=ide,index=0")
		;;
	pc98)
		qemu=$qemu_pc98
		memory=64
		vram_snapshot=$(mktemp /tmp/ws003-br-t46-pc98-vram.XXXXXX)
		rm -f -- "$vram_snapshot"
		command=("$qemu" -M pc9821,pegc=off,coregraph=on -cpu 486
		    -smp 1 -m 64M
		    -drive "if=ide,bus=0,unit=0,format=raw,file=$run_image")
		;;
	amd64-bios)
		qemu=$qemu_x86_64
		if [[ -n $auxiliary_image ]]; then
			local aux_run=$cell_dir/aux-run.img
			cp --reflink=auto --sparse=always "$auxiliary_image" "$aux_run"
			command=("$qemu" -machine pc -m "$memory" -smp 4
			    -drive "if=none,id=aux,file=$aux_run,format=raw"
			    -device "ide-hd,drive=aux,bus=ide.0,unit=0,bootindex=2"
			    -drive "if=none,id=boot,file=$run_image,format=raw"
			    -device "ide-hd,drive=boot,bus=ide.0,unit=1,bootindex=1")
		else
			command=("$qemu" -machine pc -m "$memory" -smp 4
			    -drive "file=$run_image,format=raw,if=ide,index=0")
		fi
		;;
	amd64-uefi)
		qemu=$qemu_x86_64
		vars_copy=$cell_dir/OVMF_VARS.fd
		cp "$ovmf_vars" "$vars_copy"
		command=("$qemu" -machine q35 -m "$memory" -smp 4
		    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code"
		    -drive "if=pflash,format=raw,file=$vars_copy"
		    -device qemu-xhci,id=xhci)
		if [[ -n $auxiliary_image ]]; then
			local aux_run=$cell_dir/aux-run.img
			cp --reflink=auto --sparse=always "$auxiliary_image" "$aux_run"
			# Create the auxiliary device first.  bootindex still selects the
			# production boot image, while UUID resolves boot1 independently
			# of kernel disk discovery order.
			command+=(-drive "if=none,id=aux,file=$aux_run,format=raw"
			    -device usb-storage,bus=xhci.0,drive=aux,id=auxstick,bootindex=2)
		fi
		command+=(-drive "if=none,id=boot,file=$run_image,format=raw"
		    -device usb-storage,bus=xhci.0,drive=boot,id=bootstick,bootindex=1)
		;;
	*) return 2 ;;
	esac
	command+=(-display none -serial none -debugcon "file:$guest_log"
	    -monitor stdio -no-reboot)
	{
		printf 'qemu_version=%s\n' "$(qemu_version "$qemu")"
		printf 'memory_mib=%s\n' "$memory"
		printf 'base_image=%s\nbase_sha256=%s\n' "$base_image" "$base_hash"
		if [[ -n $auxiliary_image ]]; then
			printf 'auxiliary_image=%s\nauxiliary_sha256=%s\n' \
			    "$auxiliary_image" "$auxiliary_hash"
		fi
		printf 'parameters=%s\n' "$parameters"
		printf 'command='
		printf '%q ' "${command[@]}"
		printf '\n'
	} >"$cell_dir/run-metadata.txt"
	start=$(date +%s)
	set +e
	monitor_controller "$platform" "$case_name" "$guest_log" \
	    "$controller_result" "$vram_snapshot" |
	    "${command[@]}" >"$qemu_log" 2>&1
	qemu_status=${PIPESTATUS[1]}
	set -e
	elapsed=$(( $(date +%s) - start ))
	if [[ $qemu_status -ne 0 ]]; then
		rm -f -- "$vram_snapshot"
		echo "QEMU exited with status $qemu_status" >&2
		return 1
	fi
	if [[ $(<"$controller_result") != pass ]]; then
		rm -f -- "$vram_snapshot"
		cat "$controller_result" >&2
		return 1
	fi
	# PC-98 appends flattened VRAM snapshots so wrapped positive markers can
	# be matched.  Fatal scanning must use only logical rows: otherwise an
	# unrelated later "failed" message can be joined to an earlier swap row.
	if ! awk '!/^BR-T46-VRAM-FLAT /' "$guest_log" >"$logical_log"; then
		rm -f -- "$vram_snapshot"
		echo 'failed to create logical guest log' >&2
		return 1
	fi
	if ! validate_no_fatal_log "$case_name" "$logical_log"; then
		rm -f -- "$vram_snapshot"
		return 1
	fi
	if ! validate_log "$platform" "$case_name" "$parameters" "$guest_log"; then
		rm -f -- "$vram_snapshot"
		echo "BR-T46 log validation failed: $platform/$case_name" >&2
		return 1
	fi
	if ! final_hash=$(sha256sum "$base_image" | awk '{print $1}'); then
		echo 'failed to hash base image after QEMU run' >&2
		return 1
	fi
	if [[ $final_hash != "$base_hash" ]]; then
		echo "base image mutated during QEMU run" >&2
		return 1
	fi
	if [[ -n $auxiliary_image ]]; then
		if ! auxiliary_final_hash=$(sha256sum "$auxiliary_image" | awk '{print $1}'); then
			echo 'failed to hash auxiliary image after QEMU run' >&2
			return 1
		fi
		if [[ $auxiliary_final_hash != "$auxiliary_hash" ]]; then
			echo "auxiliary base image mutated during QEMU run" >&2
			return 1
		fi
	fi
	rm -f -- "$run_image" "${aux_run:-}" "${vars_copy:-}" \
	    "$vram_snapshot"
	printf '%s\n' "$elapsed"
}

run_group()
{
	local group=$1 config build_dir
	local case_name parameters_file parameters group_cases
	local normalized_parameters_file= normalized_parameters= run_parameters
	local run_parameters_file
	local base_image auxiliary_image platform cell_dir image_hash param_hash elapsed
	local cell_status elapsed_file
	local acceptance_rootfs=
	config=$output/config/$group.mk
	# Make the generated configuration newer than artifacts left by any
	# preceding platform group.  The test makefile records it as a rootfs input.
	touch "$config"
	build_dir=$repo/build/$group
	if [[ $group == amd64 ]]; then
		group_cases=amd64_cases
	elif [[ $group == pcat ]]; then
		group_cases=pcat_cases
	else
		group_cases=common_cases
	fi
	local -n cases_ref=$group_cases
	echo "BR-T46 production build: $group"
	build_group_once "$group" "$config" \
	    "$output/build-logs/$group-production.log" "$group_cases"
	local production_hashes=$output/host/$group-production-loaders.sha256
	hash_production_loaders "$group" "$production_hashes"
	for case_name in "${cases_ref[@]}"; do
		local any_selected=0
		if [[ $group == amd64 ]]; then
			selected amd64-bios "$case_name" && any_selected=1
			selected amd64-uefi "$case_name" && any_selected=1
		else
			selected "$group" "$case_name" && any_selected=1
		fi
		((any_selected)) || continue
		parameters_file=$output/config/$group-$case_name.parameters
		parameter_text "$group" "$case_name" >"$parameters_file"
		parameters=$(<"$parameters_file")
		normalized_parameters_file=$output/config/$group-$case_name.normalized.parameters
		normalized_parameter_text "$parameters" >"$normalized_parameters_file"
		normalized_parameters=$(<"$normalized_parameters_file")
		cell_dir=$output/cells/$group-$case_name-artifacts
		mkdir -p "$cell_dir"
		local loaders=$cell_dir/loaders
		prepare_case_loaders "$group" "$parameters" "$loaders" \
		    >"$cell_dir/loader-patch.log"
		if [[ $case_name == default ]]; then
			local artifact relative
			for artifact in bootloader/bootzbsd.raw \
			    bootloader/bootzbsd.bin bootloader/BOOTZBSD.EXE; do
				relative=${artifact##*/}
				[[ $(sha256sum "$build_dir/$artifact" | awk '{print $1}') == \
				    $(sha256sum "$loaders/$relative" | awk '{print $1}') ]] || {
					echo "static default differs in $group/$artifact" >&2
					return 1
				}
			done
			if [[ $group == amd64 ]]; then
				[[ $(sha256sum "$build_dir/bootloader/stage2-chain.raw" | awk '{print $1}') == \
				    $(sha256sum "$loaders/stage2.raw" | awk '{print $1}') ]]
				[[ $(sha256sum "$build_dir/bootloader/stage2-chain.bin" | awk '{print $1}') == \
				    $(sha256sum "$loaders/stage2.bin" | awk '{print $1}') ]]
				[[ $(sha256sum "$build_dir/uefi/BOOTX64.EFI" | awk '{print $1}') == \
				    $(sha256sum "$loaders/BOOTX64.EFI" | awk '{print $1}') ]]
			fi
		fi
		base_image=$cell_dir/base.img
		auxiliary_image=
		case $case_name in
		default)
			if [[ $group == amd64 ]]; then
				build_extended_image "$group" "$case_name" \
				    "$base_image" "$loaders" \
				    >"$cell_dir/image-layout.txt"
			else
				cp --reflink=auto --sparse=always \
				    "$build_dir/hdd-image.img" "$base_image"
			fi
			;;
		file-swap|raw-swap|mixed-swap)
			if [[ -z $acceptance_rootfs ]]; then
				acceptance_rootfs=$output/cells/$group-br-t46-rootfs.img
				build_acceptance_rootfs "$group" "$acceptance_rootfs" \
				    "$output/host/$group-br-t46-rootfs"
			fi
			build_extended_image "$group" "$case_name" "$base_image" \
			    "$loaders" "$acceptance_rootfs" \
			    >"$cell_dir/image-layout.txt"
			;;
		*)
			build_extended_image "$group" "$case_name" "$base_image" \
			    "$loaders" \
			    >"$cell_dir/image-layout.txt"
			;;
		esac
		local boot_partition_lba=2048
		[[ $group == amd64 ]] && boot_partition_lba=133120
		"$image_tool" set-fat-uuid --partition-lba "$boot_partition_lba" \
		    --uuid "$uefi_fat_uuid" "$base_image" \
		    >>"$cell_dir/image-layout.txt"
		if [[ $case_name == cross-boot || \
		    $case_name == partuuid-reorder ]]; then
			auxiliary_image=$cell_dir/aux-base.img
			build_auxiliary_image "$auxiliary_image" \
			    >"$cell_dir/aux-layout.txt"
		fi
		image_hash=$(sha256sum "$base_image" | awk '{print $1}')
		if [[ $group == amd64 ]]; then
			for platform in amd64-bios amd64-uefi; do
				selected "$platform" "$case_name" || continue
				run_parameters=$normalized_parameters
				run_parameters_file=$normalized_parameters_file
				param_hash=$(sha256sum "$run_parameters_file" | \
				    awk '{print $1}')
				local run_dir=$output/cells/$platform-$case_name
				mkdir -p "$run_dir"
				echo "BR-T46 run: $platform $case_name"
				elapsed_file=$run_dir/elapsed.txt
				set +e
				(
					set -e
					run_qemu_cell "$platform" "$case_name" \
				    "$base_image" "$auxiliary_image" "$run_parameters" \
				    "$run_dir"
				) >"$elapsed_file"
				cell_status=$?
				set -e
				if [[ $cell_status -eq 0 ]]; then
					elapsed=$(<"$elapsed_file")
					printf '%s\t%s\tpass\t%s\t%s\t%s\n' \
					    "$platform" "$case_name" "$image_hash" \
					    "$param_hash" "$elapsed" >>"$output/results.tsv"
				else
					printf '%s\t%s\tfail\t%s\t%s\t-\n' \
					    "$platform" "$case_name" "$image_hash" \
					    "$param_hash" >>"$output/results.tsv"
					return 1
				fi
			done
		else
			platform=$group
			param_hash=$(sha256sum "$normalized_parameters_file" | awk '{print $1}')
			local run_dir=$output/cells/$platform-$case_name
			mkdir -p "$run_dir"
			echo "BR-T46 run: $platform $case_name"
			elapsed_file=$run_dir/elapsed.txt
			set +e
			(
				set -e
				run_qemu_cell "$platform" "$case_name" \
			    "$base_image" "$auxiliary_image" "$normalized_parameters" \
			    "$run_dir"
			) >"$elapsed_file"
			cell_status=$?
			set -e
			if [[ $cell_status -eq 0 ]]; then
				elapsed=$(<"$elapsed_file")
				printf '%s\t%s\tpass\t%s\t%s\t%s\n' "$platform" \
				    "$case_name" "$image_hash" "$param_hash" "$elapsed" \
				    >>"$output/results.tsv"
			else
				printf '%s\t%s\tfail\t%s\t%s\t-\n' "$platform" \
				    "$case_name" "$image_hash" "$param_hash" \
				    >>"$output/results.tsv"
				return 1
			fi
		fi
	done
	if ! sha256sum --check --status "$production_hashes"; then
		echo "production loader changed during BR-T46 group: $group" >&2
		return 1
	fi
}

validator_self_test

cd "$repo"
if [[ $platform_filter == all || $platform_filter == pcat ]]; then
	run_group pcat
fi
if [[ $platform_filter == all || $platform_filter == pc98 ]]; then
	run_group pc98
fi
if [[ $platform_filter == all || $platform_filter == amd64-* ]]; then
	run_group amd64
fi

config_hash_after=absent
if [[ -f $repo/config.mk ]]; then
	config_hash_after=$(sha256sum "$repo/config.mk" | awk '{print $1}')
fi
if [[ $config_hash_after != "$config_hash_before" ]]; then
	echo "config.mk changed during BR-T46" >&2
	exit 1
fi
expected=$(wc -l <"$output/cells.tsv")
passed=$(awk -F '\t' 'NR > 1 && $3 == "pass" { count++ } END { print count+0 }' \
    "$output/results.tsv")
{
	echo "finished_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "config_mk_sha256_after=$config_hash_after"
	echo "selected_cells=$expected"
	echo "passed_cells=$passed"
} >>"$output/metadata.txt"
if [[ $passed -ne $expected ]]; then
	echo "BR-T46 FAIL: passed=$passed expected=$expected" >&2
	exit 1
fi
echo "BR-T46 PASS: $passed/$expected production-loader cells"

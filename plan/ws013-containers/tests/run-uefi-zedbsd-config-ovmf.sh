#!/usr/bin/env bash
# Focused WS013 p002/p003 OVMF acceptance for zedbsd.cfg discovery.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
host_cc=${HOST_CC:-cc}
efi_cc=${EFI_CC:-x86_64-w64-mingw32-gcc}
efi_ld=${EFI_LD:-x86_64-w64-mingw32-ld}
efi_nm=${EFI_NM:-x86_64-w64-mingw32-nm}
build_dir=$repo/build/amd64
boot_timeout=${BOOT_TIMEOUT_SECONDS:-30}
fatal_settle=${FATAL_SETTLE_SECONDS:-2}
memory_mib=${MEMORY_MIB:-1024}
smp_cpus=${SMP_CPUS:-2}
fat_size=16777216
qemu_pid=

all_cases=(
	loaded-fat
	second-fat
	cross-disk-only
	duplicate-same-disk
	invalid-selected-config
	missing-configured-kernel
	load-options-ignored
)
selected_cases=()

usage()
{
	cat <<EOF
usage: $0 [OPTIONS] OUTPUT-DIRECTORY

Create disposable GPT/FAT media and run focused q35/OVMF acceptance cells.
OUTPUT-DIRECTORY must not already exist.  The normal BOOTX64.EFI and vmunix
are reused from BUILD-DIRECTORY; stale loader integration is rejected.

options:
  --case NAME       run one named case (repeatable; default: all)
  --build-dir PATH  amd64 artifact directory (default: build/amd64)
  --timeout SEC     per-case boot timeout (default: $boot_timeout)
  --list            list case names and exit
  --help            show this help and exit

environment:
  QEMU_SYSTEM_X86_64, OVMF_CODE, OVMF_VARS, HOST_CC, EFI_CC, EFI_LD,
  EFI_NM, BOOT_TIMEOUT_SECONDS, FATAL_SETTLE_SECONDS, MEMORY_MIB, SMP_CPUS
EOF
}

list_cases()
{
	cat <<'EOF'
loaded-fat                 loaded FAT contains the only configuration
second-fat                 only a second FAT on the boot disk has config/kernel
cross-disk-only            auxiliary-disk config is ignored; zero match is fatal
duplicate-same-disk        both boot-disk FATs match; loaded FAT wins with warning
invalid-selected-config    invalid first config is fatal; valid second is no fallback
missing-configured-kernel  configured missing path is fatal despite VMUNIX decoy
load-options-ignored       valid UTF-16 LoadOptions loses to exact zedbsd.cfg text
EOF
}

die()
{
	echo "WS013 OVMF acceptance: $*" >&2
	exit 2
}

is_case()
{
	local candidate=$1 known

	for known in "${all_cases[@]}"; do
		[[ $candidate == "$known" ]] && return 0
	done
	return 1
}

stop_qemu()
{
	local attempt

	[[ -n ${qemu_pid:-} ]] || return 0
	if kill -0 "$qemu_pid" 2>/dev/null; then
		kill "$qemu_pid" 2>/dev/null || true
		for ((attempt = 0; attempt < 50; attempt++)); do
			kill -0 "$qemu_pid" 2>/dev/null || break
			sleep 0.1
		done
		if kill -0 "$qemu_pid" 2>/dev/null; then
			kill -KILL "$qemu_pid" 2>/dev/null || true
		fi
	fi
	wait "$qemu_pid" 2>/dev/null || true
	qemu_pid=
}

cleanup()
{
	stop_qemu
}
trap cleanup EXIT INT TERM

list_only=0
while (($# != 0)); do
	case $1 in
	--case)
		(($# >= 2)) || die '--case requires a name'
		selected_cases+=("$2")
		shift 2
		;;
	--build-dir)
		(($# >= 2)) || die '--build-dir requires a path'
		build_dir=$2
		shift 2
		;;
	--timeout)
		(($# >= 2)) || die '--timeout requires seconds'
		boot_timeout=$2
		shift 2
		;;
	--list)
		list_only=1
		shift
		;;
	--help|-h)
		usage
		exit 0
		;;
	--)
		shift
		break
		;;
	-*) die "unknown option: $1" ;;
	*) break ;;
	esac
done

if ((list_only)); then
	(($# == 0)) || die '--list takes no output directory'
	((${#selected_cases[@]} == 0)) || die '--list cannot be combined with --case'
	list_cases
	exit 0
fi

(($# == 1)) || { usage >&2; exit 2; }
output=$1
case $boot_timeout:$fatal_settle:$memory_mib:$smp_cpus in
''|*[!0-9:]*)
	die 'timeout, FATAL_SETTLE_SECONDS, MEMORY_MIB, and SMP_CPUS must be integers'
	;;
esac
((boot_timeout > 0 && fatal_settle > 0 && memory_mib > 0 && smp_cpus > 0)) ||
	die 'timeout, FATAL_SETTLE_SECONDS, MEMORY_MIB, and SMP_CPUS must be positive'
for case_name in "${selected_cases[@]}"; do
	is_case "$case_name" || die "unknown case: $case_name"
done
if ((${#selected_cases[@]} == 0)); then
	selected_cases=("${all_cases[@]}")
fi
[[ ! -e $output ]] || die "output already exists: $output"
mkdir -p -- "$output/host"
output=$(cd -- "$output" && pwd)

bootx64=$build_dir/uefi/BOOTX64.EFI
vmunix=$build_dir/vmunix
[[ -f $bootx64 ]] || die "BOOTX64 artifact not found: $bootx64"
[[ -f $vmunix ]] || die "kernel artifact not found: $vmunix"
[[ -f $ovmf_code ]] || die "OVMF code not found: $ovmf_code"
[[ -f $ovmf_vars ]] || die "OVMF variables not found: $ovmf_vars"

commands=("$host_cc" "$qemu" cp date kill mkdir mcopy mformat mmd rg sed
	sha256sum sleep strings tail truncate)
for command_name in "${commands[@]}"; do
	command -v "$command_name" >/dev/null || die "command not found: $command_name"
done

loader_sources=(
	"$repo/bootloader/uefi/bootx64.c"
	"$repo/bootloader/uefi/include/uefi.h"
	"$repo/bootloader/uefi/elf64.c"
	"$repo/bootloader/uefi/elf64.h"
	"$repo/bootloader/uefi/framebuffer.c"
	"$repo/bootloader/uefi/framebuffer.h"
	"$repo/bootloader/uefi/memory-map.c"
	"$repo/bootloader/uefi/memory-map.h"
	"$repo/bootloader/uefi/volume-discovery.c"
	"$repo/bootloader/uefi/volume-discovery.h"
	"$repo/bootloader/uefi/zedbsd-config.c"
	"$repo/bootloader/uefi/zedbsd-config.h"
)
for source_path in "${loader_sources[@]}"; do
	[[ -f $source_path ]] || die "loader source not found: $source_path"
	if [[ $source_path -nt $bootx64 ]]; then
		die "BOOTX64 is stale relative to ${source_path#"$repo/"}; build integration first"
	fi
done
if ! strings "$bootx64" | rg -F 'A64 CFG SELECTED ' >/dev/null; then
	die 'BOOTX64 lacks zedbsd.cfg discovery diagnostics; build integration first'
fi

gpt_tool=$output/host/uefi-zedbsd-config-gpt
host_cflags=(-std=c11 -O2 -Wall -Wextra -Werror -pedantic)
"$host_cc" "${host_cflags[@]}" \
	"$script_dir/uefi-zedbsd-config-gpt.c" -o "$gpt_tool"
"$gpt_tool" --self-test >"$output/host/gpt-self-test.log"

load_options_bootx64=
build_load_options_loader()
{
	local loader_dir=$output/host/load-options-loader
	local test_loader=$loader_dir/BOOTX64.EFI
	local -a efi_cflags

	for command_name in "$efi_cc" "$efi_ld" "$efi_nm"; do
		command -v "$command_name" >/dev/null ||
			die "command not found: $command_name"
	done
	mkdir -p -- "$loader_dir"
	efi_cflags=(-std=c11 -ffreestanding -fshort-wchar -mno-red-zone
		-fno-stack-protector -fno-builtin
		-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident
		-ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror
		-I"$repo")
	"$efi_cc" "${efi_cflags[@]}" -Defi_main=zedbsd_loader_main \
		-c "$repo/bootloader/uefi/bootx64.c" -o "$loader_dir/bootx64.o"
	for source_name in elf64 framebuffer memory-map volume-discovery zedbsd-config; do
		"$efi_cc" "${efi_cflags[@]}" \
			-c "$repo/bootloader/uefi/$source_name.c" \
			-o "$loader_dir/$source_name.o"
	done
	"$efi_cc" -m64 -mno-red-zone \
		-c "$repo/bootloader/uefi/transition.S" \
		-o "$loader_dir/transition.o"
	"$efi_cc" "${efi_cflags[@]}" \
		-c "$script_dir/uefi-zedbsd-load-options-wrapper.c" \
		-o "$loader_dir/wrapper.o"
	"$efi_ld" -mi386pep --subsystem 10 --entry efi_main --image-base 0 \
		--gc-sections --enable-reloc-section --no-insert-timestamp \
		"$loader_dir/wrapper.o" "$loader_dir/bootx64.o" \
		"$loader_dir/elf64.o" "$loader_dir/framebuffer.o" \
		"$loader_dir/memory-map.o" \
		"$loader_dir/volume-discovery.o" \
		"$loader_dir/zedbsd-config.o" "$loader_dir/transition.o" \
		-o "$test_loader"
	if "$efi_nm" -u "$test_loader" | rg -v \
	    ' (__bss_start__|__bss_end__|__end__|___tls_start__|___tls_end__)$' | \
	    rg -q .; then
		"$efi_nm" -u "$test_loader" >&2
		die 'LoadOptions wrapper has unexpected undefined symbols'
	fi
	load_options_bootx64=$test_loader
}

for case_name in "${selected_cases[@]}"; do
	if [[ $case_name == load-options-ignored ]]; then
		build_load_options_loader
		break
	fi
done

write_config()
{
	local path=$1 line
	shift
	: >"$path"
	for line in "$@"; do
		printf '%s\n' "$line" >>"$path"
	done
}

create_fat()
{
	local image=$1 serial=$2 label=$3 loader=$4 config=$5 kernel=$6 log=$7

	truncate -s "$fat_size" "$image"
	mformat -i "$image" -N "$serial" -v "$label" :: >>"$log" 2>&1
	if [[ -n $loader ]]; then
		mmd -i "$image" ::/EFI ::/EFI/BOOT >>"$log" 2>&1
		mcopy -o -i "$image" "$loader" ::/EFI/BOOT/BOOTX64.EFI \
			>>"$log" 2>&1
	fi
	if [[ -n $kernel ]]; then
		mcopy -o -i "$image" "$kernel" ::/VMUNIX >>"$log" 2>&1
	fi
	if [[ -n $config ]]; then
		mcopy -o -i "$image" "$config" ::/ZEDBSD.CFG >>"$log" 2>&1
	fi
}

expected_parameters=
expected_terminal=
boot_disk=
aux_disk=
prepare_case()
{
	local case_name=$1 case_dir=$2
	local fat1=$case_dir/fat-1.img fat2=$case_dir/fat-2.img
	local aux_fat=$case_dir/aux-fat.img config1=$case_dir/config-1.cfg
	local config2=$case_dir/config-2.cfg aux_config=$case_dir/aux.cfg
	local mtools_log=$case_dir/mtools.log loader=$bootx64

	: >"$mtools_log"
	boot_disk=$case_dir/boot-disk.img
	aux_disk=
	expected_parameters=
	case $case_name in
	loaded-fat)
		write_config "$config1" kernel=vmunix \
			overlay-root=loaded.img init=/bin/sh
		create_fat "$fat1" 0x11112222 WS13LOAD "$loader" "$config1" \
			"$vmunix" "$mtools_log"
		"$gpt_tool" create "$boot_disk" "$fat1" \
			>"$case_dir/boot-layout.txt"
		expected_parameters='boot0=UUID=1111-2222 overlay-root=boot0:loaded.img init=/bin/sh'
		expected_terminal="boot: parameters: $expected_parameters"
		;;
	second-fat)
		write_config "$config2" kernel=/kernels/vmunix \
			overlay-root=second.img init=/bin/sh
		create_fat "$fat1" 0x11112222 WS13BOOT "$loader" '' '' \
			"$mtools_log"
		create_fat "$fat2" 0x33334444 WS13SECOND '' "$config2" \
			'' "$mtools_log"
		mmd -i "$fat2" ::/KERNELS >>"$mtools_log" 2>&1
		mcopy -o -i "$fat2" "$vmunix" ::/KERNELS/VMUNIX \
			>>"$mtools_log" 2>&1
		"$gpt_tool" create "$boot_disk" "$fat1" "$fat2" \
			>"$case_dir/boot-layout.txt"
		expected_parameters='boot0=UUID=3333-4444 overlay-root=boot0:second.img init=/bin/sh'
		expected_terminal="boot: parameters: $expected_parameters"
		;;
	cross-disk-only)
		write_config "$aux_config" kernel=vmunix \
			overlay-root=auxiliary-must-not-win.img init=/bin/sh
		create_fat "$fat1" 0x11112222 WS13BOOT "$loader" '' '' \
			"$mtools_log"
		create_fat "$aux_fat" 0xaabbccdd WS13AUX '' "$aux_config" \
			"$vmunix" "$mtools_log"
		"$gpt_tool" create "$boot_disk" "$fat1" \
			>"$case_dir/boot-layout.txt"
		aux_disk=$case_dir/aux-disk.img
		"$gpt_tool" create "$aux_disk" "$aux_fat" \
			>"$case_dir/aux-layout.txt"
		expected_terminal='Discover zedbsd.cfg:'
		;;
	duplicate-same-disk)
		write_config "$config1" kernel=vmunix \
			overlay-root=loaded-wins.img init=/bin/sh
		write_config "$config2" kernel=vmunix \
			overlay-root=second-must-not-win.img init=/bin/sh
		create_fat "$fat1" 0x11112222 WS13LOAD "$loader" "$config1" \
			"$vmunix" "$mtools_log"
		create_fat "$fat2" 0x33334444 WS13SECOND '' "$config2" \
			"$vmunix" "$mtools_log"
		"$gpt_tool" create "$boot_disk" "$fat1" "$fat2" \
			>"$case_dir/boot-layout.txt"
		expected_parameters='boot0=UUID=1111-2222 overlay-root=boot0:loaded-wins.img init=/bin/sh'
		expected_terminal="boot: parameters: $expected_parameters"
		;;
	invalid-selected-config)
		write_config "$config1" kernel=vmunix '[legacy-section]'
		write_config "$config2" kernel=vmunix \
			overlay-root=second-must-not-rescue.img init=/bin/sh
		create_fat "$fat1" 0x11112222 WS13BAD "$loader" "$config1" \
			"$vmunix" "$mtools_log"
		create_fat "$fat2" 0x33334444 WS13SECOND '' "$config2" \
			"$vmunix" "$mtools_log"
		"$gpt_tool" create "$boot_disk" "$fat1" "$fat2" \
			>"$case_dir/boot-layout.txt"
		expected_terminal='Load zedbsd.cfg:'
		;;
	missing-configured-kernel)
		write_config "$config1" kernel=missing.elf init=/bin/sh
		create_fat "$fat1" 0x11112222 WS13MISS "$loader" "$config1" \
			"$vmunix" "$mtools_log"
		"$gpt_tool" create "$boot_disk" "$fat1" \
			>"$case_dir/boot-layout.txt"
		expected_parameters='boot0=UUID=1111-2222 init=/bin/sh'
		expected_terminal='Open configured kernel:'
		;;
	load-options-ignored)
		loader=$load_options_bootx64
		write_config "$config1" kernel=vmunix boot0=UUID=CAFE-BABE \
			overlay-root=config-wins.img init=/bin/sh
		create_fat "$fat1" 0x11112222 WS13OPT "$loader" "$config1" \
			"$vmunix" "$mtools_log"
		"$gpt_tool" create "$boot_disk" "$fat1" \
			>"$case_dir/boot-layout.txt"
		expected_parameters='boot0=UUID=CAFE-BABE overlay-root=boot0:config-wins.img init=/bin/sh'
		expected_terminal="boot: parameters: $expected_parameters"
		;;
	*) die "internal unknown case: $case_name" ;;
	esac
	{
		printf 'case=%s\n' "$case_name"
		printf 'boot_disk=%s\n' "$boot_disk"
		printf 'aux_disk=%s\n' "${aux_disk:-none}"
		printf 'expected_terminal=%s\n' "$expected_terminal"
		printf 'expected_parameters=%s\n' "${expected_parameters:-none}"
		sha256sum "$boot_disk"
		if [[ -n $aux_disk ]]; then
			sha256sum "$aux_disk"
		fi
	} >"$case_dir/media-metadata.txt"
}

wait_for_fixed()
{
	local marker=$1 log=$2 timeout=$3
	local start now

	start=$(date +%s)
	while :; do
		if rg -a -F -q -- "$marker" "$log"; then
			return 0
		fi
		if ! kill -0 "$qemu_pid" 2>/dev/null; then
			return 2
		fi
		now=$(date +%s)
		if ((now >= start + timeout)); then
			return 1
		fi
		sleep 0.1
	done
}

run_qemu()
{
	local case_name=$1 case_dir=$2
	local vars_copy=$case_dir/OVMF_VARS.fd
	local debug_log=$case_dir/debugcon.log
	local serial_log=$case_dir/serial.log
	local qemu_log=$case_dir/qemu.log
	local command_log=$case_dir/qemu-command.txt
	local boot_index=1
	local settle_start settle_now
	local -a qemu_args

	# Let firmware try the non-bootable auxiliary FAT first so its SimpleFS
	# remains connected when BOOTX64 starts from the real boot disk.
	if [[ -n $aux_disk ]]; then
		boot_index=2
	fi
	cp -- "$ovmf_vars" "$vars_copy"
	: >"$debug_log"
	: >"$serial_log"
	: >"$qemu_log"
	qemu_args=(
		-machine q35
		-accel tcg,thread=multi
		-m "$memory_mib"
		-smp "$smp_cpus"
		-drive "if=pflash,format=raw,readonly=on,file=$ovmf_code"
		-drive "if=pflash,format=raw,file=$vars_copy"
		-device qemu-xhci,id=xhci
		-drive "if=none,id=bootdisk,file=$boot_disk,format=raw,readonly=on"
		-device "usb-storage,bus=xhci.0,port=1,drive=bootdisk,id=bootstick,bootindex=$boot_index"
		-display none
		-monitor none
		-serial "file:$serial_log"
		-debugcon "file:$debug_log"
		-net none
		-no-reboot
		-no-shutdown
	)
	if [[ -n $aux_disk ]]; then
		qemu_args+=(
			-drive "if=none,id=auxdisk,file=$aux_disk,format=raw,readonly=on"
			-device usb-storage,bus=xhci.0,port=2,drive=auxdisk,id=auxstick,bootindex=1
		)
	fi
	{
		printf '%q ' "$qemu" "${qemu_args[@]}"
		printf '\n'
	} >"$command_log"
	"$qemu" "${qemu_args[@]}" >"$qemu_log" 2>&1 &
	qemu_pid=$!
	local wait_status
	if wait_for_fixed "$expected_terminal" "$debug_log" "$boot_timeout"; then
		wait_status=0
	else
		wait_status=$?
		stop_qemu
		echo "WS013 OVMF FAIL: $case_name did not reach: $expected_terminal" >&2
		echo "wait status: $wait_status" >&2
		tail -n 100 "$debug_log" >&2 || true
		tail -n 40 "$serial_log" >&2 || true
		tail -n 40 "$qemu_log" >&2 || true
		return 1
	fi
	case $case_name in
	cross-disk-only|invalid-selected-config|missing-configured-kernel)
		settle_start=$(date +%s)
		while kill -0 "$qemu_pid" 2>/dev/null; do
			settle_now=$(date +%s)
			((settle_now < settle_start + fatal_settle)) || break
			sleep 0.1
		done
		;;
	esac
	stop_qemu
}

require_fixed()
{
	local marker=$1 log=$2

	if ! rg -a -F -q -- "$marker" "$log"; then
		echo "missing marker: $marker" >&2
		return 1
	fi
}

reject_fixed()
{
	local marker=$1 log=$2

	if rg -a -F -q -- "$marker" "$log"; then
		echo "unexpected marker: $marker" >&2
		return 1
	fi
}

validate_case()
{
	local case_name=$1 case_dir=$2 log=$case_dir/debugcon.log

	require_fixed 'A64 UEFI ENTRY' "$log" || return 1
	case $case_name in
	loaded-fat)
		require_fixed 'A64 CFG MATCH 0x0000000000000000' "$log" &&
		require_fixed 'A64 CFG SELECTED 0x0000000000000000' "$log" &&
		require_fixed 'A64 CFG UUID 1111-2222' "$log" &&
		require_fixed 'A64 KERNEL vmunix' "$log" &&
		require_fixed "A64 PARAMS $expected_parameters" "$log" &&
		require_fixed "boot: parameters: $expected_parameters" "$log" &&
		reject_fixed 'WARNING: multiple zedbsd.cfg' "$log"
		;;
	second-fat)
		require_fixed 'A64 CFG MISSING 0x0000000000000000' "$log" &&
		require_fixed 'A64 CFG MATCH 0x0000000000000001' "$log" &&
		require_fixed 'A64 CFG SELECTED 0x0000000000000001' "$log" &&
		require_fixed 'A64 CFG UUID 3333-4444' "$log" &&
		require_fixed 'A64 KERNEL kernels/vmunix' "$log" &&
		require_fixed "A64 PARAMS $expected_parameters" "$log" &&
		require_fixed "boot: parameters: $expected_parameters" "$log"
		;;
	cross-disk-only)
		require_fixed 'A64 CFG MISSING 0x0000000000000000' "$log" &&
		require_fixed 'A64 CFG OTHER DISK ' "$log" &&
		require_fixed 'zedbsd.cfg was not found on the boot disk' "$log" &&
		require_fixed 'Discover zedbsd.cfg:' "$log" &&
		reject_fixed 'A64 CFG SELECTED ' "$log" &&
		reject_fixed 'A64 KERNEL ' "$log" &&
		reject_fixed 'boot: parameters:' "$log" &&
		reject_fixed 'A64 UEFI ELF' "$log"
		;;
	duplicate-same-disk)
		require_fixed 'A64 CFG MATCH 0x0000000000000000' "$log" &&
		require_fixed 'A64 CFG MATCH 0x0000000000000001' "$log" &&
		require_fixed 'WARNING: multiple zedbsd.cfg files on the boot disk; using the first' "$log" &&
		require_fixed 'A64 CFG MATCHES 0x0000000000000002' "$log" &&
		require_fixed 'A64 CFG SELECTED 0x0000000000000000' "$log" &&
		require_fixed 'A64 CFG UUID 1111-2222' "$log" &&
		require_fixed "A64 PARAMS $expected_parameters" "$log" &&
		require_fixed "boot: parameters: $expected_parameters" "$log" &&
		reject_fixed 'A64 PARAMS boot0=UUID=3333-4444' "$log" &&
		reject_fixed 'second-must-not-win.img' "$log"
		;;
	invalid-selected-config)
		require_fixed 'WARNING: multiple zedbsd.cfg files on the boot disk; using the first' "$log" &&
		require_fixed 'A64 CFG SELECTED 0x0000000000000000' "$log" &&
		require_fixed 'zedbsd.cfg rejected: malformed-line' "$log" &&
		require_fixed 'Load zedbsd.cfg:' "$log" &&
		reject_fixed 'A64 KERNEL ' "$log" &&
		reject_fixed 'second-must-not-rescue.img' "$log" &&
		reject_fixed 'boot: parameters:' "$log" &&
		reject_fixed 'A64 UEFI ELF' "$log"
		;;
	missing-configured-kernel)
		require_fixed 'A64 CFG SELECTED 0x0000000000000000' "$log" &&
		require_fixed 'A64 KERNEL missing.elf' "$log" &&
		require_fixed "A64 PARAMS $expected_parameters" "$log" &&
		require_fixed 'Open configured kernel:' "$log" &&
		reject_fixed 'A64 UEFI ELF' "$log" &&
		reject_fixed 'boot: parameters:' "$log"
		;;
	load-options-ignored)
		require_fixed 'WS013 LoadOptions injected: boot0=UUID=DEAD-BEEF init=/bin/false' "$log" &&
		require_fixed 'A64 UEFI LoadOptions ignored' "$log" &&
		require_fixed 'A64 CFG SELECTED 0x0000000000000000' "$log" &&
		require_fixed "A64 PARAMS $expected_parameters" "$log" &&
		require_fixed "boot: parameters: $expected_parameters" "$log" &&
		reject_fixed 'A64 PARAMS boot0=UUID=DEAD-BEEF' "$log" &&
		reject_fixed 'boot: parameters: boot0=UUID=DEAD-BEEF' "$log"
		;;
	*) return 1 ;;
	esac
}

{
	printf 'qemu=%s\n' "$($qemu --version | sed -n '1p')"
	printf 'ovmf_code=%s\n' "$ovmf_code"
	printf 'ovmf_code_sha256='
	sha256sum "$ovmf_code" | { read -r digest _; printf '%s\n' "$digest"; }
	printf 'ovmf_vars=%s\n' "$ovmf_vars"
	printf 'ovmf_vars_sha256='
	sha256sum "$ovmf_vars" | { read -r digest _; printf '%s\n' "$digest"; }
	printf 'bootx64=%s\n' "$bootx64"
	printf 'bootx64_sha256='
	sha256sum "$bootx64" | { read -r digest _; printf '%s\n' "$digest"; }
	printf 'vmunix=%s\n' "$vmunix"
	printf 'vmunix_sha256='
	sha256sum "$vmunix" | { read -r digest _; printf '%s\n' "$digest"; }
	printf 'memory_mib=%s\n' "$memory_mib"
	printf 'smp_cpus=%s\n' "$smp_cpus"
	printf 'boot_timeout_seconds=%s\n' "$boot_timeout"
	printf 'fatal_settle_seconds=%s\n' "$fatal_settle"
} >"$output/run-metadata.txt"

pass_count=0
for case_name in "${selected_cases[@]}"; do
	case_dir=$output/$case_name
	mkdir -p -- "$case_dir"
	prepare_case "$case_name" "$case_dir"
	echo "WS013 OVMF RUN: $case_name"
	run_qemu "$case_name" "$case_dir"
	if ! validate_case "$case_name" "$case_dir"; then
		echo "WS013 OVMF FAIL: $case_name validation" >&2
		tail -n 120 "$case_dir/debugcon.log" >&2 || true
		exit 1
	fi
	printf 'PASS %s\n' "$case_name" >"$case_dir/result.txt"
	echo "WS013 OVMF PASS: $case_name"
	((pass_count += 1))
done

printf 'PASS %d/%d\n' "$pass_count" "${#selected_cases[@]}" \
	>"$output/result.txt"
echo "WS013 OVMF acceptance: PASS $pass_count/${#selected_cases[@]}"

#!/usr/bin/env bash
# SWAP-T011/T012 production runtime-swap QEMU acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
br_t46_dir=$repo/plan/ws003-bringup/tests
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-3600}
runtime_case=${RUNTIME_CASE:-all}
run_br_t46=${RUN_BR_T46:-1}
output=

usage()
{
	cat <<EOF
usage: $0 OUTPUT

Builds disposable amd64 UEFI USB images and runs SWAP-T011/T012.  By default it
also re-runs the representative BR-T46 file/raw/mixed boot-swap cells.

Debug-only environment controls:
  RUNTIME_CASE=all|file|mixed|native
  RUN_BR_T46=0|1
  BOOT_TIMEOUT_SECONDS=N
  COMMAND_TIMEOUT_SECONDS=N
EOF
}

if [[ $# -eq 1 && ($1 == -h || $1 == --help) ]]; then
	usage
	exit 0
elif [[ $# -ne 1 ]]; then
	usage >&2
	exit 2
fi
output=$(realpath -m -- "$1")
case $runtime_case in all|file|mixed|native) ;; *) echo "invalid RUNTIME_CASE: $runtime_case" >&2; exit 2 ;; esac
case $run_br_t46 in 0|1) ;; *) echo "RUN_BR_T46 must be 0 or 1" >&2; exit 2 ;; esac
case $boot_timeout:$command_timeout in
*[!0-9:]*|0:*|*:0)
	echo "timeouts must be positive integers" >&2
	exit 2
	;;
esac
runtime_host_timeout=$((boot_timeout + command_timeout + 30))
br_host_timeout=$((boot_timeout + command_timeout + 300))
if [[ -e $output && -n $(find "$output" -mindepth 1 -maxdepth 1 -print -quit) ]]; then
	echo "output directory is not empty: $output" >&2
	exit 2
fi
mkdir -p -- "$output/host" "$output/config" "$output/build-logs" \
	"$output/images" "$output/cells" "$output/br-t46"

for command in awk cat cc chmod cp date find git make mkdir mktemp mcopy \
	realpath rg rm sed sha256sum sleep stat tail tar tee timeout touch \
	truncate wc; do
	command -v "$command" >/dev/null || {
		echo "required host command not found: $command" >&2
		exit 2
	}
done
[[ -x $qemu ]] || command -v "$qemu" >/dev/null || {
	echo "QEMU binary not found: $qemu" >&2
	exit 2
}
[[ -f $ovmf_code ]] || { echo "OVMF code not found: $ovmf_code" >&2; exit 2; }
[[ -f $ovmf_vars ]] || { echo "OVMF vars not found: $ovmf_vars" >&2; exit 2; }
noct=$repo/build/NoctLang/build-static/noct
[[ -x $noct ]] || {
	echo "Noct toolchain missing; run make toolchain first" >&2
	exit 2
}

image_tool=$output/host/boot-parameter-image-tool
cc -std=c11 -Wall -Wextra -Werror \
	"$br_t46_dir/boot-parameter-image-tool.c" -o "$image_tool"
"$image_tool" self-test >"$output/host/image-tool-self-test.log"

config_hash_before=absent
if [[ -f $repo/config.mk ]]; then
	config_hash_before=$(sha256sum "$repo/config.mk" | awk '{print $1}')
fi
ovmf_code_hash_before=$(sha256sum "$ovmf_code" | awk '{print $1}')
ovmf_vars_hash_before=$(sha256sum "$ovmf_vars" | awk '{print $1}')
invocation_file=$output/invocation.txt
{
	printf 'command='
	printf '%q ' "$0" "$@"
	printf '\n'
	printf 'RUNTIME_CASE=%q\nRUN_BR_T46=%q\n' "$runtime_case" "$run_br_t46"
	printf 'BOOT_TIMEOUT_SECONDS=%q\nCOMMAND_TIMEOUT_SECONDS=%q\n' \
	    "$boot_timeout" "$command_timeout"
} >"$invocation_file"
{
	echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "repository=$repo"
	echo "repository_head=$(git -C "$repo" rev-parse HEAD)"
	echo "config_mk_sha256_before=$config_hash_before"
	echo "qemu=$qemu"
	echo "qemu_version=$($qemu --version | sed -n '1p')"
	echo "ovmf_code=$ovmf_code"
	echo "ovmf_code_sha256_before=$ovmf_code_hash_before"
	echo "ovmf_vars=$ovmf_vars"
	echo "ovmf_vars_sha256_before=$ovmf_vars_hash_before"
	echo "make_parallelism=16"
	echo "boot_timeout_seconds=$boot_timeout"
	echo "command_timeout_seconds=$command_timeout"
	echo "runtime_host_timeout_seconds=$runtime_host_timeout"
	echo "br_host_timeout_seconds=$br_host_timeout"
} >"$output/metadata.txt"
printf 'suite\tcell\tstatus\timage_sha256\tparameters_sha256\telapsed_seconds\tguest_log_sha256\n' \
	>"$output/results.tsv"

config=$output/config/amd64.mk
cat >"$config" <<'EOF'
# Generated only for SWAP-T011/T012; config.mk is not modified.
ZEDBSD_MENU_VERSION := 2
ZEDBSD_PLATFORM := amd64
ZEDBSD_ARCHITECTURE := amd64
ZEDBSD_BOARD := pcat

CONFIG_KERNEL_TEST_CHECKPOINTS := n
CONFIG_BUF_CACHE_KIB := 0
CONFIG_DRIVER_NE2000 := y
CONFIG_DRIVER_PCI_UHCI := y
CONFIG_DRIVER_PCI_EHCI := y
CONFIG_DRIVER_PCI_XHCI := y
CONFIG_DRIVER_USB_STORAGE := y
CONFIG_DRIVER_GRAPHICS := y
CONFIG_DRIVER_LGY98 := n

ZEDBSD_USER_PROGRAMS := swapon swapoff
EOF

parameters_for()
{
	case $1 in
	file)
		printf '%s\n' \
		    'overlay-root=boot0:rootfs.img overlay-data=boot0:data.img init=/bin/ws016-swap'
		;;
	mixed)
		printf '%s\n' \
		    'overlay-root=boot0:rootfs.img overlay-data=boot0:data.img swap0=boot0:swapfile init=/bin/ws016-swap'
		;;
	native)
		printf '%s\n' \
		    'rootpart=/dev/sda3 swap0=boot0:swapfile init=/bin/ws016-swap'
		;;
	*) return 2 ;;
	esac
}

make_case()
{
	local case_name=$1 parameters=$output/config/$case_name.parameters
	local build_log=$output/build-logs/$case_name.log
	parameters_for "$case_name" >"$parameters"
	if ! make -C "$repo" -j16 -f Makefile \
	    -f "$br_t46_dir/boot-parameter-acceptance.mk" \
	    -f "$script_dir/runtime-swap-acceptance.mk" \
	    ZEDBSD_CONFIG="$config" ARCH_IMAGE_DIR=build/amd64/arch-images \
	    ZEDBSD_BOOT_PARAMETERS_FILE="$parameters" disk-image \
	    build/amd64/rootfs.tar.gz build/amd64/WS016-SWAP.ELF \
	    >"$build_log" 2>&1; then
		tail -n 100 "$build_log" >&2
		return 1
	fi
}

rootfs_staging=$output/host/rootfs
overlay_rootfs=$output/host/rootfs-overlay.img
native_rootfs=$output/host/rootfs-native.img
file_swap=$output/host/runtime-file.swap
boot_swap=$output/host/boot-file.swap
raw_swap=$output/host/runtime-raw.swap
bad_swap=$output/host/badswap

build_rootfs_fixtures()
{
	rm -rf -- "$rootfs_staging"
	mkdir -p -- "$rootfs_staging"
	tar -xzf "$repo/build/amd64/rootfs.tar.gz" -C "$rootfs_staging"
	cp "$repo/build/amd64/WS016-SWAP.ELF" \
	    "$rootfs_staging/bin/ws016-swap"
	chmod 0755 "$rootfs_staging/bin/ws016-swap"
	"$noct" --path="$repo/tools/build" \
	    "$repo/tools/build/make-swapfile.noct" --size-mib 16 \
	    --format v2 --uuid 0102030405060708 --label RUNTIMEFILE \
	    --output "$file_swap"
	"$noct" --path="$repo/tools/build" \
	    "$repo/tools/build/make-swapfile.noct" --size-mib 2 \
	    --format v2 --uuid 1112131415161718 --label BOOTFILE \
	    --output "$boot_swap"
	"$noct" --path="$repo/tools/build" \
	    "$repo/tools/build/make-swapfile.noct" --size-mib 64 \
	    --format v2 --uuid 2122232425262728 --label RAWSOURCE \
	    --output "$raw_swap"
	truncate -s 1048576 "$bad_swap"
	"$repo/build/zedimage-host" ufs 33554432 "$rootfs_staging" \
	    "$overlay_rootfs"
	touch "$rootfs_staging/etc/ws016-native"
	cp "$file_swap" "$rootfs_staging/unsupported.swap"
	"$repo/build/zedimage-host" ufs 33554432 "$rootfs_staging" \
	    "$native_rootfs"
}

build_image()
{
	local case_name=$1 destination=$2 build_dir=$repo/build/amd64
	local command=("$repo/build/zedimage-host" disk --machine pcat --gpt
	    --size-mib 201 --fat-size-mib 128
	    --stage1 "$build_dir/bootloader/stage1.bin"
	    --stage2 "$build_dir/bootloader/stage2.bin"
	    --partition-pbr "$build_dir/bootloader/partition-pbr.bin"
	    --bootzbsd "$build_dir/bootloader/BOOTZBSD.EXE"
	    --kernel "$build_dir/vmunix"
	    --bootx64 "$build_dir/uefi/BOOTX64.EFI")
	case $case_name in
	file)
		command+=(--arch-image "$overlay_rootfs"
		    --data-image "$repo/build/data.img"
		    --swapfile "$file_swap")
		;;
	mixed)
		command+=(--arch-image "$overlay_rootfs"
		    --data-image "$repo/build/data.img"
		    --swapfile "$boot_swap")
		;;
	native)
		command+=(--swapfile "$file_swap")
		;;
	*) return 2 ;;
	esac
	command+=("$destination")
	"${command[@]}"
	case $case_name in
	file)
		mcopy -o -i "$destination@@1048576" "$bad_swap" ::BADSWAP
		;;
	mixed)
		"$image_tool" add-partition --machine pcat --kind swap \
		    --index 3 --start-lba 264192 --payload "$raw_swap" \
		    "$destination"
		;;
	native)
		"$image_tool" add-partition --machine pcat --kind ufs \
		    --index 3 --start-lba 264192 --payload "$native_rootfs" \
		    "$destination"
		;;
	esac
}

marker_count()
{
	local pattern=$1 file=$2 result
	result=$(rg -a -c -- "$pattern" "$file" 2>/dev/null || true)
	printf '%s\n' "${result:-0}"
}

wait_for_marker()
{
	local pattern=$1 file=$2 timeout=$3 deadline
	deadline=$(( $(date +%s) + timeout ))
	while :; do
		if (( $(marker_count "$pattern" "$file") > 0 )); then
			return 0
		fi
		if (( $(date +%s) >= deadline )); then
			return 1
		fi
		sleep 0.1
	done
}

monitor_controller()
{
	local guest_log=$1 result=$2
	exec 3>>"${result%.txt}-controller.log"
	if ! wait_for_marker 'WS016-SWAP START' "$guest_log" \
	    "$boot_timeout"; then
		echo 'fail: guest start timeout' >"$result"
	elif wait_for_marker 'WS016-SWAP (PASS|FAIL)' "$guest_log" \
	    "$command_timeout"; then
		if rg -a -q 'WS016-SWAP FAIL' "$guest_log"; then
			echo 'fail: guest reported failure' >"$result"
		else
			echo pass >"$result"
		fi
	else
		echo 'fail: guest completion timeout' >"$result"
	fi
	printf 'quit\n'
}

validate_no_fatal_log()
{
	local guest_log=$1
	local pattern='fatal:|FATAL:|kernel panic|panic:| fault v=|loop[0-9]+: .*error=[1-9]|usb-storage: .*error=[1-9]|VFS initialization failed'
	if rg -a -q "$pattern" "$guest_log"; then
		rg -a -m 1 "$pattern" "$guest_log" >&2
		return 1
	fi
}

validate_case_log()
{
	local case_name=$1 guest_log=$2 parameters=$3
	rg -a -F -q -- "boot: parameters: $parameters" "$guest_log" &&
	rg -a -F -q -- 'boot: starting init /bin/ws016-swap' "$guest_log" &&
	rg -a -F -q -- 'WS016-SWAP START' "$guest_log" || return 1
	case $case_name in
	file)
		rg -a -q 'WS016-SWAP FILE ADD PASS id=0 pages=[1-9][0-9]* label=RUNTIMEFILE' "$guest_log" || return 1
		for negative in duplicate-alias malformed-header unknown-removal \
		    nonroot-control unsafe-commit-removal; do
			rg -a -F -q -- "WS016-SWAP NEGATIVE PASS case=$negative" \
			    "$guest_log" || return 1
		done
		rg -a -q 'WS016-SWAP PRESSURE READBACK PASS generation=[0-9][0-9]* pages=[1-9][0-9]* page-in=[1-9][0-9]* page-out=[1-9][0-9]*' "$guest_log" &&
		rg -a -F -q -- 'WS016-SWAP FILE REUSE PASS id=0' "$guest_log" &&
		rg -a -F -q -- 'WS016-SWAP PASS scenario=file' "$guest_log"
		;;
	mixed)
		rg -a -q 'WS016-SWAP MIXED ADD PASS id0=boot0:swapfile pages0=[1-9][0-9]* id1=/dev/sda3 pages1=[1-9][0-9]* label1=RAWSOURCE' "$guest_log" &&
		rg -a -q 'WS016-SWAP PRESSURE READY generation=[0-9][0-9]* pages=[1-9][0-9]* used0=[1-9][0-9]* total0=[1-9][0-9]* used1=[1-9][0-9]* total1=[1-9][0-9]*' "$guest_log" &&
		rg -a -q 'WS016-SWAP NUMERIC ORDER PASS full0=[1-9][0-9]* used1=[1-9][0-9][0-9][0-9][0-9]*' "$guest_log" &&
		rg -a -q 'WS016-SWAP NUMERIC FIRST PASS full0=[1-9][0-9]* first-used1=[1-9][0-9]*' "$guest_log" &&
		rg -a -q 'WS016-SWAP PRESSURE PREFIX READBACK PASS generation=17 pages=[1-9][0-9]*' "$guest_log" &&
		rg -a -q 'WS016-SWAP PRESSURE HEADROOM PASS generation=17 prefix=[1-9][0-9]* retained=[1-9][0-9]* free=[1-9][0-9]* used0=[1-9][0-9]* used1=[1-9][0-9]*' "$guest_log" &&
		rg -a -q 'WS016-SWAP COMMAND PRECHECK PASS used0=[1-9][0-9]* minimum=64' "$guest_log" &&
		rg -a -F -q -- 'WS016-SWAP COMMAND path=/sbin/swapoff source=boot0:swapfile uid=root status=0' "$guest_log" &&
		rg -a -F -q -- 'WS016-SWAP COMMAND DRAIN PASS removed=0 preserved=1' "$guest_log" &&
		rg -a -F -q -- 'WS016-SWAP REMOVED-SKIP PASS removed=0 active=1' "$guest_log" &&
		rg -a -q 'WS016-SWAP PRESSURE READBACK PASS generation=17 pages=[1-9][0-9]* page-in=[1-9][0-9]* page-out=[1-9][0-9]*' "$guest_log" &&
		rg -a -F -q -- 'WS016-SWAP REUSE PASS id=0 old-generation=17 new-generation=51 stale-token-errors=0' "$guest_log" &&
		rg -a -F -q -- 'WS016-SWAP PASS scenario=mixed' "$guest_log"
		;;
	native)
		rg -a -F -q -- \
		    'WS016-SWAP NEGATIVE PASS case=unsupported-backend' "$guest_log" &&
		rg -a -F -q -- \
		    'WS016-SWAP NEGATIVE PASS case=root-raw-overlap' "$guest_log" &&
		rg -a -F -q -- \
		    'WS016-SWAP PASS scenario=native-root-negative' "$guest_log"
		;;
	esac
}

run_qemu_cell()
{
	local case_name=$1 base_image=$2 parameters=$3 cell_dir=$4
	local guest_log=$cell_dir/guest.log qemu_log=$cell_dir/qemu.log
	local controller_result=$cell_dir/controller-result.txt
	local run_image=$cell_dir/run.img vars_copy=$cell_dir/OVMF_VARS.fd
	local base_hash final_hash
	local start elapsed qemu_status
	local command=(timeout --kill-after=10 "$runtime_host_timeout"
	    "$qemu" -machine q35 -m 128 -smp 4
	    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code"
	    -drive "if=pflash,format=raw,file=$vars_copy"
	    -device qemu-xhci,id=xhci
	    -drive "if=none,id=boot,file=$run_image,format=raw"
	    -device usb-storage,bus=xhci.0,drive=boot,id=bootstick,bootindex=1
	    -display none -serial none -debugcon "file:$guest_log"
	    -monitor stdio -no-reboot)
	mkdir -p -- "$cell_dir"
	cp --reflink=auto --sparse=always "$base_image" "$run_image"
	cp "$ovmf_vars" "$vars_copy"
	base_hash=$(sha256sum "$base_image" | awk '{print $1}')
	: >"$guest_log"
	: >"$qemu_log"
	: >"$controller_result"
	{
		printf 'qemu_version=%s\n' "$($qemu --version | sed -n '1p')"
		printf 'memory_mib=128\nbase_image=%s\nbase_sha256=%s\n' \
		    "$base_image" "$base_hash"
		printf 'ovmf_code=%s\novmf_code_sha256=%s\n' \
		    "$ovmf_code" "$ovmf_code_hash_before"
		printf 'ovmf_vars_source=%s\novmf_vars_source_sha256=%s\n' \
		    "$ovmf_vars" "$ovmf_vars_hash_before"
		printf 'parameters=%s\n' "$parameters"
		printf 'host_timeout_seconds=%s\n' "$runtime_host_timeout"
		printf 'command='
		printf '%q ' "${command[@]}"
		printf '\n'
	} >"$cell_dir/run-metadata.txt"
	start=$(date +%s)
	set +e
	monitor_controller "$guest_log" "$controller_result" |
	    "${command[@]}" >"$qemu_log" 2>&1
	qemu_status=${PIPESTATUS[1]}
	set -e
	elapsed=$(( $(date +%s) - start ))
	if [[ $qemu_status -ne 0 || $(<"$controller_result") != pass ]]; then
		tail -n 100 "$guest_log" >&2
		cat "$controller_result" >&2
		return 1
	fi
	validate_no_fatal_log "$guest_log" || return 1
	validate_case_log "$case_name" "$guest_log" "$parameters" || {
		echo "runtime swap log validation failed: $case_name" >&2
		return 1
	}
	final_hash=$(sha256sum "$base_image" | awk '{print $1}')
	if [[ $final_hash != "$base_hash" ]]; then
		echo "base image mutated during QEMU run: $case_name" >&2
		return 1
	fi
	rm -f -- "$run_image" "$vars_copy"
	printf '%s\n' "$elapsed"
}

selected_runtime_case()
{
	[[ $runtime_case == all || $runtime_case == "$1" ]]
}

runtime_status=0
fixtures_ready=0
for case_name in file mixed native; do
	selected_runtime_case "$case_name" || continue
	echo "SWAP-T011/T012 build: $case_name"
	make_case "$case_name"
	if [[ $fixtures_ready -eq 0 ]]; then
		build_rootfs_fixtures >"$output/build-logs/fixtures.log" 2>&1
		fixtures_ready=1
	fi
	base_image=$output/images/$case_name-base.img
	build_image "$case_name" "$base_image" \
	    >"$output/cells/$case_name-image-layout.txt"
	parameters=$(parameters_for "$case_name")
	image_hash=$(sha256sum "$base_image" | awk '{print $1}')
	parameter_hash=$(sha256sum "$output/config/$case_name.parameters" | awk '{print $1}')
	echo "SWAP-T011/T012 run: $case_name"
	set +e
	elapsed=$(run_qemu_cell "$case_name" "$base_image" "$parameters" \
	    "$output/cells/$case_name")
	cell_status=$?
	set -e
	if [[ $cell_status -eq 0 ]]; then
		guest_hash=$(sha256sum "$output/cells/$case_name/guest.log" | \
		    awk '{print $1}')
		printf 'SWAP-T011/T012\t%s\tpass\t%s\t%s\t%s\t%s\n' \
		    "$case_name" "$image_hash" "$parameter_hash" "$elapsed" \
		    "$guest_hash" \
		    >>"$output/results.tsv"
	else
		guest_hash=-
		if [[ -f $output/cells/$case_name/guest.log ]]; then
			guest_hash=$(sha256sum "$output/cells/$case_name/guest.log" | \
			    awk '{print $1}')
		fi
		printf 'SWAP-T011/T012\t%s\tfail\t%s\t%s\t-\t%s\n' \
		    "$case_name" "$image_hash" "$parameter_hash" "$guest_hash" \
		    >>"$output/results.tsv"
		runtime_status=1
	fi
done

br_status=0
if [[ $run_br_t46 -eq 1 ]]; then
	platform=amd64-uefi
	for case_name in file-swap raw-swap mixed-swap; do
		br_output=$output/br-t46/$platform-$case_name
		echo "BR-T46 representative run: $platform $case_name"
		set +e
		BOOT_TIMEOUT_SECONDS=$boot_timeout \
		COMMAND_TIMEOUT_SECONDS=$command_timeout \
		timeout --kill-after=10 "$br_host_timeout" \
		    "$br_t46_dir/boot-parameter-qemu-acceptance.sh" \
		    --platform "$platform" --case "$case_name" "$br_output" \
		    >"$output/br-t46-$platform-$case_name-driver.log" 2>&1
		cell_status=$?
		set -e
		if [[ -f $br_output/results.tsv ]]; then
			br_guest=$br_output/cells/$platform-$case_name/guest.log
			br_guest_hash=-
			if [[ -f $br_guest ]]; then
				br_guest_hash=$(sha256sum "$br_guest" | awk '{print $1}')
			fi
			awk -F '\t' -v OFS='\t' \
			    -v cell="$platform/$case_name" \
			    -v hash="$br_guest_hash" \
			    'NR == 2 { print "BR-T46", cell, $3, $4, $5, $6, hash }' \
			    "$br_output/results.tsv" >>"$output/results.tsv"
		else
			printf 'BR-T46\t%s/%s\tfail\t-\t-\t-\t-\n' \
			    "$platform" "$case_name" \
			    >>"$output/results.tsv"
		fi
		if [[ $cell_status -ne 0 ]]; then
			tail -n 100 \
			    "$output/br-t46-$platform-$case_name-driver.log" >&2
			br_status=1
		fi
		done
fi

config_hash_after=absent
if [[ -f $repo/config.mk ]]; then
	config_hash_after=$(sha256sum "$repo/config.mk" | awk '{print $1}')
fi
if [[ $config_hash_after != "$config_hash_before" ]]; then
	echo "config.mk changed during SWAP-T011/T012" >&2
	runtime_status=1
fi
ovmf_code_hash_after=$(sha256sum "$ovmf_code" | awk '{print $1}')
ovmf_vars_hash_after=$(sha256sum "$ovmf_vars" | awk '{print $1}')
if [[ $ovmf_code_hash_after != "$ovmf_code_hash_before" ||
    $ovmf_vars_hash_after != "$ovmf_vars_hash_before" ]]; then
	echo "source OVMF firmware changed during SWAP-T011/T012" >&2
	runtime_status=1
fi
selected=$(awk 'END { print NR - 1 }' "$output/results.tsv")
passed=$(awk -F '\t' 'NR > 1 && $3 == "pass" { n++ } END { print n+0 }' \
	"$output/results.tsv")
run_kind=partial-debug
if [[ $runtime_case == all && $run_br_t46 -eq 1 ]]; then
	run_kind=full
	if [[ $selected -ne 6 ]]; then
		echo "full acceptance selected $selected cells, expected 6" >&2
		runtime_status=1
	fi
fi
{
	echo "finished_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "config_mk_sha256_after=$config_hash_after"
	echo "ovmf_code_sha256_after=$ovmf_code_hash_after"
	echo "ovmf_vars_sha256_after=$ovmf_vars_hash_after"
	echo "selected_cells=$selected"
	echo "passed_cells=$passed"
	echo "run_kind=$run_kind"
	printf 'results_sha256='
	sha256sum "$output/results.tsv" | awk '{print $1}'
} >>"$output/metadata.txt"
if [[ $runtime_status -ne 0 || $br_status -ne 0 || $passed -ne $selected ]]; then
	echo "SWAP-T011/T012 FAIL: passed=$passed selected=$selected" >&2
	exit 1
fi
if [[ $run_kind == full ]]; then
	echo "SWAP-T011/T012 PASS: $passed/$selected full cells"
else
	echo "SWAP-T011/T012 PARTIAL/DEBUG PASS: $passed/$selected cells"
fi

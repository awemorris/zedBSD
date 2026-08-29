#!/usr/bin/env bash
# WS004 p024 disposable QEMU strict-GPT publication acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
io_runner=$script_dir/qemu-nvme-io.sh
image_tool_source=$script_dir/gpt-image-tool.c
source_image=$repo/build/amd64/ws004-p023-hdd-image.img
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
cell_timeout=${CELL_TIMEOUT_SECONDS:-180}
skip_valid=${P024_SKIP_VALID:-0}

usage()
{
	cat <<EOF
usage: $0 [OUTPUT-DIRECTORY]

Builds the p023 test-only image/helper, initializes a disposable NVMe
namespace with strict primary and backup GPT, and reuses the p023 two-boot
write/fsync/readback harness through /dev/nvme0n1p1. A final QEMU boot attaches
a separately broken GPT and requires visible rejection, no partition
publication, and continued login from the disposable IDE system image.

P024_SKIP_VALID=1 runs only the malformed-GPT cell against an already built
build/amd64/ws004-p023-hdd-image.img. It exists so retained positive evidence
does not have to be repeated while refining the rejection oracle.
EOF
}

if [[ $# -eq 1 && ($1 == -h || $1 == --help) ]]; then
	usage
	exit 0
elif [[ $# -gt 1 ]]; then
	usage >&2
	exit 2
fi
for value in "$boot_timeout" "$cell_timeout"; do
	[[ $value =~ ^[1-9][0-9]{0,8}$ ]] || {
		echo "timeouts must be positive integers of at most 9 digits" >&2
		exit 2
	}
done
[[ $skip_valid == 0 || $skip_valid == 1 ]] || {
	echo "P024_SKIP_VALID must be 0 or 1" >&2
	exit 2
}
for command in "${HOSTCC:-cc}" "$qemu" awk cp date find mkdir mktemp rg sed \
	sha256sum sleep timeout tr truncate; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done
[[ -x $io_runner ]] || { echo "p023 I/O runner is absent" >&2; exit 2; }
[[ -f $ovmf_code ]] || { echo "OVMF code not found: $ovmf_code" >&2; exit 2; }
[[ -f $ovmf_vars ]] || { echo "OVMF vars not found: $ovmf_vars" >&2; exit 2; }

if [[ $# -eq 1 ]]; then
	output=$1
	if [[ -e $output ]]; then
		[[ -d $output ]] || { echo "not a directory: $output" >&2; exit 2; }
		[[ -z $(find "$output" -mindepth 1 -maxdepth 1 -print -quit) ]] || {
			echo "output directory is not empty: $output" >&2
			exit 2
		}
	fi
	mkdir -p -- "$output"
else
	temp_root=${TMPDIR:-$repo/plan/ws004-hardware/temp}
	mkdir -p -- "$temp_root"
	output=$(mktemp -d "$temp_root/ws004-p024-gpt.XXXXXX")
fi
output=$(cd -- "$output" && pwd)

metadata=$output/metadata.txt
results=$output/results.tsv
tool=$output/gpt-image-tool
valid_output=$output/valid
broken_boot=$output/broken-boot.img
broken_namespace=$output/broken-nvme.img
broken_vars=$output/OVMF_VARS-broken.fd
broken_log=$output/broken-guest.log
broken_qemu_log=$output/broken-qemu.log

printf 'case\tresult\tevidence\n' >"$results"
{
	printf 'test=HW-T20 ws004-p024 strict GPT\n'
	printf 'start_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'qemu=%s\n' "$("$qemu" --version | sed -n '1p')"
	printf 'topology=OVMF q35 IDE system image plus PCI NVMe GPT namespace\n'
	printf 'positive_device=/dev/nvme0n1p1\n'
	printf 'negative_contract=broken GPT rejected without MBR fallback or p1 publication\n'
} >"$metadata"

"${HOSTCC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror \
	"$image_tool_source" -o "$tool"
printf 'gpt-image-tool\tpass\tgpt-image-tool\n' >>"$results"

if [[ $skip_valid == 0 ]]; then
	NVME_GUEST_DEVICE=/dev/nvme0n1p1 \
	NVME_NAMESPACE_INITIALIZER="$tool" \
		"$io_runner" "$valid_output"
	printf 'partition-write-flush-restart\tpass\tvalid/\n' >>"$results"
else
	printf 'partition-write-flush-restart\tskipped\tretained external evidence\n' \
		>>"$results"
fi

[[ -f $source_image ]] || {
	echo "p023 source image missing after positive run: $source_image" >&2
	exit 1
}
source_hash=$(sha256sum "$source_image" | awk '{print $1}')
cp --reflink=auto --sparse=always "$source_image" "$broken_boot"
truncate -s 5368709120 "$broken_namespace"
"$tool" broken "$broken_namespace" >>"$metadata"
cp -- "$ovmf_vars" "$broken_vars"
: >"$broken_log"
: >"$broken_qemu_log"

marker_count()
{
	local pattern=$1 file=$2 count

	count=$(rg -a -c -- "$pattern" "$file" 2>/dev/null || true)
	printf '%s\n' "${count:-0}"
}

broken_controller()
{
	local deadline=$(( $(date +%s) + 10#$boot_timeout ))

	trap '' PIPE
	while (( $(date +%s) < deadline )); do
		if [[ $(marker_count '(^|[[:blank:]])login:[[:blank:]]*$' \
		    "$broken_log") -ge 1 ]]; then
			printf 'quit\n'
			return 0
		fi
		sleep 0.1
	done
	echo "broken-GPT boot did not reach login" >&2
	printf 'quit\n' || :
	return 1
}

qemu_command=(
	"$qemu" -machine q35 -m 512 -smp 4
	-drive "if=pflash,format=raw,readonly=on,file=$ovmf_code"
	-drive "if=pflash,format=raw,file=$broken_vars"
	-device piix3-ide,id=legacyide
	-drive "if=none,id=bootdisk,file=$broken_boot,format=raw"
	-device ide-hd,bus=legacyide.0,drive=bootdisk,bootindex=1
	-drive "if=none,id=nvmedisk,file=$broken_namespace,format=raw,cache=writeback"
	-device nvme,id=nvme0,drive=nvmedisk,serial=ZEDBSD-P024-BROKEN
	-display none -serial none -debugcon "file:$broken_log"
	-monitor stdio -no-reboot
)
{
	printf 'broken_command='
	printf '%q ' timeout --foreground --kill-after=5 "${cell_timeout}s" \
		"${qemu_command[@]}"
	printf '\n'
} >>"$metadata"

set +e
broken_controller |
	timeout --foreground --kill-after=5 "${cell_timeout}s" \
		"${qemu_command[@]}" >"$broken_qemu_log" 2>&1
pipeline_status=("${PIPESTATUS[@]}")
set -e
[[ ${pipeline_status[0]} -eq 0 && ${pipeline_status[1]} -eq 0 ]] || {
	echo "broken-GPT cell failed: controller=${pipeline_status[0]} qemu=${pipeline_status[1]}" >&2
	exit 1
}
tr -d '\r' <"$broken_log" >"$output/broken-guest-logical.log"
broken_logical=$output/broken-guest-logical.log
[[ $(marker_count 'gpt: nvme0n1 rejected:' "$broken_logical") -eq 1 ]] || {
	echo "strict GPT rejection marker is missing or ambiguous" >&2
	exit 1
}
[[ $(marker_count 'vfs: nvme0n1 partition' "$broken_logical") -eq 0 ]] || {
	echo "a partition was published from broken GPT" >&2
	exit 1
}
[[ $(marker_count '(^|[[:blank:]])login:[[:blank:]]*$' "$broken_logical") \
	-ge 1 ]] || {
	echo "system did not continue to login after rejecting broken GPT" >&2
	exit 1
}
printf 'broken-gpt-rejection\tpass\tbroken-guest-logical.log\n' >>"$results"

current_hash=$(sha256sum "$source_image" | awk '{print $1}')
[[ $current_hash == "$source_hash" ]] || {
	echo "test source image was modified" >&2
	exit 1
}
{
	printf 'source_image=%s\n' "$source_image"
	printf 'source_image_sha256=%s\n' "$source_hash"
	printf 'end_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'result=pass\n'
} >>"$metadata"
echo "HW-T20 QEMU strict GPT: PASS ($output)"

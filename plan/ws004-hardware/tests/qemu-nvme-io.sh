#!/usr/bin/env bash
# WS004 p023 disposable QEMU NVMe write/flush/restart acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
test_makefile=$script_dir/nvme-io-qemu.mk
source_image=$repo/build/amd64/ws004-p023-hdd-image.img
config=$repo/config.mk
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
build_timeout=${BUILD_TIMEOUT_SECONDS:-3600}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-120}
cell_timeout=${CELL_TIMEOUT_SECONDS:-420}
key_delay=${KEY_DELAY_SECONDS:-0.015}
guest_device=${NVME_GUEST_DEVICE:-/dev/nvme0n1}
namespace_initializer=${NVME_NAMESPACE_INITIALIZER:-}
low_offset=8388608
high_offset=4294971392
stress_offset=4296015872
concurrent_offset=4311744512

usage()
{
	cat <<EOF
usage: $0 [OUTPUT-DIRECTORY]

Builds a WS004-p023-only amd64 image containing nvme-io-guest, then boots a
disposable system-image copy twice with one persistent raw NVMe namespace.
The first boot writes, fsyncs, and rereads patterns below and above 4 GiB; the
second boot verifies both patterns after QEMU/controller restart.

When OUTPUT-DIRECTORY is omitted, mktemp uses TMPDIR. Existing non-empty output
directories are rejected. Production disk images are never booted directly.

NVME_GUEST_DEVICE may select a published partition such as /dev/nvme0n1p1.
NVME_NAMESPACE_INITIALIZER may name an executable called once with the newly
created sparse namespace path before either QEMU boot.
EOF
}

if [[ $# -eq 1 && ($1 == -h || $1 == --help) ]]; then
	usage
	exit 0
elif [[ $# -gt 1 ]]; then
	usage >&2
	exit 2
fi
for value in "$build_timeout" "$boot_timeout" "$command_timeout" \
	"$cell_timeout"; do
	[[ $value =~ ^[1-9][0-9]{0,8}$ ]] || {
		echo "timeouts must be positive integers of at most 9 digits" >&2
		exit 2
	}
done
case $key_delay in
''|*[!0-9.]*|.*|*.*.*)
	echo "KEY_DELAY_SECONDS must be a non-negative decimal" >&2
	exit 2
	;;
esac
[[ $guest_device =~ ^/dev/[a-z0-9._/-]+$ ]] || {
	echo "NVME_GUEST_DEVICE is not a sendkey-safe /dev path" >&2
	exit 2
}
if [[ -n $namespace_initializer && ! -x $namespace_initializer ]]; then
	echo "NVME_NAMESPACE_INITIALIZER is not executable: $namespace_initializer" >&2
	exit 2
fi

for command in "$qemu" awk cp date find make mkdir mktemp rg sed sha256sum \
	sleep tail timeout tr truncate; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done
[[ -f $config ]] || { echo "configuration not found: $config" >&2; exit 2; }
[[ -f $ovmf_code ]] || { echo "OVMF code not found: $ovmf_code" >&2; exit 2; }
[[ -f $ovmf_vars ]] || { echo "OVMF vars not found: $ovmf_vars" >&2; exit 2; }

config_value()
{
	local name=$1

	awk -v name="$name" '
		$0 ~ "^[[:space:]]*" name "[[:space:]]*:?=" {
			value = $0
			sub("^[[:space:]]*" name "[[:space:]]*:?=[[:space:]]*", "", value)
			sub("[[:space:]]*#.*$", "", value)
			sub("[[:space:]]+$", "", value)
			result = value
		}
		END { print result }
	' "$config"
}

[[ $(config_value ZEDBSD_PLATFORM) == amd64 &&
    $(config_value ZEDBSD_ARCHITECTURE) == amd64 &&
    $(config_value ZEDBSD_BOARD) == pcat ]] || {
	echo "HW-T20 requires an amd64/amd64/pcat config.mk" >&2
	exit 2
}

if [[ $# -eq 1 ]]; then
	output=$1
	if [[ -e $output ]]; then
		[[ -d $output ]] || {
			echo "output path is not a directory: $output" >&2
			exit 2
		}
		if [[ -n $(find "$output" -mindepth 1 -maxdepth 1 -print -quit) ]]; then
			echo "output directory is not empty: $output" >&2
			exit 2
		fi
	fi
	mkdir -p -- "$output"
else
	temp_root=${TMPDIR:-$repo/plan/ws004-hardware/temp}
	mkdir -p -- "$temp_root"
	output=$(mktemp -d "$temp_root/ws004-p023-nvme.XXXXXX")
fi
output=$(cd -- "$output" && pwd)

build_log=$output/build.log
metadata=$output/metadata.txt
results=$output/results.tsv
run_boot=$output/boot.img
namespace=$output/nvme.img
source_hash=
config_hash=$(sha256sum "$config" | awk '{print $1}')

finish()
{
	local status=$1 current_config current_source integrity=pass

	trap - EXIT
	set +e
	current_config=$(sha256sum "$config" 2>/dev/null | awk '{print $1}')
	if [[ $current_config != "$config_hash" ]]; then
		echo "config.mk changed during HW-T20" >&2
		integrity=fail
		status=1
	fi
	current_source=-
	if [[ -n $source_hash ]]; then
		current_source=$(sha256sum "$source_image" 2>/dev/null | awk '{print $1}')
		if [[ $current_source != "$source_hash" ]]; then
			echo "test source image changed during HW-T20" >&2
			integrity=fail
			status=1
		fi
	fi
	{
		printf 'end_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		printf 'config_sha256_after=%s\n' "$current_config"
		printf 'source_image_sha256_after=%s\n' "$current_source"
		printf 'input_integrity=%s\n' "$integrity"
		printf 'acceptance_exit_status=%s\n' "$status"
	} >>"$metadata" 2>/dev/null || status=1
	if [[ $status -eq 0 ]]; then
		echo "HW-T20 QEMU NVMe I/O: PASS ($output)"
	else
		echo "HW-T20 QEMU NVMe I/O: FAIL ($output)" >&2
	fi
	exit "$status"
}
trap 'finish "$?"' EXIT

: >"$build_log"
printf 'case\tresult\tevidence\n' >"$results"
{
	printf 'test=HW-T20 ws004-p023\n'
	printf 'start_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'repository=%s\n' "$repo"
	printf 'config=%s\n' "$config"
	printf 'config_sha256_before=%s\n' "$config_hash"
	printf 'qemu=%s\n' "$("$qemu" --version | sed -n '1p')"
	printf 'topology=OVMF q35 piix3-ide boot plus standard PCI NVMe\n'
	printf 'namespace_size_bytes=5368709120\n'
	printf 'low_offset=%s\n' "$low_offset"
	printf 'high_offset=%s\n' "$high_offset"
	printf 'stress_offset=%s\n' "$stress_offset"
	printf 'concurrent_offset=%s\n' "$concurrent_offset"
	printf 'guest_flush=open-pwrite-fsync-pread-compare on raw descriptor\n'
	printf 'guest_device=%s\n' "$guest_device"
	printf 'namespace_initializer=%s\n' "${namespace_initializer:--}"
} >"$metadata"

if ! timeout --foreground --kill-after=10 "${build_timeout}s" \
	make -C "$repo" -j16 toolchain >>"$build_log" 2>&1; then
	echo "make toolchain failed or timed out" >&2
	exit 1
fi
if ! timeout --foreground --kill-after=10 "${build_timeout}s" \
	make -C "$repo" -j16 -f Makefile -f "$test_makefile" \
	ws004-p023-qemu-image >>"$build_log" 2>&1; then
	echo "WS004-p023 test image build failed or timed out" >&2
	tail -n 100 "$build_log" >&2
	exit 1
fi
[[ -f $source_image ]] || {
	echo "test source image not found: $source_image" >&2
	exit 1
}
source_hash=$(sha256sum "$source_image" | awk '{print $1}')
printf 'source_image=%s\nsource_image_sha256_before=%s\n' \
	"$source_image" "$source_hash" >>"$metadata"
printf 'build\tpass\tbuild.log\n' >>"$results"

cp --reflink=auto --sparse=always "$source_image" "$run_boot"
truncate -s 5368709120 "$namespace"
if [[ -n $namespace_initializer ]]; then
	"$namespace_initializer" "$namespace" >>"$build_log" 2>&1
fi

marker_count()
{
	local pattern=$1 file=$2 count

	count=$(rg -a -c -- "$pattern" "$file" 2>/dev/null || true)
	printf '%s\n' "${count:-0}"
}

controller_deadline=0
guest_log=
controller_result=
cell_mode=

wait_for_pattern()
{
	local pattern=$1 file=$2 wait_seconds=$3 minimum=${4:-1}
	local deadline count

	deadline=$(( $(date +%s) + 10#$wait_seconds ))
	if ((controller_deadline > 0 && controller_deadline < deadline)); then
		deadline=$controller_deadline
	fi
	while :; do
		count=$(marker_count "$pattern" "$file")
		((count >= minimum)) && return 0
		(( $(date +%s) >= deadline )) && return 1
		sleep 0.1
	done
}

send_text()
{
	local text=$1 character key index lower

	for ((index = 0; index < ${#text}; index++)); do
		character=${text:index:1}
		case $character in
		' ') key=spc ;;
		/) key=slash ;;
		-) key=minus ;;
		_) key=shift-minus ;;
		.) key=dot ;;
		'$') key=shift-4 ;;
		[a-z0-9]) key=$character ;;
		[A-Z]) lower=${character,,}; key=shift-$lower ;;
		*) echo "unsupported sendkey character: $character" >&2; return 1 ;;
		esac
		printf 'sendkey %s\n' "$key"
		sleep "$key_delay"
	done
	printf 'sendkey ret\n'
}

shell_prompt='root@[^[:space:]]*:[^$]*\$([[:blank:]]|$)'
login_prompt='(^|[[:blank:]])login:[[:blank:]]*$'
password_prompt='Password:'

send_shell()
{
	local text=$1 before

	before=$(marker_count "$shell_prompt" "$guest_log")
	send_text "$text" || return 1
	wait_for_pattern "$shell_prompt" "$guest_log" "$command_timeout" \
		$((before + 1))
}

controller_body()
{
	local password_before shell_before mode=$cell_mode

	wait_for_pattern "$login_prompt" "$guest_log" "$boot_timeout" || {
		echo "login prompt timeout" >"$controller_result"
		return 1
	}
	password_before=$(marker_count "$password_prompt" "$guest_log")
	shell_before=$(marker_count "$shell_prompt" "$guest_log")
	send_text root || return 1
	wait_for_pattern "$password_prompt" "$guest_log" "$command_timeout" \
		$((password_before + 1)) || return 1
	send_text '' || return 1
	wait_for_pattern "$shell_prompt" "$guest_log" "$command_timeout" \
		$((shell_before + 1)) || return 1
	send_shell "/usr/bin/nvme-io-guest $mode $guest_device $low_offset" || {
		echo "low-offset helper timeout" >"$controller_result"
		return 1
	}
	send_shell "/usr/bin/nvme-io-guest $mode $guest_device $high_offset" || {
		echo "high-offset helper timeout" >"$controller_result"
		return 1
	}
	local stress_mode=stress-$mode
	send_shell "/usr/bin/nvme-io-guest $stress_mode $guest_device $stress_offset" || {
		echo "phase-wrap stress helper timeout" >"$controller_result"
		return 1
	}
	local concurrent_mode=concurrent-$mode
	send_shell "/usr/bin/nvme-io-guest $concurrent_mode $guest_device $concurrent_offset" || {
		echo "concurrent helper timeout" >"$controller_result"
		return 1
	}
	echo pass >"$controller_result"
}

controller()
{
	local status

	trap '' PIPE
	controller_deadline=$(( $(date +%s) + 10#$cell_timeout ))
	set +e
	controller_body
	status=$?
	[[ $status -eq 0 ]] || echo "guest control failed" >>"$controller_result"
	printf 'quit\n' || :
	return "$status"
}

validate_cell()
{
	local mode=$1 log=$2 trace=$3 offset marker stress_mode=stress-$1
	local concurrent_mode=concurrent-$1
	local fatal='fatal:|FATAL:|kernel panic|panic:| fault v=|VFS initialization failed|HW-T20 NVME-IO FAIL|nvme: .*timed out|nvme: .*quarantined|nvme: I/O recovery failed'

	if rg -a -q -- "$fatal" "$log"; then
		rg -a -m 1 -- "$fatal" "$log" >&2
		return 1
	fi
	[[ $(marker_count 'nvme: /dev/nvme0n1 namespace=1 .* writable' "$log") -eq 1 ]] || {
		echo "missing or ambiguous writable namespace marker" >&2
		return 1
	}
	for offset in "$low_offset" "$high_offset"; do
		for marker in \
		    "^HW-T20 NVME-IO BEGIN mode=$mode device=$guest_device offset=$offset$" \
		    "^HW-T20 NVME-IO READBACK bytes=4096 offset=$offset$" \
		    "^HW-T20 NVME-IO PASS mode=$mode offset=$offset$"; do
			[[ $(marker_count "$marker" "$log") -eq 1 ]] || {
				echo "missing or ambiguous marker: $marker" >&2
				return 1
			}
		done
		if [[ $mode == write ]]; then
			for marker in \
			    "^HW-T20 NVME-IO WRITE bytes=4096 offset=$offset$" \
			    "^HW-T20 NVME-IO FSYNC offset=$offset$"; do
				[[ $(marker_count "$marker" "$log") -eq 1 ]] || {
					echo "missing or ambiguous marker: $marker" >&2
					return 1
				}
			done
		fi
	done
	for marker in \
	    "^HW-T20 NVME-IO BEGIN mode=$stress_mode device=$guest_device offset=$stress_offset$" \
	    "^HW-T20 NVME-IO STRESS-READBACK commands=96 bytes=393216 offset=$stress_offset$" \
	    "^HW-T20 NVME-IO PASS mode=$stress_mode offset=$stress_offset$"; do
		[[ $(marker_count "$marker" "$log") -eq 1 ]] || {
			echo "missing or ambiguous marker: $marker" >&2
			return 1
		}
	done
	if [[ $mode == write ]]; then
		for marker in \
		    "^HW-T20 NVME-IO STRESS-WRITE commands=96 bytes=393216 offset=$stress_offset$" \
		    "^HW-T20 NVME-IO FSYNC offset=$stress_offset$"; do
			[[ $(marker_count "$marker" "$log") -eq 1 ]] || {
				echo "missing or ambiguous marker: $marker" >&2
				return 1
			}
		done
	fi
	for marker in \
	    "^HW-T20 NVME-IO BEGIN mode=$concurrent_mode device=$guest_device offset=$concurrent_offset$" \
	    "^HW-T20 NVME-IO CONCURRENT-READBACK workers=4 commands=128 bytes=524288 offset=$concurrent_offset$" \
	    "^HW-T20 NVME-IO PASS mode=$concurrent_mode offset=$concurrent_offset$"; do
		[[ $(marker_count "$marker" "$log") -eq 1 ]] || {
			echo "missing or ambiguous marker: $marker" >&2
			return 1
		}
	done
	if [[ $mode == write ]]; then
		for marker in \
		    "^HW-T20 NVME-IO CONCURRENT-WRITE workers=4 commands=128 bytes=524288 offset=$concurrent_offset$" \
		    "^HW-T20 NVME-IO FSYNC offset=$concurrent_offset$"; do
			[[ $(marker_count "$marker" "$log") -eq 1 ]] || {
				echo "missing or ambiguous marker: $marker" >&2
				return 1
			}
		done
	fi
	[[ $(marker_count 'pci_nvme_io_cmd ' "$trace") -ge 224 ]] || {
		echo "fewer than 224 model I/O commands reached SQ1" >&2
		return 1
	}
	[[ $(marker_count 'pci_nvme_mmio_doorbell_sq sqid 1 new_tail 0$' \
	    "$trace") -ge 1 ]] || {
		echo "SQ1 did not wrap to tail zero" >&2
		return 1
	}
	[[ $(marker_count 'pci_nvme_mmio_doorbell_cq cqid 1 new_head 0$' \
	    "$trace") -ge 1 ]] || {
		echo "CQ1 did not wrap to head zero" >&2
		return 1
	}
}

run_cell()
{
	local index=$1 mode=$2
	local vars=$output/OVMF_VARS-$index.fd
	local qemu_log=$output/qemu-$index.log
	local trace_log=$output/nvme-trace-$index.log
	local logical_log=$output/guest-$index-logical.log
	local qemu_command pipeline_status controller_status qemu_status

	guest_log=$output/guest-$index.log
	controller_result=$output/controller-$index.txt
	cell_mode=$mode
	cp -- "$ovmf_vars" "$vars"
	: >"$guest_log"
	: >"$qemu_log"
	: >"$controller_result"
	qemu_command=(
		"$qemu" -machine q35 -m 512 -smp 4
		-drive "if=pflash,format=raw,readonly=on,file=$ovmf_code"
		-drive "if=pflash,format=raw,file=$vars"
		-device piix3-ide,id=legacyide
		-drive "if=none,id=bootdisk,file=$run_boot,format=raw"
		-device ide-hd,bus=legacyide.0,drive=bootdisk,bootindex=1
		-drive "if=none,id=nvmedisk,file=$namespace,format=raw,cache=writeback"
		-device nvme,id=nvme0,drive=nvmedisk,serial=ZEDBSD-P023
		-trace "enable=pci_nvme_*,file=$trace_log"
		-display none -serial none -debugcon "file:$guest_log"
		-monitor stdio -no-reboot
	)
	{
		printf 'cell_%s_mode=%s\n' "$index" "$mode"
		printf 'cell_%s_command=' "$index"
		printf '%q ' timeout --foreground --kill-after=5 "${cell_timeout}s" \
			"${qemu_command[@]}"
		printf '\n'
	} >>"$metadata"
	set +e
	controller |
		timeout --foreground --kill-after=5 "${cell_timeout}s" \
			"${qemu_command[@]}" >"$qemu_log" 2>&1
	pipeline_status=("${PIPESTATUS[@]}")
	set -e
	controller_status=${pipeline_status[0]}
	qemu_status=${pipeline_status[1]}
	if [[ $controller_status -ne 0 || $qemu_status -ne 0 ||
	    $(<"$controller_result") != pass ]]; then
		echo "cell $index failed: controller=$controller_status qemu=$qemu_status" >&2
		[[ ! -s $controller_result ]] || cat "$controller_result" >&2
		return 1
	fi
	tr -d '\r' <"$guest_log" >"$logical_log"
	validate_cell "$mode" "$logical_log" "$trace_log"
}

run_cell 1 write
printf 'write-fsync-readback\tpass\tguest-1-logical.log\n' >>"$results"
run_cell 2 verify
printf 'restart-readback\tpass\tguest-2-logical.log\n' >>"$results"

printf 'result=pass\n' >>"$metadata"

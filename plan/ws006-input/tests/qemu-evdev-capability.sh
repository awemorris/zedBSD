#!/usr/bin/env bash
# WS006 IN-T12 production capability-discovery QEMU acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
config=$repo/config.mk
makefile=$script_dir/evdev-capability-qemu.mk
probe_source=$script_dir/evdev-capability-probe.c
test_image=$repo/build/amd64/ws006-p005-hdd-image.img
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
build_timeout=${BUILD_TIMEOUT_SECONDS:-1800}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-30}
cell_timeout=${CELL_TIMEOUT_SECONDS:-300}
key_delay=${KEY_DELAY_SECONDS:-0.015}

usage()
{
	cat <<EOF
usage: $0 [OUTPUT-DIRECTORY]

Builds a test-only amd64 image containing the IN-T12 guest probe, boots a
writable copy, and requires one keyboard and one relative pointer discovered
solely with EVIOCGBIT.  The probe also checks ioctl bitmap copy boundaries,
rejection without caller-buffer mutation, and EVIOCGKEY.  If OUTPUT-DIRECTORY
is omitted, an untracked directory is created below plan/ws006-input/temp/.
EOF
}

if [[ $# -eq 1 && ($1 == -h || $1 == --help) ]]; then
	usage
	exit 0
fi
if [[ $# -gt 1 ]]; then
	usage >&2
	exit 2
fi
for timeout_value in "$build_timeout" "$boot_timeout" "$command_timeout" \
	"$cell_timeout"; do
	if ! [[ $timeout_value =~ ^[1-9][0-9]{0,8}$ ]]; then
		echo "timeouts must be canonical positive integers of at most 9 digits" >&2
		exit 2
	fi
done
case $key_delay in
''|*[!0-9.]*|.*|*.*.*)
	echo "KEY_DELAY_SECONDS must be a non-negative decimal" >&2
	exit 2
	;;
esac

[[ -f $config ]] || { echo "configuration not found: $config" >&2; exit 2; }
for command in "$qemu" awk cp date make rg sed sha256sum sleep timeout tr; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done

config_value()
{
	local name=$1 file=$2

	awk -v name="$name" '
		$0 ~ "^[[:space:]]*" name "[[:space:]]*:?=" {
			value = $0
			sub("^[[:space:]]*" name "[[:space:]]*:?=[[:space:]]*", "", value)
			sub("[[:space:]]*#.*$", "", value)
			sub("[[:space:]]+$", "", value)
			result = value
		}
		END { print result }
	' "$file"
}

[[ $(config_value ZEDBSD_PLATFORM "$config") == amd64 &&
    $(config_value ZEDBSD_ARCHITECTURE "$config") == amd64 &&
    $(config_value ZEDBSD_BOARD "$config") == pcat ]] || {
	echo "IN-T12 requires an amd64/amd64/pcat config.mk" >&2
	exit 2
}

# Keep the test's central promise machine-checkable: fixed node numbers and
# identity/name ioctls must never enter the role-discovery probe.
if rg -n '(/dev/input/event[0-9]|EVIOCG(NAME|PHYS|UNIQ|ID))' "$probe_source"; then
	echo "guest probe contains a forbidden node/identity assumption" >&2
	exit 2
fi

if [[ $# -eq 1 ]]; then
	output=$1
	[[ ! -e $output ]] || { echo "output path already exists: $output" >&2; exit 2; }
	mkdir -p -- "$output"
else
	temp_root=$repo/plan/ws006-input/temp
	mkdir -p -- "$temp_root"
	output=$(mktemp -d "$temp_root/q020-p005-capability.XXXXXX")
fi
output=$(cd -- "$output" && pwd)

build_log=$output/build.log
guest_log=$output/guest.log
logical_log=$output/guest-logical.log
qemu_log=$output/qemu.log
probe_log=$output/probe-section.log
metadata=$output/run-metadata.txt
results=$output/results.tsv
controller_result=$output/controller-result.txt
run_image=$output/run.img
config_hash=$(sha256sum "$config" | awk '{print $1}')

printf 'case\tresult\tevidence\n' >"$results"
: >"$build_log"
: >"$guest_log"
: >"$qemu_log"
: >"$controller_result"

build_command=(make -C "$repo" -j16 -f Makefile -f "$makefile"
	ws006-p005-qemu-image)
{
	printf 'test=IN-T12\n'
	printf 'start_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'repository=%s\n' "$repo"
	printf 'config=%s\n' "$config"
	printf 'config_sha256_before=%s\n' "$config_hash"
	printf 'probe_source=%s\n' "$probe_source"
	printf 'probe_source_sha256=%s\n' "$(sha256sum "$probe_source" | awk '{print $1}')"
	printf 'test_image=%s\n' "$test_image"
	printf 'qemu=%s\n' "$("$qemu" --version | sed -n '1p')"
	printf 'build_command='
	printf '%q ' timeout --foreground --kill-after=10 "${build_timeout}s" \
		"${build_command[@]}"
	printf '\n'
} >"$metadata"

finish_acceptance()
{
	local status=$1 current_config integrity=pass

	trap - EXIT
	set +e
	current_config=$(sha256sum "$config" 2>/dev/null | awk '{print $1}')
	if [[ -z $current_config || $current_config != "$config_hash" ]]; then
		echo "config.mk changed during IN-T12" >&2
		integrity=fail
		status=1
	fi
	{
		printf 'end_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		printf 'config_sha256_after=%s\n' "${current_config:-missing}"
		printf 'input_integrity_result=%s\n' "$integrity"
		printf 'acceptance_exit_status=%s\n' "$status"
	} >>"$metadata" || status=1
	printf 'input-integrity\t%s\trun-metadata.txt\n' "$integrity" >>"$results" || status=1
	if [[ $status -eq 0 ]]; then
		rm -f -- "$run_image" || status=1
	fi
	if [[ $status -eq 0 ]]; then
		echo "WS006 IN-T12 QEMU acceptance: PASS ($output)"
	else
		echo "WS006 IN-T12 QEMU acceptance: FAIL ($output)" >&2
	fi
	exit "$status"
}
trap 'finish_acceptance "$?"' EXIT

set +e
timeout --foreground --kill-after=10 "${build_timeout}s" \
	"${build_command[@]}" >"$build_log" 2>&1
build_status=$?
set -e
if [[ $build_status -ne 0 || ! -f $test_image ]]; then
	echo "IN-T12 image build failed or timed out (status $build_status)" >&2
	exit 1
fi
printf 'build\tpass\tbuild.log\n' >>"$results"
printf 'test_image_sha256=%s\n' "$(sha256sum "$test_image" | awk '{print $1}')" \
	>>"$metadata"
cp --reflink=auto --sparse=always "$test_image" "$run_image"

controller_deadline=0

marker_count()
{
	local pattern=$1 file=$2 count

	count=$(rg -a -c -- "$pattern" "$file" 2>/dev/null || true)
	printf '%s\n' "${count:-0}"
}

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
		'_') key=shift-minus ;;
		'$') key=shift-4 ;;
		'?') key=shift-slash ;;
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
	local password_before shell_before

	wait_for_pattern "$login_prompt" "$guest_log" "$boot_timeout" || return 1
	password_before=$(marker_count "$password_prompt" "$guest_log")
	shell_before=$(marker_count "$shell_prompt" "$guest_log")
	send_text root || return 1
	wait_for_pattern "$password_prompt" "$guest_log" "$command_timeout" \
		$((password_before + 1)) || return 1
	send_text '' || return 1
	wait_for_pattern "$shell_prompt" "$guest_log" "$command_timeout" \
		$((shell_before + 1)) || return 1
	send_shell 'echo IN-T12-BEGIN' || return 1
	# QEMU sendkey releases are asynchronous.  Let the i8042 keyboard drain
	# before opening the PS/2 mouse-backed event node for capability queries.
	send_shell 'sleep 1' || return 1
	send_shell '/usr/bin/evdev-capability-probe' || return 1
	send_shell 'echo IN-T12-STATUS-$?' || return 1
	send_shell 'echo IN-T12-END' || return 1
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
	if [[ $status -ne 0 ]]; then
		echo "guest control timed out" >"$controller_result"
	fi
	printf 'quit\n' || :
	return "$status"
}

qemu_command=(
	"$qemu" -machine pc -m 512 -smp 4
	-drive "file=$run_image,format=raw,if=ide"
	-display none -serial none -debugcon "file:$guest_log"
	-monitor stdio -no-reboot
)
set +e
controller |
	timeout --foreground --kill-after=5 "${cell_timeout}s" \
		"${qemu_command[@]}" >"$qemu_log" 2>&1
pipeline_status=("${PIPESTATUS[@]}")
set -e
if [[ ${pipeline_status[0]} -ne 0 || ${pipeline_status[1]} -ne 0 ||
    $(<"$controller_result") != pass ]]; then
	echo "IN-T12 QEMU/controller failure: ${pipeline_status[*]}" >&2
	exit 1
fi

tr -d '\r' <"$guest_log" >"$logical_log"
awk '
	$0 == "IN-T12-BEGIN" { active = 1; found_begin = 1; next }
	$0 == "IN-T12-END" { found_end = active; exit }
	active { print }
	END { if (!found_begin || !found_end) exit 1 }
' "$logical_log" >"$probe_log"

[[ $(marker_count '^IN-T12 PASS devices=[0-9]+ keyboards=1 relative-pointers=1$' \
	"$probe_log") -eq 1 ]] || {
	echo "capability probe did not identify the production roles" >&2
	exit 1
}
[[ $(marker_count '^IN-T12-STATUS-0$' "$probe_log") -eq 1 ]] || {
	echo "capability probe returned a nonzero status" >&2
	exit 1
}
[[ $(marker_count '^IN-T12 caps path=/dev/input/event[0-9]+ .*roles=keyboard boundaries=pass$' \
	"$probe_log") -eq 1 ]] || {
	echo "keyboard capability record missing or ambiguous" >&2
	exit 1
}
[[ $(marker_count '^IN-T12 caps path=/dev/input/event[0-9]+ .*roles=relative-pointer boundaries=pass$' \
	"$probe_log") -eq 1 ]] || {
	echo "relative-pointer capability record missing or ambiguous" >&2
	exit 1
}
fatal_pattern='fatal:|FATAL:|kernel panic|panic:| fault v=|VFS initialization failed|Input/output error|Segmentation fault|IN-T12 FAIL'
if rg -a -q "$fatal_pattern" "$logical_log"; then
	rg -a -m 1 "$fatal_pattern" "$logical_log" >&2
	exit 1
fi
printf 'capability-discovery\tpass\tprobe-section.log\n' >>"$results"
exit 0

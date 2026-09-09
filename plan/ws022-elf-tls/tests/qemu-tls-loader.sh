#!/usr/bin/env bash
# WS022 static TLS loader and exec rollback acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
config=${ZEDBSD_TEST_CONFIG:-$repo/config.mk}
makefile=$script_dir/tls-loader-qemu.mk
arch=${TLS_TEST_ARCH:-amd64}
if [[ $arch == i386 ]]; then
	makefile=$script_dir/tls-loader-pcat-qemu.mk
fi
# Never boot an image with a missing or partial malformed-ELF corpus.
fixture_dir=$repo/plan/ws022-elf-tls/temp/q128-fixtures
[[ -f $fixture_dir/manifest.tsv ]] || { echo "TLS fixture manifest missing" >&2; exit 2; }
[[ $(find "$fixture_dir" -maxdepth 1 -name "$arch*.elf" | wc -l) -eq 14 ]] || exit 2
probe_source=$script_dir/tls-loader-probe.c
test_image=$repo/build/amd64/ws022-p002-hdd-image.img
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
if [[ $arch == i386 ]]; then
	test_image=$repo/build/pcat/ws022-p002-hdd-image.img
	qemu=${QEMU_SYSTEM_I386:-qemu-system-i386}
fi
build_timeout=${BUILD_TIMEOUT_SECONDS:-1800}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-30}
cell_timeout=${CELL_TIMEOUT_SECONDS:-300}
key_delay=${KEY_DELAY_SECONDS:-0.015}

usage()
{
	cat <<EOF
usage: $0 [OUTPUT-DIRECTORY]

Builds a disposable amd64 image, rejects malformed TLS executables, and
checks compiler-emitted initial TLS through a successful fork/exec.
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

[[ $(config_value ZEDBSD_PLATFORM "$config") == "$arch" &&
    $(config_value ZEDBSD_ARCHITECTURE "$config") == "$arch" &&
    $(config_value ZEDBSD_BOARD "$config") == pcat ]] || {
	echo "TLS-LOADER requires an amd64/amd64/pcat config.mk" >&2
	exit 2
}

if [[ $# -eq 1 ]]; then
	output=$1
	[[ ! -e $output ]] || { echo "output path already exists: $output" >&2; exit 2; }
	mkdir -p -- "$output"
else
	temp_root=$repo/plan/ws022-elf-tls/temp
	mkdir -p -- "$temp_root"
	output=$(mktemp -d "$temp_root/q128-loader.XXXXXX")
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

build_command=(make -C "$repo" -j16 "ZEDBSD_CONFIG=$config" -f Makefile -f "$makefile"
	ws022-p002-qemu-image)
{
	printf 'test=TLS-LOADER\n'
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
		echo "config.mk changed during TLS-LOADER" >&2
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
		echo "WS022 TLS-LOADER QEMU acceptance: PASS ($output)"
	else
		echo "WS022 TLS-LOADER QEMU acceptance: FAIL ($output)" >&2
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
	echo "TLS-LOADER image build failed or timed out (status $build_status)" >&2
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
		if rg -a -q "kernel panic|VFS initialization failed|amd64 fault v=" "$file"; then return 1; fi
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
	send_shell 'echo TLS-LOADER-BEGIN' || return 1
	# Let asynchronous sendkey releases drain before the probe.
	send_shell 'sleep 1' || return 1
	send_shell '/usr/bin/tls-loader-probe' || return 1
	send_shell '/bin/dyntest' || return 1
	send_shell 'echo TLS-LOADER-STATUS-$?' || return 1
	send_shell 'echo TLS-LOADER-END' || return 1
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
	"$qemu" -machine q35 -m 512 -smp 4
	-device qemu-xhci,id=xhci
	-drive "file=$run_image,format=raw,if=none,id=boot"
	-device usb-storage,bus=xhci.0,drive=boot,bootindex=1
	-display none -serial none -debugcon "file:$guest_log"
	-monitor stdio -no-reboot
)
if [[ $arch == i386 ]]; then
	qemu_command=(
		"$qemu" -machine pc -m 128 -smp 1
		-drive "file=$run_image,format=raw,if=ide"
		-display none -serial none -debugcon "file:$guest_log"
		-monitor stdio -no-reboot
	)
fi
set +e
controller |
	timeout --foreground --kill-after=5 "${cell_timeout}s" \
		"${qemu_command[@]}" >"$qemu_log" 2>&1
pipeline_status=("${PIPESTATUS[@]}")
set -e
if [[ ${pipeline_status[0]} -ne 0 || ${pipeline_status[1]} -ne 0 ||
    $(<"$controller_result") != pass ]]; then
	echo "TLS-LOADER QEMU/controller failure: ${pipeline_status[*]}" >&2
	exit 1
fi

tr -d '\r' <"$guest_log" >"$logical_log"
awk '
	$0 == "TLS-LOADER-BEGIN" { active = 1; found_begin = 1; next }
	$0 == "TLS-LOADER-END" { found_end = active; exit }
	active { print }
	END { if (!found_begin || !found_end) exit 1 }
' "$logical_log" >"$probe_log"

[[ $(marker_count '^TLS-LOADER PASS rejected=10 initial=4$' "$probe_log") -eq 1 ]]
[[ $(marker_count '^TLS-RUNTIME PASS concurrent=8 repeated=100 signal fork failed-create-recovery$' "$probe_log") -eq 1 ]]
[[ $(marker_count '^TLS-INITIAL PASS$' "$probe_log") -eq 2 ]]
[[ $(marker_count '^DL:06:PLUGIN-TLS$' "$probe_log") -eq 1 ]]
fatal_pattern='fatal:|FATAL:|kernel panic|panic:| fault v=|VFS initialization failed|Input/output error|Segmentation fault|TLS-LOADER FAIL|TLS-RUNTIME FAIL'
if rg -a -q "$fatal_pattern" "$logical_log"; then
	rg -a -m 1 "$fatal_pattern" "$logical_log" >&2
	exit 1
fi
printf 'tls-loader\tpass\tprobe-section.log\n' >>"$results"
exit 0

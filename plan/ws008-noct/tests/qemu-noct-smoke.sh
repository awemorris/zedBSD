#!/usr/bin/env bash
# WS008 NOCT-T003 canonical amd64 Noct package/QEMU acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
production_config=$repo/config.mk
production_image=$repo/build/amd64/hdd-image.img
package_artifact=$repo/build/amd64/bin/noct
staged_artifact=$repo/build/amd64/rootfs/usr/bin/noct
cmake_artifact=${NOCT_CMAKE_ARTIFACT:-$repo/userland/noct/NoctLang/build-zedbsd/noct}
noct_source=${NOCT_SOURCE_DIR:-$repo/userland/noct/NoctLang}
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
build_timeout=${BUILD_TIMEOUT_SECONDS:-3600}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-60}
cell_timeout=${CELL_TIMEOUT_SECONDS:-300}
key_delay=${KEY_DELAY_SECONDS:-0.015}

usage()
{
	cat <<EOF
usage: $0 [OUTPUT-DIRECTORY]

Builds the amd64 image with a private copy of config.mk that selects the Noct
package, then boots a writable image copy and runs the canonical CLI with JIT
disabled.  If OUTPUT-DIRECTORY is omitted, an untracked directory is created
under plan/ws008-noct/temp/.  Existing output paths are never reused.

NOCT_CMAKE_ARTIFACT may override the expected canonical CMake output path.
EOF
}

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

[[ -f $production_config ]] || {
	echo "configuration not found: $production_config" >&2
	exit 2
}
[[ -d $noct_source ]] || {
	echo "Noct integration source not found: $noct_source" >&2
	exit 2
}
for command in "$qemu" awk cksum cp date git make rg sed sha256sum sleep timeout tr; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done

if [[ $# -eq 1 ]]; then
	output=$1
	if [[ -e $output ]]; then
		echo "output path already exists: $output" >&2
		exit 2
	fi
	mkdir -p -- "$output"
else
	temp_root=$repo/plan/ws008-noct/temp
	mkdir -p -- "$temp_root"
	output=$(mktemp -d "$temp_root/q019-p001-noct.XXXXXX")
fi
output=$(cd -- "$output" && pwd)

temporary_config=$output/config-noct.mk
build_log=$output/build.log
guest_log=$output/guest.log
logical_log=$output/guest-logical.log
qemu_log=$output/qemu.log
smoke_log=$output/smoke-section.log
metadata=$output/run-metadata.txt
results=$output/results.tsv
controller_result=$output/controller-result.txt
run_image=$output/run.img
source_status=$output/noct-source-status.txt

hash_file()
{
	local hash ignored

	read -r hash ignored < <(sha256sum "$1")
	printf '%s\n' "$hash"
}

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

require_amd64_config()
{
	local file=$1

	[[ $(config_value ZEDBSD_PLATFORM "$file") == amd64 ]] || {
		echo "NOCT-T003 requires ZEDBSD_PLATFORM=amd64: $file" >&2
		return 1
	}
	[[ $(config_value ZEDBSD_ARCHITECTURE "$file") == amd64 ]] || {
		echo "NOCT-T003 requires ZEDBSD_ARCHITECTURE=amd64: $file" >&2
		return 1
	}
	[[ $(config_value ZEDBSD_BOARD "$file") == pcat ]] || {
		echo "NOCT-T003 requires ZEDBSD_BOARD=pcat: $file" >&2
		return 1
	}
}

config_hash_before=$(hash_file "$production_config")
cp -- "$production_config" "$temporary_config"
printf '\n# NOCT-T003 private package selection.\nZEDBSD_USER_PROGRAMS += noct cksum\n' \
	>>"$temporary_config"
require_amd64_config "$temporary_config"
temporary_config_hash_before=$(hash_file "$temporary_config")
git -C "$noct_source" status --porcelain=v1 --untracked-files=all \
	>"$source_status"
noct_revision=$(git -C "$noct_source" rev-parse HEAD)
qemu_version=$("$qemu" --version | sed -n '1p')

printf 'case\tresult\tevidence\n' >"$results"
: >"$build_log"
: >"$guest_log"
: >"$qemu_log"
: >"$controller_result"

build_command=(make -C "$repo" -j16 "ZEDBSD_CONFIG=$temporary_config")
boot_parameter_command=(make -C "$repo" -B
	"ZEDBSD_CONFIG=$temporary_config" build/amd64/generated/boot-parameters.h)
qemu_command=(
	"$qemu" -machine pc -m 512 -smp 4
	-drive "file=$run_image,format=raw,if=ide"
	-display none -serial none -debugcon "file:$guest_log"
	-monitor stdio -no-reboot
)
{
	printf 'test=NOCT-T003\n'
	printf 'start_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'repository=%s\n' "$repo"
	printf 'production_config=%s\n' "$production_config"
	printf 'production_config_sha256_before=%s\n' "$config_hash_before"
	printf 'temporary_config=%s\n' "$temporary_config"
	printf 'temporary_config_sha256_before=%s\n' \
		"$temporary_config_hash_before"
	printf 'noct_source=%s\n' "$noct_source"
	printf 'noct_source_revision=%s\n' "$noct_revision"
	printf 'noct_source_status=%s\n' "$source_status"
	printf 'cmake_artifact=%s\n' "$cmake_artifact"
	printf 'package_artifact=%s\n' "$package_artifact"
	printf 'production_image=%s\n' "$production_image"
	printf 'qemu=%s\n' "$qemu_version"
	printf 'console_capture=isa-debugcon\n'
	printf 'input_control=qemu-monitor-sendkey\n'
	printf 'build_timeout_seconds=%s\n' "$build_timeout"
	printf 'boot_timeout_seconds=%s\n' "$boot_timeout"
	printf 'command_timeout_seconds=%s\n' "$command_timeout"
	printf 'cell_timeout_seconds=%s\n' "$cell_timeout"
	printf 'build_command='
	printf '%q ' timeout --foreground --kill-after=10 "${build_timeout}s" \
		"${build_command[@]}"
	printf '\n'
	printf 'boot_parameter_command='
	printf '%q ' "${boot_parameter_command[@]}"
	printf '\n'
	printf 'qemu_command='
	printf '%q ' timeout --foreground --kill-after=5 "${cell_timeout}s" \
		"${qemu_command[@]}"
	printf '\n'
} >"$metadata"

input_image_hash=
cmake_hash=
package_hash=
staged_hash=
host_cksum_crc=
host_cksum_size=
acceptance_status=1

finish_acceptance()
{
	local status=$1 current_config current_temporary current_image=-
	local current_cmake=- current_package=- integrity=pass

	trap - EXIT
	set +e
	if [[ -f $production_config ]]; then
		current_config=$(hash_file "$production_config" 2>/dev/null)
	else
		current_config=missing
	fi
	if [[ $current_config != "$config_hash_before" ]]; then
		echo "production config changed during NOCT-T003" >&2
		integrity=fail
		status=1
	fi
	if [[ -f $temporary_config ]]; then
		current_temporary=$(hash_file "$temporary_config" 2>/dev/null)
	else
		current_temporary=missing
	fi
	if [[ $current_temporary != "$temporary_config_hash_before" ]]; then
		echo "private test config changed during NOCT-T003" >&2
		integrity=fail
		status=1
	fi
	if [[ -n $input_image_hash ]]; then
		if [[ -f $production_image ]]; then
			current_image=$(hash_file "$production_image" 2>/dev/null)
		else
			current_image=missing
		fi
		if [[ $current_image != "$input_image_hash" ]]; then
			echo "production input image changed during QEMU acceptance" >&2
			integrity=fail
			status=1
		fi
	fi
	if [[ -n $cmake_hash ]]; then
		[[ -f $cmake_artifact ]] &&
			current_cmake=$(hash_file "$cmake_artifact" 2>/dev/null)
		[[ -f $package_artifact ]] &&
			current_package=$(hash_file "$package_artifact" 2>/dev/null)
		if [[ $current_cmake != "$cmake_hash" ||
		    $current_package != "$package_hash" ]]; then
			echo "Noct artifact changed during QEMU acceptance" >&2
			integrity=fail
			status=1
		fi
	fi
	{
		printf 'end_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		printf 'production_config_sha256_after=%s\n' "$current_config"
		printf 'temporary_config_sha256_after=%s\n' "$current_temporary"
		printf 'production_image_sha256_after=%s\n' "$current_image"
		printf 'cmake_artifact_sha256_after=%s\n' "$current_cmake"
		printf 'package_artifact_sha256_after=%s\n' "$current_package"
		printf 'input_integrity_result=%s\n' "$integrity"
		printf 'acceptance_exit_status=%s\n' "$status"
	} >>"$metadata" || status=1
	printf 'input-integrity\t%s\trun-metadata.txt\n' "$integrity" \
		>>"$results" || status=1
	if [[ $status -eq 0 ]]; then
		rm -f -- "$run_image" || status=1
	fi
	if [[ $status -eq 0 ]]; then
		echo "WS008 NOCT-T003 QEMU acceptance: PASS ($output)"
	else
		echo "WS008 NOCT-T003 QEMU acceptance: FAIL ($output)" >&2
	fi
	exit "$status"
}

trap 'finish_acceptance "$?"' EXIT

build_start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
build_started=$(date +%s)
set +e
timeout --foreground --kill-after=10 "${build_timeout}s" \
	"${boot_parameter_command[@]}" >"$build_log" 2>&1 && \
	timeout --foreground --kill-after=10 "${build_timeout}s" \
	"${build_command[@]}" >>"$build_log" 2>&1
build_status=$?
set -e
build_elapsed=$(( $(date +%s) - build_started ))
{
	printf 'build_start_utc=%s\n' "$build_start_utc"
	printf 'build_end_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'build_status=%s\n' "$build_status"
	printf 'build_elapsed_seconds=%s\n' "$build_elapsed"
} >>"$metadata"
if [[ $build_status -ne 0 ]]; then
	echo "make -j16 failed or timed out (status $build_status)" >&2
	exit 1
fi
printf 'build\tpass\tbuild.log\n' >>"$results"

for required_file in "$cmake_artifact" "$package_artifact" \
	"$staged_artifact" "$production_image"; do
	if [[ ! -f $required_file ]]; then
		echo "required build artifact not found: $required_file" >&2
		exit 1
	fi
done
cmake_hash=$(hash_file "$cmake_artifact")
package_hash=$(hash_file "$package_artifact")
staged_hash=$(hash_file "$staged_artifact")
if [[ $cmake_hash != "$package_hash" || $cmake_hash != "$staged_hash" ]]; then
	echo "canonical, package, and staged Noct artifacts are not identical" >&2
	exit 1
fi
read -r host_cksum_crc host_cksum_size ignored < <(cksum "$package_artifact")
[[ $host_cksum_crc =~ ^[0-9]+$ && $host_cksum_size =~ ^[0-9]+$ ]] || {
	echo "could not calculate the package artifact POSIX checksum" >&2
	exit 1
}
input_image_hash=$(hash_file "$production_image")
{
	printf 'cmake_artifact_sha256=%s\n' "$cmake_hash"
	printf 'package_artifact_sha256=%s\n' "$package_hash"
	printf 'staged_artifact=%s\n' "$staged_artifact"
	printf 'staged_artifact_sha256=%s\n' "$staged_hash"
	printf 'package_artifact_cksum=%s %s\n' \
		"$host_cksum_crc" "$host_cksum_size"
	printf 'production_image_sha256=%s\n' "$input_image_hash"
} >>"$metadata"
printf 'artifact-identity\tpass\trun-metadata.txt\n' >>"$results"

cp --reflink=auto --sparse=always "$production_image" "$run_image"

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
	local deadline count now

	now=$(date +%s)
	deadline=$((now + 10#$wait_seconds))
	if ((controller_deadline > 0 && controller_deadline < deadline)); then
		deadline=$controller_deadline
	fi
	while :; do
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
		'=') key=equal ;;
		"'") key=apostrophe ;;
		'"') key=shift-apostrophe ;;
		'(') key=shift-9 ;;
		')') key=shift-0 ;;
		';') key=semicolon ;;
		'$') key=shift-4 ;;
		'?' ) key=shift-slash ;;
		[a-z0-9]) key=$character ;;
		[A-Z])
			lower=${character,,}
			key=shift-$lower
			;;
		*)
			echo "unsupported sendkey character: $character" >&2
			return 1
			;;
		esac
		printf 'sendkey %s\n' "$key"
		sleep "$key_delay"
	done
	printf 'sendkey ret\n'
}

shell_prompt='root@[^[:space:]]*:[^$]*\$([[:blank:]]|$)'
login_prompt='(^|[[:blank:]])login:[[:blank:]]*$'
password_prompt='Password:'

login_guest()
{
	local password_before shell_before

	password_before=$(marker_count "$password_prompt" "$guest_log")
	shell_before=$(marker_count "$shell_prompt" "$guest_log")
	send_text root || return 1
	wait_for_pattern "$password_prompt" "$guest_log" "$command_timeout" \
		$((password_before + 1)) || return 1
	send_text '' || return 1
	wait_for_pattern "$shell_prompt" "$guest_log" "$command_timeout" \
		$((shell_before + 1))
}

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
	wait_for_pattern "$login_prompt" "$guest_log" "$boot_timeout" || {
		echo "login prompt timeout" >"$controller_result"
		return 1
	}
	login_guest || {
		echo "root login timeout" >"$controller_result"
		return 1
	}
	send_shell 'echo NOCT-P001-BEGIN' || {
		echo "begin marker timeout" >"$controller_result"
		return 1
	}
	send_shell 'cksum /usr/bin/noct' || {
		echo "guest artifact checksum timeout" >"$controller_result"
		return 1
	}
	send_shell "/usr/bin/noct -j0 -e 'print(\"NOCT-P001-SMOKE\")'" || {
		echo "Noct smoke command timeout" >"$controller_result"
		return 1
	}
	send_shell 'echo NOCT-P001-STATUS-$?' || {
		echo "status marker timeout" >"$controller_result"
		return 1
	}
	send_shell 'echo NOCT-P001-END' || {
		echo "end marker timeout" >"$controller_result"
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
	printf 'quit\n' || :
	return "$status"
}

qemu_start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
qemu_started=$(date +%s)
set +e
controller |
	timeout --foreground --kill-after=5 "${cell_timeout}s" \
		"${qemu_command[@]}" >"$qemu_log" 2>&1
pipeline_status=("${PIPESTATUS[@]}")
set -e
controller_status=${pipeline_status[0]}
qemu_status=${pipeline_status[1]}
qemu_elapsed=$(( $(date +%s) - qemu_started ))
{
	printf 'qemu_start_utc=%s\n' "$qemu_start_utc"
	printf 'qemu_end_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'controller_status=%s\n' "$controller_status"
	printf 'qemu_status=%s\n' "$qemu_status"
	printf 'qemu_elapsed_seconds=%s\n' "$qemu_elapsed"
} >>"$metadata"
if [[ $controller_status -ne 0 || $qemu_status -ne 0 ]]; then
	echo "QEMU cell failed: controller=$controller_status qemu=$qemu_status" >&2
	[[ ! -s $controller_result ]] || cat "$controller_result" >&2
	exit 1
fi
if [[ $(<"$controller_result") != pass ]]; then
	cat "$controller_result" >&2
	exit 1
fi

tr -d '\r' <"$guest_log" >"$logical_log"
awk '
	$0 == "NOCT-P001-BEGIN" { active = 1; found_begin = 1; next }
	$0 == "NOCT-P001-END" { found_end = active; exit }
	active { print }
	END { if (!found_begin || !found_end) exit 1 }
' "$logical_log" >"$smoke_log"

smoke_count=$(marker_count '^NOCT-P001-SMOKE$' "$smoke_log")
status_count=$(marker_count '^NOCT-P001-STATUS-0$' "$smoke_log")
cksum_count=$(marker_count \
	"^${host_cksum_crc}[[:blank:]]+${host_cksum_size}[[:blank:]]+/usr/bin/noct$" \
	"$smoke_log")
if [[ $smoke_count -ne 1 || $status_count -ne 1 || $cksum_count -ne 1 ]]; then
	echo "guest smoke result/status marker mismatch" >&2
	exit 1
fi
if rg -a -q 'noct-jit:|NOCT-P001-STATUS-[1-9][0-9]*|Error:' "$smoke_log"; then
	echo "guest smoke used JIT or reported a failure" >&2
	exit 1
fi
if ! rg -a -q 'init: system running' "$logical_log" ||
	! rg -a -q '^login: root$' "$logical_log" ||
	! rg -a -q "$shell_prompt" "$logical_log"; then
	echo "guest did not reach the production init/login state" >&2
	exit 1
fi
fatal_pattern='fatal:|FATAL:|kernel panic|panic:| fault v=|VFS initialization failed|Input/output error|Out of memory\.|Segmentation fault|loop[0-9]+: .*error=-?[1-9]|usb-storage: .*error=-?[1-9]'
if rg -a -q "$fatal_pattern" "$logical_log"; then
	rg -a -m 1 "$fatal_pattern" "$logical_log" >&2
	exit 1
fi
printf 'guest-non-jit\tpass\tsmoke-section.log\n' >>"$results"
printf 'guest-artifact-identity\tpass\tsmoke-section.log\n' >>"$results"
printf 'guest-fatal-scan\tpass\tguest-logical.log\n' >>"$results"

acceptance_status=0
exit "$acceptance_status"

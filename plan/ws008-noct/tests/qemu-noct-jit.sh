#!/usr/bin/env bash
# WS008 NOCT-T020/T021/T022 amd64 executable-memory/JIT acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
runner=$script_dir/qemu-noct-jit.sh
production_config=$repo/config.mk
production_image=$repo/build/amd64/hdd-image.img
cmake_artifact=${NOCT_CMAKE_ARTIFACT:-$repo/userland/noct/NoctLang/build-zedbsd/noct}
package_artifact=$repo/build/amd64/bin/noct
staged_artifact=$repo/build/amd64/rootfs/usr/bin/noct
probe_source=$script_dir/noct-jit-vm-probe.c
probe_target=build/amd64/NOCT-JIT-VM-PROBE.ELF
probe_artifact=$repo/$probe_target
probe_staged=$repo/build/amd64/rootfs/usr/bin/noct-jit-vm-probe
jit_script=$script_dir/noct-jit-qemu.noct
noct_source=${NOCT_SOURCE_DIR:-$repo/userland/noct/NoctLang}
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
build_timeout=${BUILD_TIMEOUT_SECONDS:-3600}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-90}
cell_timeout=${CELL_TIMEOUT_SECONDS:-420}
key_delay=${KEY_DELAY_SECONDS:-0.015}

# Keep the lifecycle vocabulary local so the runner and opt-in canonical Noct
# diagnostics can evolve together without weakening the acceptance checks.
jit_compiled_pattern='^noct-jit: jit_target: compiled$'
jit_mapped_pattern='^noct-jit-memory: mmap-rw size=[1-9][0-9]* status=ok$'
jit_protected_pattern='^noct-jit-memory: mprotect-rx size=[1-9][0-9]* status=ok$'
jit_entered_pattern='^noct-jit: jit_target: native-entry$'
jit_unmapped_pattern='^noct-jit-memory: munmap size=[1-9][0-9]* status=ok$'
jit_published_pattern='^noct-jit-lifecycle: publish status=ok$'
jit_destroyed_pattern='^noct-jit-lifecycle: destroy status=ok$'
jit_result_marker='NOCT-JIT-RESULT-4242'

usage()
{
	cat <<EOF
usage: $0 [OUTPUT-DIRECTORY]

Builds a private amd64 image containing the test-only executable-memory probe
and Noct JIT fixture, then runs NOCT-T020, T021, and T022 in one disposable
qemu-system-x86_64 instance.  With no OUTPUT-DIRECTORY, evidence is written
below plan/ws008-noct/temp/.  Existing output paths are never reused.

NOCT_CMAKE_ARTIFACT and NOCT_SOURCE_DIR may override the canonical artifact
and integration source paths.  QEMU_SYSTEM_X86_64 may select the QEMU binary.
EOF
}

if [[ $# -gt 1 ]]; then
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

for required in "$production_config" "$probe_source" "$jit_script"; do
	[[ -f $required ]] || {
		echo "required input not found: $required" >&2
		exit 2
	}
done
[[ -d $noct_source ]] || {
	echo "Noct integration source not found: $noct_source" >&2
	exit 2
}
for command in "$qemu" awk cksum cp date git make rg sed sha256sum sleep \
	timeout tr; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done

if [[ $# -eq 1 ]]; then
	output=$1
	[[ ! -e $output ]] || {
		echo "output path already exists: $output" >&2
		exit 2
	}
	mkdir -p -- "$output"
else
	mkdir -p -- "$repo/plan/ws008-noct/temp"
	output=$(mktemp -d \
		"$repo/plan/ws008-noct/temp/q020-p003-jit.XXXXXX")
fi
output=$(cd -- "$output" && pwd)

temporary_config=$output/config-jit.mk
build_log=$output/build.log
guest_log=$output/guest.log
logical_log=$output/guest-logical.log
qemu_log=$output/qemu.log
metadata=$output/run-metadata.txt
results=$output/results.tsv
controller_result=$output/controller-result.txt
run_image=$output/run.img
zedbsd_status=$output/zedbsd-source-status.txt
noct_status=$output/noct-source-status.txt
t020_log=$output/t020-direct.log
t021_log=$output/t021-forced-jit.log
t022_interpreter_log=$output/t022-interpreter.log
t022_jit_log=$output/t022-second-jit.log

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

	[[ $(config_value ZEDBSD_PLATFORM "$file") == amd64 &&
	   $(config_value ZEDBSD_ARCHITECTURE "$file") == amd64 &&
	   $(config_value ZEDBSD_BOARD "$file") == pcat ]] || {
		echo "NOCT-T020/T021/T022 requires amd64 PC/AT: $file" >&2
		return 1
	}
}

production_config_hash=$(hash_file "$production_config")
cp -- "$production_config" "$temporary_config"
cat >>"$temporary_config" <<EOF

# NOCT-T020/T021/T022 private payload.  The direct probe is a test-only
# amd64 target, not a registered base-system package.
ZEDBSD_USER_PROGRAMS += noct cksum
ZEDBSD_PACKAGE_INPUTS += $probe_target $jit_script
ZEDBSD_PACKAGE_FILES += --file /usr/bin/noct-jit-vm-probe=$probe_target --mode /usr/bin/noct-jit-vm-probe=0755
ZEDBSD_PACKAGE_FILES += --file /usr/share/noct/noct-jit-qemu.noct=$jit_script --mode /usr/share/noct/noct-jit-qemu.noct=0644
EOF
require_amd64_config "$temporary_config"
temporary_config_hash=$(hash_file "$temporary_config")

git -C "$repo" status --porcelain=v1 --untracked-files=all >"$zedbsd_status"
git -C "$noct_source" status --porcelain=v1 --untracked-files=all \
	>"$noct_status"
zedbsd_revision=$(git -C "$repo" rev-parse HEAD)
noct_revision=$(git -C "$noct_source" rev-parse HEAD)
qemu_version=$("$qemu" --version | sed -n '1p')
probe_source_hash=$(hash_file "$probe_source")
jit_script_hash=$(hash_file "$jit_script")
runner_hash=$(hash_file "$runner")

printf 'case\tresult\tevidence\n' >"$results"
: >"$build_log"
: >"$guest_log"
: >"$qemu_log"
: >"$controller_result"

build_command=(make -C "$repo" -j16 "ZEDBSD_CONFIG=$temporary_config"
	"$probe_target" disk-image)
qemu_command=(
	"$qemu" -machine pc -m 512 -smp 4
	-drive "file=$run_image,format=raw,if=ide"
	-display none -serial none -debugcon "file:$guest_log"
	-monitor stdio -no-reboot
)
guest_t020_command='/usr/bin/noct-jit-vm-probe'
guest_t021_command='NOCT_JIT_DEBUG=1 /usr/bin/noct -j /usr/share/noct/noct-jit-qemu.noct'
guest_t022_interpreter_command='NOCT_JIT_DEBUG=1 /usr/bin/noct -j0 /usr/share/noct/noct-jit-qemu.noct'
guest_t022_jit_command=$guest_t021_command

{
	printf 'tests=NOCT-T020,NOCT-T021,NOCT-T022\n'
	printf 'start_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'repository=%s\n' "$repo"
	printf 'zedbsd_revision=%s\n' "$zedbsd_revision"
	printf 'zedbsd_source_status=%s\n' "$zedbsd_status"
	printf 'noct_source=%s\n' "$noct_source"
	printf 'noct_revision=%s\n' "$noct_revision"
	printf 'noct_source_status=%s\n' "$noct_status"
	printf 'production_config=%s\n' "$production_config"
	printf 'production_config_sha256_before=%s\n' \
		"$production_config_hash"
	printf 'temporary_config=%s\n' "$temporary_config"
	printf 'temporary_config_sha256_before=%s\n' \
		"$temporary_config_hash"
	printf 'probe_source=%s\n' "$probe_source"
	printf 'probe_source_sha256=%s\n' "$probe_source_hash"
	printf 'jit_script=%s\n' "$jit_script"
	printf 'jit_script_sha256=%s\n' "$jit_script_hash"
	printf 'runner=%s\n' "$runner"
	printf 'runner_sha256=%s\n' "$runner_hash"
	printf 'cmake_artifact=%s\n' "$cmake_artifact"
	printf 'package_artifact=%s\n' "$package_artifact"
	printf 'probe_artifact=%s\n' "$probe_artifact"
	printf 'production_image=%s\n' "$production_image"
	printf 'qemu=%s\n' "$qemu_version"
	printf 'console_capture=isa-debugcon\n'
	printf 'input_control=qemu-monitor-sendkey\n'
	printf 'build_timeout_seconds=%s\n' "$build_timeout"
	printf 'boot_timeout_seconds=%s\n' "$boot_timeout"
	printf 'command_timeout_seconds=%s\n' "$command_timeout"
	printf 'cell_timeout_seconds=%s\n' "$cell_timeout"
	printf 'build_command='; printf '%q ' timeout --foreground --kill-after=10 \
		"${build_timeout}s" "${build_command[@]}"; printf '\n'
	printf 'qemu_command='; printf '%q ' timeout --foreground --kill-after=5 \
		"${cell_timeout}s" "${qemu_command[@]}"; printf '\n'
	printf 'guest_t020_command=%s\n' "$guest_t020_command"
	printf 'guest_t021_command=%s\n' "$guest_t021_command"
	printf 'guest_t022_interpreter_command=%s\n' \
		"$guest_t022_interpreter_command"
	printf 'guest_t022_second_jit_command=%s\n' "$guest_t022_jit_command"
} >"$metadata"

image_hash=
cmake_hash=
package_hash=
probe_hash=
acceptance_status=1

finish_acceptance()
{
	local status=$1 current_config current_temporary current_image=-
	local current_cmake=- current_package=- current_probe=- integrity=pass
	local current_probe_source current_jit_script current_runner

	trap - EXIT
	set +e
	current_config=$(hash_file "$production_config" 2>/dev/null || echo missing)
	current_temporary=$(hash_file "$temporary_config" 2>/dev/null || echo missing)
	if [[ $current_config != "$production_config_hash" ||
	      $current_temporary != "$temporary_config_hash" ]]; then
		echo "configuration changed during JIT acceptance" >&2
		integrity=fail
		status=1
	fi
	current_probe_source=$(hash_file "$probe_source" 2>/dev/null || echo missing)
	current_jit_script=$(hash_file "$jit_script" 2>/dev/null || echo missing)
	current_runner=$(hash_file "$runner" 2>/dev/null || echo missing)
	if [[ $current_probe_source != "$probe_source_hash" ||
	      $current_jit_script != "$jit_script_hash" ||
	      $current_runner != "$runner_hash" ]]; then
		echo "acceptance fixture or runner changed during execution" >&2
		integrity=fail
		status=1
	fi
	if [[ -n $image_hash ]]; then
		current_image=$(hash_file "$production_image" 2>/dev/null || echo missing)
		if [[ $current_image != "$image_hash" ]]; then
			echo "production image changed during QEMU acceptance" >&2
			integrity=fail
			status=1
		fi
	fi
	if [[ -n $cmake_hash ]]; then
		current_cmake=$(hash_file "$cmake_artifact" 2>/dev/null || echo missing)
		current_package=$(hash_file "$package_artifact" 2>/dev/null || echo missing)
		current_probe=$(hash_file "$probe_artifact" 2>/dev/null || echo missing)
		if [[ $current_cmake != "$cmake_hash" ||
		      $current_package != "$package_hash" ||
		      $current_probe != "$probe_hash" ]]; then
			echo "tested executable changed during acceptance" >&2
			integrity=fail
			status=1
		fi
	fi
	{
		printf 'end_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		printf 'production_config_sha256_after=%s\n' "$current_config"
		printf 'temporary_config_sha256_after=%s\n' "$current_temporary"
		printf 'probe_source_sha256_after=%s\n' "$current_probe_source"
		printf 'jit_script_sha256_after=%s\n' "$current_jit_script"
		printf 'runner_sha256_after=%s\n' "$current_runner"
		printf 'production_image_sha256_after=%s\n' "$current_image"
		printf 'cmake_artifact_sha256_after=%s\n' "$current_cmake"
		printf 'package_artifact_sha256_after=%s\n' "$current_package"
		printf 'probe_artifact_sha256_after=%s\n' "$current_probe"
		printf 'input_integrity_result=%s\n' "$integrity"
		printf 'acceptance_exit_status=%s\n' "$status"
	} >>"$metadata" || status=1
	printf 'input-integrity\t%s\trun-metadata.txt\n' "$integrity" \
		>>"$results" || status=1
	if [[ $status -eq 0 ]]; then
		rm -f -- "$run_image" || status=1
	fi
	if [[ $status -eq 0 ]]; then
		echo "WS008 NOCT-T020/T021/T022 acceptance: PASS ($output)"
	else
		echo "WS008 NOCT-T020/T021/T022 acceptance: FAIL ($output)" >&2
	fi
	exit "$status"
}

trap 'finish_acceptance "$?"' EXIT

build_start=$(date +%s)
printf 'build_start_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
	>>"$metadata"
set +e
timeout --foreground --kill-after=10 "${build_timeout}s" \
	"${build_command[@]}" >"$build_log" 2>&1
build_status=$?
set -e
{
	printf 'build_end_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'build_status=%s\n' "$build_status"
	printf 'build_elapsed_seconds=%s\n' "$(( $(date +%s) - build_start ))"
} >>"$metadata"
[[ $build_status -eq 0 ]] || {
	echo "private make -j16 failed or timed out (status $build_status)" >&2
	exit 1
}
printf 'build\tpass\tbuild.log\n' >>"$results"

for required in "$cmake_artifact" "$package_artifact" "$staged_artifact" \
	"$probe_artifact" "$probe_staged" "$production_image"; do
	[[ -f $required ]] || {
		echo "required build artifact not found: $required" >&2
		exit 1
	}
done
cmake_hash=$(hash_file "$cmake_artifact")
package_hash=$(hash_file "$package_artifact")
staged_hash=$(hash_file "$staged_artifact")
probe_hash=$(hash_file "$probe_artifact")
probe_staged_hash=$(hash_file "$probe_staged")
[[ $cmake_hash == "$package_hash" && $cmake_hash == "$staged_hash" ]] || {
	echo "canonical, packaged, and staged Noct artifacts differ" >&2
	exit 1
}
[[ $probe_hash == "$probe_staged_hash" ]] || {
	echo "built and staged direct VM probe artifacts differ" >&2
	exit 1
}
read -r noct_crc noct_size ignored < <(cksum "$package_artifact")
read -r probe_crc probe_size ignored < <(cksum "$probe_artifact")
image_hash=$(hash_file "$production_image")
{
	printf 'cmake_artifact_sha256=%s\n' "$cmake_hash"
	printf 'package_artifact_sha256=%s\n' "$package_hash"
	printf 'staged_artifact_sha256=%s\n' "$staged_hash"
	printf 'probe_artifact_sha256=%s\n' "$probe_hash"
	printf 'probe_staged_sha256=%s\n' "$probe_staged_hash"
	printf 'noct_cksum=%s %s\n' "$noct_crc" "$noct_size"
	printf 'probe_cksum=%s %s\n' "$probe_crc" "$probe_size"
	printf 'production_image_sha256=%s\n' "$image_hash"
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
	local pattern=$1 file=$2 seconds=$3 minimum=${4:-1}
	local deadline count

	deadline=$(( $(date +%s) + 10#$seconds ))
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
		' ') key=spc ;; /) key=slash ;; -) key=minus ;;
		_) key=shift-minus ;; .) key=dot ;; '=') key=equal ;;
		"'") key=apostrophe ;; '"') key=shift-apostrophe ;;
		'(') key=shift-9 ;; ')') key=shift-0 ;; ';') key=semicolon ;;
		'$') key=shift-4 ;; '?') key=shift-slash ;;
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

login_guest()
{
	local passwords shells

	passwords=$(marker_count "$password_prompt" "$guest_log")
	shells=$(marker_count "$shell_prompt" "$guest_log")
	send_text root
	wait_for_pattern "$password_prompt" "$guest_log" "$command_timeout" \
		$((passwords + 1))
	send_text ''
	wait_for_pattern "$shell_prompt" "$guest_log" "$command_timeout" \
		$((shells + 1))
}

send_shell()
{
	local command=$1 shells

	shells=$(marker_count "$shell_prompt" "$guest_log")
	send_text "$command"
	wait_for_pattern "$shell_prompt" "$guest_log" "$command_timeout" \
		$((shells + 1))
}

run_section()
{
	local name=$1 command=$2

	send_shell "echo ${name}-BEGIN"
	send_shell "$command"
	send_shell "echo ${name}-STATUS-\$?"
	send_shell "echo ${name}-END"
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
	send_shell "cksum /usr/bin/noct"
	send_shell "cksum /usr/bin/noct-jit-vm-probe"
	run_section NOCT-T020 "$guest_t020_command"
	run_section NOCT-T021 "$guest_t021_command"
	run_section NOCT-T022-INTERPRETER "$guest_t022_interpreter_command"
	run_section NOCT-T022-JIT2 "$guest_t022_jit_command"
	send_shell 'echo NOCT-T022-CONSOLE-OK'
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
	[[ $status -eq 0 ]] || echo fail >"$controller_result"
	printf 'quit\n' || :
	return "$status"
}

qemu_start=$(date +%s)
printf 'qemu_start_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
	>>"$metadata"
set +e
controller |
	timeout --foreground --kill-after=5 "${cell_timeout}s" \
		"${qemu_command[@]}" >"$qemu_log" 2>&1
pipeline_status=("${PIPESTATUS[@]}")
set -e
controller_status=${pipeline_status[0]}
qemu_status=${pipeline_status[1]}
{
	printf 'qemu_end_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'controller_status=%s\n' "$controller_status"
	printf 'qemu_status=%s\n' "$qemu_status"
	printf 'qemu_elapsed_seconds=%s\n' "$(( $(date +%s) - qemu_start ))"
} >>"$metadata"
[[ $controller_status -eq 0 && $qemu_status -eq 0 &&
	$(<"$controller_result") == pass ]] || {
	echo "QEMU controller failed: ${pipeline_status[*]}" >&2
	[[ ! -s $controller_result ]] || cat "$controller_result" >&2
	exit 1
}

tr -d '\r' <"$guest_log" >"$logical_log"

extract_section()
{
	local name=$1 destination=$2

	awk -v begin="${name}-BEGIN" -v end="${name}-END" '
		$0 == begin { active = 1; found_begin++; next }
		$0 == end { if (active) found_end++; active = 0; next }
		active { print }
		END { if (found_begin != 1 || found_end != 1 || active) exit 1 }
	' "$logical_log" >"$destination"
}

extract_section NOCT-T020 "$t020_log"
extract_section NOCT-T021 "$t021_log"
extract_section NOCT-T022-INTERPRETER "$t022_interpreter_log"
extract_section NOCT-T022-JIT2 "$t022_jit_log"

for marker in NOCT-T020-RW-OK NOCT-T020-RX-OK NOCT-T020-EXEC-OK \
	NOCT-T020-INVALID-OK NOCT-T020-UNMAP-OK NOCT-T020-PASS \
	NOCT-T020-STATUS-0; do
	[[ $(marker_count "^${marker}$" "$t020_log") -eq 1 ]] || {
		echo "NOCT-T020 marker missing or duplicated: $marker" >&2
		exit 1
	}
done
printf 'NOCT-T020\tpass\tt020-direct.log\n' >>"$results"

check_forced_jit_section()
{
	local file=$1 status_marker=$2
	local pattern

	[[ $(marker_count "^${jit_result_marker}$" "$file") -eq 1 &&
	   $(marker_count "^${status_marker}$" "$file") -eq 1 ]] || return 1
	for pattern in "$jit_compiled_pattern" "$jit_mapped_pattern" \
		"$jit_protected_pattern" "$jit_entered_pattern" \
		"$jit_unmapped_pattern" "$jit_published_pattern" \
		"$jit_destroyed_pattern"; do
		[[ $(marker_count "$pattern" "$file") -ge 1 ]] || return 1
	done
	if rg -a -i -q \
		'fallback|noct-jit:.*fail|mmap.*fail|mprotect.*fail|munmap.*fail|segmentation fault|user fault|kernel fault|Error:' \
		"$file"; then
		return 1
	fi
}

check_forced_jit_section "$t021_log" NOCT-T021-STATUS-0 || {
	echo "NOCT-T021 forced-JIT result/lifecycle evidence failed" >&2
	exit 1
}
printf 'NOCT-T021\tpass\tt021-forced-jit.log\n' >>"$results"

[[ $(marker_count "^${jit_result_marker}$" "$t022_interpreter_log") -eq 1 &&
   $(marker_count '^NOCT-T022-INTERPRETER-STATUS-0$' \
	"$t022_interpreter_log") -eq 1 ]] || {
	echo "NOCT-T022 interpreter result/status failed" >&2
	exit 1
}
if rg -a -q 'noct-jit:|noct-jit-memory:|noct-jit-lifecycle:' \
	"$t022_interpreter_log"; then
	echo "NOCT-T022 interpreter negative control emitted JIT evidence" >&2
	exit 1
fi
check_forced_jit_section "$t022_jit_log" NOCT-T022-JIT2-STATUS-0 || {
	echo "NOCT-T022 second forced-JIT lifecycle failed" >&2
	exit 1
}
[[ $(marker_count '^NOCT-T022-CONSOLE-OK$' "$logical_log") -eq 1 ]] || {
	echo "guest console did not survive JIT teardown" >&2
	exit 1
}
printf 'NOCT-T022\tpass\tt022-interpreter.log, t022-second-jit.log\n' \
	>>"$results"

[[ $(marker_count \
	"^${noct_crc}[[:blank:]]+${noct_size}[[:blank:]]+/usr/bin/noct$" \
	"$logical_log") -eq 1 &&
   $(marker_count \
	"^${probe_crc}[[:blank:]]+${probe_size}[[:blank:]]+/usr/bin/noct-jit-vm-probe$" \
	"$logical_log") -eq 1 ]] || {
	echo "guest executable checksum mismatch" >&2
	exit 1
}
printf 'guest-artifact-identity\tpass\tguest-logical.log\n' >>"$results"

fatal_pattern='fatal:|FATAL:|kernel panic|panic:| fault v=|VFS initialization failed|Input/output error|Out of memory\.|Segmentation fault|NOCT-T020-FAIL|noct-jit[^:]*:.*status=failed|loop[0-9]+: .*error=-?[1-9]|usb-storage: .*error=-?[1-9]'
if rg -a -q "$fatal_pattern" "$logical_log"; then
	rg -a -m 1 "$fatal_pattern" "$logical_log" >&2
	exit 1
fi
printf 'guest-fatal-scan\tpass\tguest-logical.log\n' >>"$results"

qemu_fatal_pattern='(^|[[:space:]])Error:|unknown command|invalid command|invalid parameter|qemu-system-[^:]+:.*([Ee]rror|[Ff]ailed|Could not|cannot)|[Aa]ssertion .* failed|Segmentation fault|Aborted'
if rg -a -q "$qemu_fatal_pattern" "$qemu_log"; then
	rg -a -m 1 "$qemu_fatal_pattern" "$qemu_log" >&2
	exit 1
fi
printf 'qemu-fatal-scan\tpass\tqemu.log\n' >>"$results"

acceptance_status=0
exit "$acceptance_status"

#!/usr/bin/env bash
# WS008 NOCT-T011/T012/T013 canonical zedBSD BeUI acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
production_config=$repo/config.mk
production_image=$repo/build/amd64/hdd-image.img
package_artifact=$repo/build/amd64/bin/noct
staged_artifact=$repo/build/amd64/rootfs/usr/bin/noct
cmake_artifact=${NOCT_CMAKE_ARTIFACT:-$repo/userland/noct/build-zedbsd/noct}
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
build_timeout=${BUILD_TIMEOUT_SECONDS:-3600}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-90}
cell_timeout=${CELL_TIMEOUT_SECONDS:-360}
key_delay=${KEY_DELAY_SECONDS:-0.015}

if [[ $# -gt 1 ]]; then
	echo "usage: $0 [OUTPUT-DIRECTORY]" >&2
	exit 2
fi
for command in "$qemu" awk base64 cc cp date make rg sed sha256sum \
	sleep timeout tr; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done
[[ -f $production_config ]] || {
	echo "configuration not found: $production_config" >&2
	exit 2
}

if [[ $# -eq 1 ]]; then
	output=$1
	[[ ! -e $output ]] || {
		echo "output path already exists: $output" >&2
		exit 2
	}
	mkdir -p -- "$output"
else
	mkdir -p -- "$repo/plan/ws008-noct/temp"
	output=$(mktemp -d "$repo/plan/ws008-noct/temp/q020-p002-beui.XXXXXX")
fi
output=$(cd -- "$output" && pwd)

config=$output/config-beui.mk
bmp=$output/beui-zedbsd.bmp
build_log=$output/build.log
guest_log=$output/guest.log
logical_log=$output/guest-logical.log
qemu_log=$output/qemu.log
screen=$output/beui-screen.ppm
screen_report=$output/beui-screen-report.txt
checker=$output/beui-ppm-check
run_image=$output/run.img
metadata=$output/run-metadata.txt
results=$output/results.tsv
controller_result=$output/controller-result.txt

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

production_config_hash=$(hash_file "$production_config")
cp -- "$production_config" "$config"
base64 --decode "$script_dir/beui-zedbsd.bmp.b64" >"$bmp"
cat >>"$config" <<EOF

# NOCT-T011/T012 private package payload.
ZEDBSD_USER_PROGRAMS += noct
ZEDBSD_PACKAGE_INPUTS += $script_dir/beui-zedbsd-qemu.noct $bmp
ZEDBSD_PACKAGE_FILES += --file /usr/share/noct/beui-zedbsd.noct=$script_dir/beui-zedbsd-qemu.noct --file /usr/share/noct/beui-zedbsd.bmp=$bmp
EOF
[[ $(config_value ZEDBSD_PLATFORM "$config") == amd64 &&
   $(config_value ZEDBSD_ARCHITECTURE "$config") == amd64 &&
   $(config_value ZEDBSD_BOARD "$config") == pcat ]] || {
	echo "NOCT-T011/T012 requires the amd64 PC/AT configuration" >&2
	exit 2
}
config_hash=$(hash_file "$config")

printf 'case\tresult\tevidence\n' >"$results"
: >"$build_log"
: >"$guest_log"
: >"$qemu_log"
: >"$controller_result"

build_command=(make -C "$repo" -j16 "ZEDBSD_CONFIG=$config")
qemu_command=(
	"$qemu" -machine pc -m 512 -smp 4
	-drive "file=$run_image,format=raw,if=ide"
	-display none -serial none -debugcon "file:$guest_log"
	-monitor stdio -no-reboot
)
{
	printf 'tests=NOCT-T011,NOCT-T012,NOCT-T013\n'
	printf 'start_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'repository=%s\n' "$repo"
	printf 'production_config=%s\n' "$production_config"
	printf 'production_config_sha256_before=%s\n' "$production_config_hash"
	printf 'test_config=%s\n' "$config"
	printf 'test_config_sha256=%s\n' "$config_hash"
	printf 'production_image=%s\n' "$production_image"
	printf 'qemu=%s\n' "$("$qemu" --version | sed -n '1p')"
	printf 'build_command='; printf '%q ' "${build_command[@]}"; printf '\n'
	printf 'qemu_command='; printf '%q ' "${qemu_command[@]}"; printf '\n'
} >"$metadata"

timeout --foreground --kill-after=10 "${build_timeout}s" \
	"${build_command[@]}" >"$build_log" 2>&1
printf 'build\tpass\tbuild.log\n' >>"$results"
for required in "$cmake_artifact" "$package_artifact" "$staged_artifact" \
	"$production_image"; do
	[[ -f $required ]] || {
		echo "required artifact missing: $required" >&2
		exit 1
	}
done
cmake_hash=$(hash_file "$cmake_artifact")
[[ $cmake_hash == "$(hash_file "$package_artifact")" &&
   $cmake_hash == "$(hash_file "$staged_artifact")" ]] || {
	echo "canonical, packaged, and staged Noct artifacts differ" >&2
	exit 1
}
printf 'artifact-identity\tpass\trun-metadata.txt\n' >>"$results"
printf 'cmake_artifact_sha256=%s\n' "$cmake_hash" >>"$metadata"

backend=$repo/userland/noct/src/api/api-beui-zedbsd.c
input_source=$repo/userland/noct/src/api/beui-zedbsd-input.c
link_file=$repo/userland/noct/build-zedbsd/CMakeFiles/noctapi.dir/link.txt
for source in "$backend" "$input_source"; do
	[[ -f $source ]] || { echo "backend source missing: $source" >&2; exit 1; }
	if rg -n 'ZEDBSD_CONSOLE_(POLL_EVENT|READ_EVENT|KEY_STATE|DRAIN_INPUT)|#include[[:space:]]*[<"]zedbsd/console.h' "$source"; then
		echo "legacy console input dependency remains in canonical BeUI" >&2
		exit 1
	fi
done
rg -q 'api-beui-zedbsd.c' "$link_file"
rg -q 'beui-zedbsd-input.c' "$link_file"
nm "$cmake_artifact" | rg -q 'noct_register_api_beui_zedbsd'
printf 'NOCT-T013\tpass\tcanonical source and linked-object audit\n' >>"$results"

cc -std=c11 -Wall -Wextra -Werror \
	"$script_dir/beui-zedbsd-ppm-check.c" -o "$checker"
cp --reflink=auto --sparse=always "$production_image" "$run_image"

marker_count()
{
	local pattern=$1 count
	count=$(rg -a -c -- "$pattern" "$guest_log" 2>/dev/null || true)
	printf '%s\n' "${count:-0}"
}

deadline=0
wait_for_pattern()
{
	local pattern=$1 seconds=$2 minimum=${3:-1} end count
	end=$(( $(date +%s) + 10#$seconds ))
	if ((deadline > 0 && deadline < end)); then end=$deadline; fi
	while :; do
		count=$(marker_count "$pattern")
		((count >= minimum)) && return 0
		(( $(date +%s) >= end )) && return 1
		sleep 0.1
	done
}

send_text()
{
	local text=$1 character key index lower
	for ((index = 0; index < ${#text}; index++)); do
		character=${text:index:1}
		case $character in
		' ') key=spc ;; /) key=slash ;; -) key=minus ;; _) key=shift-minus ;;
		.) key=dot ;; '=') key=equal ;; "'") key=apostrophe ;;
		'$') key=shift-4 ;; [a-z0-9]) key=$character ;;
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
	passwords=$(marker_count "$password_prompt")
	shells=$(marker_count "$shell_prompt")
	send_text root
	wait_for_pattern "$password_prompt" "$command_timeout" $((passwords + 1))
	send_text ''
	wait_for_pattern "$shell_prompt" "$command_timeout" $((shells + 1))
}

send_shell()
{
	local command=$1 shells
	shells=$(marker_count "$shell_prompt")
	send_text "$command"
	wait_for_pattern "$shell_prompt" "$command_timeout" $((shells + 1))
}

controller_body()
{
	wait_for_pattern "$login_prompt" "$boot_timeout"
	login_guest
	send_text '/usr/bin/noct -j0 /usr/share/noct/beui-zedbsd.noct'
	wait_for_pattern '^NOCT-T011-DRAWN$' "$command_timeout"
	printf 'screendump %s\n' "$screen"
	printf 'mouse_move 20 10\n'
	printf 'sendkey shift 1000\n'
	printf 'mouse_button 1\n'
	sleep 0.5
	printf 'mouse_button 0\n'
	wait_for_pattern '^NOCT-T012-INPUT-OK$' "$command_timeout"
	wait_for_pattern '^NOCT-T011-CLOSED$' "$command_timeout"
	# The one-second Shift injection can release just after the shell prompt
	# is drawn and splice kernel output onto that line.  CLOSED is emitted
	# only on the test's return-0 path, so wait for key release and ask the
	# shell for one fresh, unambiguous post-BeUI marker.
	sleep 1
	send_shell 'echo NOCT-T012-CONSOLE-OK'
	echo pass >"$controller_result"
}

controller()
{
	local status
	trap '' PIPE
	deadline=$(( $(date +%s) + 10#$cell_timeout ))
	set +e
	controller_body
	status=$?
	if [[ $status -ne 0 ]]; then echo fail >"$controller_result"; fi
	printf 'quit\n' || :
	return "$status"
}

set +e
controller |
	timeout --foreground --kill-after=5 "${cell_timeout}s" \
		"${qemu_command[@]}" >"$qemu_log" 2>&1
pipeline_status=("${PIPESTATUS[@]}")
set -e
[[ ${pipeline_status[0]} -eq 0 && ${pipeline_status[1]} -eq 0 &&
   $(<"$controller_result") == pass ]] || {
	echo "QEMU controller failed: ${pipeline_status[*]}" >&2
	exit 1
}
tr -d '\r' <"$guest_log" >"$logical_log"

for marker in NOCT-T011-BEGIN NOCT-T011-DRAWN NOCT-T012-INPUT-OK \
	NOCT-T011-CLOSED NOCT-T012-CONSOLE-OK; do
	[[ $(rg -c "^${marker}$" "$logical_log" || true) -eq 1 ]] || {
		echo "missing or duplicate guest marker: $marker" >&2
		exit 1
	}
done
if rg -a -q 'NOCT-T0(11|12)-.*FAIL|panic|assertion failed|Input/output error' \
	"$logical_log"; then
	echo "fatal guest marker observed" >&2
	exit 1
fi
[[ -s $screen ]] || { echo "QEMU screendump was not produced" >&2; exit 1; }
"$checker" "$screen" >"$screen_report"
printf 'NOCT-T011\tpass\tbeui-screen.ppm, beui-screen-report.txt\n' >>"$results"
printf 'NOCT-T012\tpass\tguest-logical.log\n' >>"$results"

[[ $(hash_file "$production_config") == "$production_config_hash" ]] || {
	echo "production config changed during acceptance" >&2
	exit 1
}
{
	printf 'production_image_sha256=%s\n' "$(hash_file "$production_image")"
	printf 'screenshot_sha256=%s\n' "$(hash_file "$screen")"
	printf 'end_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'acceptance_exit_status=0\n'
} >>"$metadata"
rm -f -- "$run_image"
echo "WS008 NOCT-T011/T012/T013 acceptance: PASS ($output)"

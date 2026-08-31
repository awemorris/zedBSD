#!/usr/bin/env bash
# WS005 p003 direct AF_UNIX SO_PEERCRED QEMU acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
makefile=$script_dir/peercred-native-qemu.mk
source_image=$repo/build/pc98/peercred-native-hdd-image.img
qemu=${QEMU_PC98:-$repo/build/qemu-pc98/build/qemu-system-i386}
build_timeout=${BUILD_TIMEOUT_SECONDS:-1800}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-60}
cell_timeout=${CELL_TIMEOUT_SECONDS:-240}
key_delay=${KEY_DELAY_SECONDS:-0.04}

usage()
{
	cat <<EOF
usage: $0 [OUTPUT-DIRECTORY]

Builds a test-only PC-98 image and directly exercises AF_UNIX SO_PEERCRED in
the zedBSD guest. The probe covers pathname listen/connect/accept snapshots,
stream socketpair, strict short/large lengths, state/family errors, delayed
accept after credential changes and peer exit, and SCM_RIGHTS preservation.
It also checks datagram-socketpair reconnect rejection, listener-close resource
cleanup, and live networkd pathname admission and SHOW-only authorization.
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
for command in awk cc cp date find make mktemp rg sha256sum sleep timeout tr; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done
[[ -x $qemu ]] || { echo "qemu-pc98 not found: $qemu" >&2; exit 2; }

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
	' "$repo/config.mk"
}

[[ $(config_value ZEDBSD_PLATFORM) == pc98 &&
    $(config_value ZEDBSD_ARCHITECTURE) == i386 &&
    $(config_value ZEDBSD_BOARD) == pc98 ]] || {
	echo "peercred native QEMU test requires the PC-98 config.mk" >&2
	exit 2
}

if [[ $# -eq 1 ]]; then
	output=$1
	if [[ -e $output && -n $(find "$output" -mindepth 1 -print -quit) ]]; then
		echo "output directory is not empty: $output" >&2
		exit 2
	fi
	mkdir -p -- "$output"
else
	temp_root=$repo/plan/ws005-networking/temp
	mkdir -p -- "$temp_root"
	output=$(mktemp -d "$temp_root/peercred-native.XXXXXX")
fi
output=$(cd -- "$output" && pwd)
temp_root=$repo/plan/ws005-networking/temp
mkdir -p -- "$temp_root"
temporary=$(mktemp -d "$temp_root/peercred-native-run.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

build_log=$output/build.log
guest_log=$output/guest.log
logical_log=$output/guest-logical.log
qemu_log=$output/qemu.log
metadata=$output/metadata.txt
result_file=$output/controller-result.txt
run_image=$temporary/run.img
decoder=$temporary/pc98-vram-decoder
vram=$temporary/vram.bin
screen=$output/final-screen.txt
readiness_screen=$output/readiness-screen.txt
config_hash=$(sha256sum "$repo/config.mk" | awk '{print $1}')

: >"$build_log"
: >"$guest_log"
: >"$qemu_log"
: >"$result_file"
{
	printf 'test=WS005 peercred native\n'
	printf 'start_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'repository=%s\n' "$repo"
	printf 'config_sha256_before=%s\n' "$config_hash"
	printf 'qemu=%s\n' "$qemu"
} >"$metadata"

set +e
timeout --foreground --kill-after=10 "${build_timeout}s" \
	make -C "$repo" -j16 -f Makefile -f "$makefile" \
	ws005-peercred-native-qemu-image >"$build_log" 2>&1
build_status=$?
set -e
if [[ $build_status -ne 0 || ! -s $source_image ]]; then
	echo "peercred test image build failed (status $build_status)" >&2
	exit 1
fi
cp --reflink=auto --sparse=always "$source_image" "$run_image"
source_hash=$(sha256sum "$source_image" | awk '{print $1}')
cc -std=c11 -Wall -Wextra -Werror \
	"$repo/plan/ws003-bringup/tests/boot-parameter-image-tool.c" \
	-o "$decoder"

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
	while :; do
		count=$(marker_count "$pattern" "$file")
		((count >= minimum)) && return 0
		(( $(date +%s) >= deadline )) && return 1
		sleep 0.1
	done
}

capture_screen()
{
	local index=0

	rm -f -- "$vram" "$screen"
	printf 'pmemsave 0xa0000 0x2000 "%s"\n' "$vram"
	while ((index < 100)); do
		[[ -s $vram ]] && break
		sleep 0.02
		((index += 1))
	done
	[[ -s $vram ]] || return 1
	"$decoder" decode-pc98-vram "$vram" >"$screen"
}

wait_for_screen()
{
	local pattern=$1 seconds=$2 deadline

	deadline=$(( $(date +%s) + 10#$seconds ))
	while :; do
		capture_screen && rg -q -- "$pattern" "$screen" && return 0
		(( $(date +%s) >= deadline )) && return 1
		sleep 0.25
	done
}

send_text()
{
	local text=$1 character key

	while [[ -n $text ]]; do
		character=${text::1}
		case $character in
		[a-z0-9]) key=$character ;;
		*) echo "unsupported sendkey character: $character" >&2; return 1 ;;
		esac
		printf 'sendkey %s\n' "$key"
		text=${text:1}
		sleep "$key_delay"
	done
	printf 'sendkey ret\n'
}

shell_prompt='root@[^[:space:]]*:[^$]*\$([[:blank:]]|$)'
login_prompt='(^|[[:blank:]])login:[[:blank:]]*$'
password_prompt='Password:'

controller()
{
	trap '' PIPE
	set +e
	if ! wait_for_screen 'login:' "$boot_timeout"; then
		echo login-timeout >"$result_file"
		printf 'quit\n'
		return 1
	fi
	send_text root
	if ! wait_for_screen "$password_prompt" "$command_timeout"; then
		echo password-timeout >"$result_file"
		printf 'quit\n'
		return 1
	fi
	printf 'sendkey ret\n'
	if ! wait_for_screen 'root@zedbsd:' "$command_timeout"; then
		echo shell-timeout >"$result_file"
		printf 'quit\n'
		return 1
	fi
	if ! capture_screen; then
		echo readiness-capture-failed >"$result_file"
		printf 'quit\n'
		return 1
	fi
	cp -- "$screen" "$readiness_screen"
	send_text peercred
	if ! wait_for_screen 'PEERCRED-NATIVE: PASS' "$command_timeout"; then
		echo probe-timeout >"$result_file"
		printf 'quit\n'
		return 1
	fi
	echo pass >"$result_file"
	printf 'quit\n'
}

set +e
controller |
	timeout --foreground --kill-after=5 "${cell_timeout}s" \
	"$qemu" -M 'pc9821,pegc=off,coregraph=on' -cpu 486 -m 64M -smp 1 \
	-drive "if=ide,bus=0,unit=0,format=raw,file=$run_image" \
	-display none -serial none -debugcon "file:$guest_log" \
	-monitor stdio -no-reboot >"$qemu_log" 2>&1
statuses=("${PIPESTATUS[@]}")
set -e
if [[ ${statuses[0]} -ne 0 || ${statuses[1]} -ne 0 ||
    ! -s $result_file || $(<"$result_file") != pass ]]; then
	echo "peercred QEMU/controller failure: ${statuses[*]} "\
"($(<"$result_file" 2>/dev/null || echo no-result))" >&2
	exit 1
fi

tr -d '\r' <"$guest_log" >"$logical_log"
rg -q 'PEERCRED-NATIVE: PASS' "$screen" || {
	echo "peercred success marker missing from final screen" >&2; exit 1;
}
rg -q 'init: started networkd pid' "$readiness_screen" || {
	echo "networkd did not reach its ready notification" >&2; exit 1;
}
rg -q 'init: system running' "$readiness_screen" || {
	echo "init did not reach the running state" >&2; exit 1;
}
if rg -q 'networkd exited before readiness|Segmentation fault' \
	"$readiness_screen" "$screen"; then
	echo "networkd failed during peercred acceptance" >&2
	exit 1
fi
if rg -a -q 'PEERCRED-NATIVE: FAIL|fatal:|kernel panic|panic:| fault v=|VFS initialization failed' \
	"$logical_log"; then
	echo "peercred guest log contains a failure" >&2
	exit 1
fi
[[ $(sha256sum "$source_image" | awk '{print $1}') == "$source_hash" ]]
[[ $(sha256sum "$repo/config.mk" | awk '{print $1}') == "$config_hash" ]]
{
	printf 'end_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'source_image_sha256=%s\n' "$source_hash"
	printf 'config_sha256_after=%s\n' "$config_hash"
	printf 'controller_status=%s\n' "${statuses[0]}"
	printf 'qemu_status=%s\n' "${statuses[1]}"
} >>"$metadata"
echo "WS005 peercred native QEMU: PASS ($output)"

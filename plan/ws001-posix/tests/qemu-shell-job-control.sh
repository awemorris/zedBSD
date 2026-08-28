#!/usr/bin/env bash
# WS001-p014 production amd64 shell job-control QEMU acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
image=$repo/build/amd64/hdd-image.img
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-30}
cell_timeout=${CELL_TIMEOUT_SECONDS:-300}
key_delay=${KEY_DELAY_SECONDS:-0.015}

if [[ $# -gt 1 ]]; then
	echo "usage: $0 [OUTPUT-DIRECTORY]" >&2
	exit 2
fi
for command in "$qemu" awk cp date rg sed sha256sum sleep timeout tr; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done
[[ -f $image ]] || {
	echo "production image not found: $image" >&2
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
	mkdir -p -- "$repo/plan/ws001-posix/temp"
	output=$(mktemp -d \
		"$repo/plan/ws001-posix/temp/q023-p014-qemu.XXXXXX")
fi
output=$(cd -- "$output" && pwd)

run_image=$output/run.img
guest_log=$output/guest.log
logical_log=$output/guest-logical.log
qemu_log=$output/qemu.log
metadata=$output/run-metadata.txt
results=$output/results.tsv
base_hash=$(sha256sum "$image" | awk '{print $1}')
cp --reflink=auto --sparse=always "$image" "$run_image"
: >"$guest_log"
: >"$qemu_log"

qemu_version=$("$qemu" --version | sed -n '1p')
{
	printf 'date_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'qemu=%s\n' "$qemu_version"
	printf 'base_image=%s\n' "$image"
	printf 'base_sha256=%s\n' "$base_hash"
	printf 'boot_timeout_seconds=%s\n' "$boot_timeout"
	printf 'command_timeout_seconds=%s\n' "$command_timeout"
	printf 'cell_timeout_seconds=%s\n' "$cell_timeout"
} >"$metadata"

controller_deadline=0

marker_count()
{
	local pattern=$1 file=$2 count

	count=$(rg -a -c -- "$pattern" "$file" 2>/dev/null || true)
	printf '%s\n' "${count:-0}"
}

wait_for_pattern()
{
	local pattern=$1 file=$2 timeout_value=$3 minimum=${4:-1}
	local count deadline

	deadline=$(( $(date +%s) + 10#$timeout_value ))
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

send_key()
{
	printf 'sendkey %s\n' "$1"
	sleep "$key_delay"
}

send_line()
{
	local text=$1 character key index

	for ((index = 0; index < ${#text}; index++)); do
		character=${text:index:1}
		case $character in
		' ') key=spc ;;
		/) key=slash ;;
		-) key=minus ;;
		.) key=dot ;;
		_) key=shift-minus ;;
		'|') key=shift-backslash ;;
		'&') key=shift-7 ;;
		[a-z0-9]) key=$character ;;
		*)
			echo "unsupported sendkey character: $character" >&2
			return 1
			;;
		esac
		send_key "$key"
	done
	send_key ret
}

shell_prompt='root@[^[:space:]]*:[^$]*\$([[:blank:]]|$)'
login_prompt='(^|[[:blank:]])login:[[:blank:]]*$'
password_prompt='Password:'

login_guest()
{
	local password_before shell_before

	password_before=$(marker_count "$password_prompt" "$guest_log")
	shell_before=$(marker_count "$shell_prompt" "$guest_log")
	send_line root || return 1
	wait_for_pattern "$password_prompt" "$guest_log" "$command_timeout" \
		$((password_before + 1)) || return 1
	send_line '' || return 1
	wait_for_pattern "$shell_prompt" "$guest_log" "$command_timeout" \
		$((shell_before + 1))
}

send_shell()
{
	local text=$1 before

	before=$(marker_count "$shell_prompt" "$guest_log")
	send_line "$text" || return 1
	wait_for_pattern "$shell_prompt" "$guest_log" "$command_timeout" \
		$((before + 1))
}

run_foreground_reader()
{
	local command=$1 marker=$2 marker_pattern prompt_before marker_before
	local marker_after

	prompt_before=$(marker_count "$shell_prompt" "$guest_log")
	marker_pattern="^${marker}\\r?$"
	marker_before=$(marker_count "$marker_pattern" "$guest_log")
	send_line "$command" || return 1
	sleep 0.2
	send_line "$marker" || return 1
	# A successful reader produces two exact lines: the terminal's input echo
	# and the command's copied output.  Requiring both prevents a stopped or
	# missing reader from passing merely because the outer shell later sees the
	# marker as a command.
	wait_for_pattern "$marker_pattern" "$guest_log" "$command_timeout" \
		$((marker_before + 2)) || return 1
	wait_for_pattern "$shell_prompt" "$guest_log" "$command_timeout" \
		$((prompt_before + 1)) || return 1
	marker_after=$(marker_count "$marker_pattern" "$guest_log")
	[[ $marker_after -eq $((marker_before + 2)) ]]
}

controller_body()
{
	local prompt_before stopped_before

	wait_for_pattern "$login_prompt" "$guest_log" "$boot_timeout" || return 1
	login_guest || return 1
	send_shell uname || return 1

	run_foreground_reader '/bin/head -n 1 | /bin/cat' p014pipeline || \
		return 1

	prompt_before=$(marker_count "$shell_prompt" "$guest_log")
	stopped_before=$(marker_count '\[[0-9]+\] stopped' "$guest_log")
	send_line '/bin/head -n 1' || return 1
	sleep 0.2
	send_key ctrl-z || return 1
	wait_for_pattern '\[[0-9]+\] stopped' "$guest_log" "$command_timeout" \
		$((stopped_before + 1)) || return 1
	wait_for_pattern "$shell_prompt" "$guest_log" "$command_timeout" \
		$((prompt_before + 1)) || return 1
	run_foreground_reader fg p014fg || return 1

	prompt_before=$(marker_count "$shell_prompt" "$guest_log")
	send_line '/bin/head -n 1 &' || return 1
	wait_for_pattern "$shell_prompt" "$guest_log" "$command_timeout" \
		$((prompt_before + 1)) || return 1
	send_shell jobs || return 1
	run_foreground_reader fg p014background || return 1

	# Use -c so the non-TTY shell consumes the pipe as command stdin without
	# entering its readline loop.  A bare `sh` emits inner prompts before the
	# outer shell regains the terminal, which can make prompt-based monitor
	# synchronization race with the next injected command.
	send_shell 'echo p014nontty | sh -c /bin/head' || return 1
	wait_for_pattern '^p014nontty\r?$' "$guest_log" "$command_timeout" || \
		return 1
	send_shell 'echo p014complete' || return 1
	printf 'quit\n'
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
		printf 'quit\n'
	fi
	return "$status"
}

set +e
controller | timeout --foreground --kill-after=5 "${cell_timeout}s" \
	"$qemu" -machine pc -m 512 -smp 4 \
	-drive "file=$run_image,format=raw,if=ide" \
	-display none -serial none -debugcon "file:$guest_log" -monitor stdio \
	>"$qemu_log" 2>&1
statuses=("${PIPESTATUS[@]}")
set -e
controller_status=${statuses[0]}
qemu_status=${statuses[1]}
printf 'controller_status=%s\nqemu_status=%s\n' "$controller_status" \
	"$qemu_status" >>"$metadata"
if [[ $controller_status -ne 0 || $qemu_status -ne 0 ]]; then
	echo "QEMU cell failed: controller=$controller_status qemu=$qemu_status" >&2
	exit 1
fi

tr -d '\r' <"$guest_log" >"$logical_log"
for marker in p014pipeline p014fg p014background; do
	[[ $(marker_count "^${marker}$" "$logical_log") -eq 2 ]] || {
		echo "reader marker must have one input echo and one output: $marker" >&2
		exit 1
	}
done
for marker in p014nontty p014complete; do
	[[ $(marker_count "^${marker}$" "$logical_log") -eq 1 ]] || {
		echo "guest marker must occur exactly once: $marker" >&2
		exit 1
	}
done
[[ $(marker_count '\[[0-9]+\] stopped' "$logical_log") -eq 1 ]]
rg -a -q '\[[0-9]+\] active or stopped' "$logical_log"
if rg -a -q 'sh: p014(pipeline|fg|background): not found|usage: (head|cat)' \
	"$logical_log"; then
	echo "reader input escaped into the shell or the reader rejected stdin" >&2
	exit 1
fi

fatal_pattern='fatal:|FATAL:|kernel panic|panic:|amd64 fault v=|assert(ion)? failed|Segmentation fault|double free|heap corruption|VFS initialization failed|usb-storage: BOT .*error=[1-9]|usb-storage: sd[a-z]+ .*error=[1-9]|loop[0-9]+: write .*error=[1-9]'
qemu_fatal_pattern='unknown command|invalid command|invalid parameter|qemu-system-[^:]+:.*([Ee]rror|[Ff]ailed|Could not|cannot)|[Aa]ssertion .* failed|Segmentation fault|Aborted'
if rg -a -n -- "$fatal_pattern" "$guest_log" >"$output/guest-fatal-scan.txt"; then
	echo "fatal guest diagnostic found" >&2
	cat "$output/guest-fatal-scan.txt" >&2
	exit 1
fi
if rg -a -n -- "$qemu_fatal_pattern" "$qemu_log" \
	>"$output/qemu-fatal-scan.txt"; then
	echo "QEMU monitor diagnostic found" >&2
	cat "$output/qemu-fatal-scan.txt" >&2
	exit 1
fi
: >"$output/guest-fatal-scan.txt"
: >"$output/qemu-fatal-scan.txt"

current_hash=$(sha256sum "$image" | awk '{print $1}')
[[ $current_hash == "$base_hash" ]] || {
	echo "production input image changed during acceptance" >&2
	exit 1
}
rm -f -- "$run_image"
{
	printf 'test\tresult\tevidence\n'
	printf 'foreground-pipeline\tpass\tguest-logical.log\n'
	printf 'ctrl-z-fg\tpass\tguest-logical.log\n'
	printf 'background-reader-fg\tpass\tguest-logical.log\n'
	printf 'non-tty-inner-shell\tpass\tguest-logical.log\n'
	printf 'fatal-scans\tpass\tguest-fatal-scan.txt,qemu-fatal-scan.txt\n'
	printf 'input-integrity\tpass\trun-metadata.txt\n'
} >"$results"
printf 'base_sha256_after=%s\n' "$current_hash" >>"$metadata"
echo "WS001-p014 amd64 QEMU acceptance: PASS ($output)"

#!/usr/bin/env bash
# Exercise the Agent 2 Queue utilities in a disposable amd64 guest image.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
image=$repo/build/amd64/hdd-image.img
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-30}
cell_timeout=${CELL_TIMEOUT_SECONDS:-240}

if [[ $# -gt 1 ]]; then
	echo "usage: $0 [OUTPUT-DIRECTORY]" >&2
	exit 2
fi
for command in "$qemu" awk cp grep sha256sum sleep timeout tr; do
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
	output=$(mktemp -d "$repo/plan/ws001-posix/temp/agent2-q001.XXXXXX")
fi
output=$(cd -- "$output" && pwd)

run_image=$output/run.img
guest_log=$output/guest.log
logical_log=$output/guest-logical.log
qemu_log=$output/qemu.log
base_hash=$(sha256sum "$image" | awk '{print $1}')
cp --reflink=auto --sparse=always "$image" "$run_image"
: >"$guest_log"
: >"$qemu_log"

marker_count()
{
	local pattern=$1
	local count

	count=$(grep -a -E -c -- "$pattern" "$guest_log" 2>/dev/null || true)
	printf '%s\n' "${count:-0}"
}

wait_for_pattern()
{
	local pattern=$1 timeout_value=$2 minimum=${3:-1}
	local deadline count

	deadline=$((SECONDS + timeout_value))
	while ((SECONDS < deadline)); do
		count=$(marker_count "$pattern")
		if ((count >= minimum)); then
			return 0
		fi
		sleep 0.1
	done

	return 1
}

send_key()
{
	printf 'sendkey %s\n' "$1"
	sleep 0.015
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

send_shell()
{
	local text=$1 before

	before=$(marker_count "$shell_prompt")
	send_line "$text"
	wait_for_pattern "$shell_prompt" "$command_timeout" $((before + 1))
}

controller()
{
	local before

	trap '' PIPE
	wait_for_pattern 'login:[[:blank:]]*$' "$boot_timeout"
	send_line root
	wait_for_pattern 'Password:' "$command_timeout"
	send_line ''
	wait_for_pattern "$shell_prompt" "$command_timeout"

	before=$(marker_count '^p017guest\r?$')
	send_shell 'cmp /bin/cmp /bin/cmp && echo p017guest'
	wait_for_pattern '^p017guest\r?$' "$command_timeout" $((before + 1))

	send_shell 'echo p018data | tee /tmp/p018'
	before=$(marker_count '^p018guest\r?$')
	send_shell 'grep p018data /tmp/p018 && echo p018guest'
	wait_for_pattern '^p018guest\r?$' "$command_timeout" $((before + 1))

	before=$(marker_count '^p016guest\r?$')
	send_shell 'lp -d invalid /bin/cmp || echo p016guest'
	wait_for_pattern '^p016guest\r?$' "$command_timeout" $((before + 1))

	printf 'quit\n'
}

set +e
controller | timeout --foreground --kill-after=5 "${cell_timeout}s" \
	"$qemu" -machine pc -m 512 -smp 4 \
	-drive "file=$run_image,format=raw,if=ide" \
	-display none -serial none -debugcon "file:$guest_log" -monitor stdio \
	>"$qemu_log" 2>&1
statuses=("${PIPESTATUS[@]}")
set -e
if [[ ${statuses[0]} -ne 0 || ${statuses[1]} -ne 0 ]]; then
	echo "QEMU smoke failed: controller=${statuses[0]} qemu=${statuses[1]}" >&2
	exit 1
fi

tr -d '\r' <"$guest_log" >"$logical_log"
for marker in p016guest p017guest p018guest; do
	if [[ $(grep -a -E -c "^${marker}$" "$logical_log") -ne 1 ]]; then
		echo "guest marker count is not one: $marker" >&2
		exit 1
	fi
done
if grep -a -E -q 'fatal:|kernel panic|panic:|VFS initialization failed' \
	"$logical_log"; then
	echo "guest reported a fatal boot/runtime failure" >&2
	exit 1
fi
if [[ $(sha256sum "$image" | awk '{print $1}') != "$base_hash" ]]; then
	echo "production image changed during disposable smoke" >&2
	exit 1
fi

echo "AGENT2-Q001 amd64 utility smoke: PASS ($output)"

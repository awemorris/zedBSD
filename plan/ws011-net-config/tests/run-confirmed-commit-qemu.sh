#!/usr/bin/env bash
# WS011 NCOM-T020/T021 production confirmed-commit QEMU acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
makefile=$script_dir/confirmed-commit-qemu.mk
build_config=$script_dir/confirmed-commit-qemu-config.mk
source_image=$repo/build/amd64/tests/ws011-p007-confirmed.img
production_image=$repo/build/amd64/hdd-image.img
production_config=$repo/config.mk
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
build_timeout=${BUILD_TIMEOUT_SECONDS:-1800}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-30}
rollback_wait=${ROLLBACK_WAIT_SECONDS:-70}
cell_timeout=${CELL_TIMEOUT_SECONDS:-420}
key_delay=${KEY_DELAY_SECONDS:-0.015}
cell_selection=${NCOM_CELL_SELECTION:-both}
synthetic_mac=52:54:00:11:07:01
old_address=10.0.2.15
timeout_address=10.0.2.16
confirm_address=10.0.2.17

usage()
{
	cat <<EOF
usage: $0 [OUTPUT-DIRECTORY]

Builds one test-only amd64/PC-AT NE2000 image and runs exactly two fresh QEMU
cells: NCOM-T020 timeout/client-loss restoration and NCOM-T021 same-session
confirmation, late-timer absence, and reboot persistence.
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
	"$rollback_wait" "$cell_timeout"; do
	[[ $value =~ ^[1-9][0-9]{0,8}$ ]] || {
		echo "timeouts must be positive integers of at most 9 digits" >&2
		exit 2
	}
done
if ((rollback_wait <= 60)); then
	echo "ROLLBACK_WAIT_SECONDS must exceed the real one-minute deadline" >&2
	exit 2
fi
case $key_delay in
''|*[!0-9.]*|.*|*.*.*)
	echo "KEY_DELAY_SECONDS must be a non-negative decimal" >&2
	exit 2
	;;
esac
case $cell_selection in
both|t020|t021) ;;
*)
	echo "NCOM_CELL_SELECTION must be both, t020, or t021" >&2
	exit 2
	;;
esac
for command in "$qemu" awk cp date git make mktemp rg sed sha256sum sleep sort \
	timeout tr; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done
[[ -f $build_config && -f $makefile ]] || {
	echo "phase-owned build fixtures are missing" >&2
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
	mkdir -p -- "$repo/plan/ws011-net-config/temp"
	output=$(mktemp -d "$repo/plan/ws011-net-config/temp/q074.XXXXXX")
fi
output=$(cd -- "$output" && pwd)
temporary=$(mktemp -d "$repo/plan/ws011-net-config/temp/q074-run.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

build_log=$output/build.log
metadata=$output/run-metadata.txt
results=$output/results.tsv
timeout_guest=$output/ncom-t020-guest.log
timeout_logical=$output/ncom-t020-guest-logical.log
timeout_qemu=$output/ncom-t020-qemu.log
confirm_guest=$output/ncom-t021-guest.log
confirm_logical=$output/ncom-t021-guest-logical.log
confirm_qemu=$output/ncom-t021-qemu.log

path_hash()
{
	local path=$1

	if [[ -f $path ]]; then
		sha256sum "$path" | awk '{print $1}'
	else
		printf '%s\n' absent
	fi
}

tracked_digest()
{
	(
		cd -- "$repo"
		git ls-files -z | sort -z |
			while IFS= read -r -d '' tracked; do
				if [[ -f $tracked ]]; then
					sha256sum "$tracked"
				fi
			done |
			sha256sum | awk '{print $1}'
	)
}

tracked_before=$(tracked_digest)
config_before=$(path_hash "$production_config")
production_image_before=$(path_hash "$production_image")
build_config_before=$(path_hash "$build_config")
qemu_version=$("$qemu" --version | sed -n '1p')
: >"$build_log"
: >"$timeout_guest"
: >"$timeout_qemu"
: >"$confirm_guest"
: >"$confirm_qemu"
printf 'case\tresult\tevidence\n' >"$results"
{
	printf 'test=WS011 confirmed-commit automatic acceptance\n'
	printf 'start_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'repository=%s\n' "$repo"
	printf 'qemu=%s\n' "$qemu_version"
	printf 'qemu_machine=pc\n'
	printf 'qemu_network=user,restrict=on\n'
	printf 'qemu_device=ne2k_isa,iobase=0x300,irq=10,mac=%s\n' "$synthetic_mac"
	printf '%s\n' 'qemu_command=qemu-system-x86_64 -machine pc -m 512 -smp 4 -drive file=DISPOSABLE.img,format=raw,if=ide -netdev user,id=net0,restrict=on -device ne2k_isa,netdev=net0,iobase=0x300,irq=10,mac=52:54:00:11:07:01 -display none -serial none -debugcon file=GUEST.log -monitor stdio'
	printf 'old_address=%s/24\n' "$old_address"
	printf 'timeout_address=%s/24\n' "$timeout_address"
	printf 'confirm_address=%s/24\n' "$confirm_address"
	printf 'gateway=10.0.2.2\n'
	printf 'dns=10.0.2.3\n'
	printf 'build_timeout_seconds=%s\n' "$build_timeout"
	printf 'boot_timeout_seconds=%s\n' "$boot_timeout"
	printf 'command_timeout_seconds=%s\n' "$command_timeout"
	printf 'rollback_wait_seconds=%s\n' "$rollback_wait"
	printf 'cell_timeout_seconds=%s\n' "$cell_timeout"
	printf 'cell_selection=%s\n' "$cell_selection"
	printf 'tracked_tree_sha256_before=%s\n' "$tracked_before"
	printf 'config_sha256_before=%s\n' "$config_before"
	printf 'production_image_sha256_before=%s\n' "$production_image_before"
	printf 'test_config_sha256_before=%s\n' "$build_config_before"
} >"$metadata"

set +e
timeout --foreground --kill-after=10 "${build_timeout}s" \
	make -C "$repo" -j16 -f Makefile -f "$makefile" \
	ZEDBSD_CONFIG="$build_config" ws011-p007-qemu-image \
	>"$build_log" 2>&1
build_status=$?
set -e
if [[ $build_status -ne 0 || ! -s $source_image ]]; then
	echo "WS011 p007 test image build failed (status $build_status)" >&2
	exit 1
fi
source_hash=$(path_hash "$source_image")
printf 'source_image=%s\nsource_image_sha256=%s\n' \
	"$source_image" "$source_hash" >>"$metadata"

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

send_text()
{
	local text=$1 character key index

	for ((index = 0; index < ${#text}; index++)); do
		character=${text:index:1}
		case $character in
		' ') key=spc ;;
		/) key=slash ;;
		-) key=minus ;;
		.) key=dot ;;
		:) key=shift-semicolon ;;
		[a-z0-9]) key=$character ;;
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
operational_prompt='net> '
configuration_prompt='net\(config\)> '
interface_prompt='net\(config-if:ne0\)> '
discard_prompt='Discard unsaved changes\? \[y/N\] '

login_guest()
{
	local log=$1 login_number=$2 password_before shell_before

	wait_for_pattern "$login_prompt" "$log" "$boot_timeout" "$login_number" ||
		return 1
	password_before=$(marker_count "$password_prompt" "$log")
	shell_before=$(marker_count "$shell_prompt" "$log")
	send_text root || return 1
	wait_for_pattern "$password_prompt" "$log" "$command_timeout" \
		$((password_before + 1)) || return 1
	send_text '' || return 1
	wait_for_pattern "$shell_prompt" "$log" "$command_timeout" \
		$((shell_before + 1))
}

send_shell()
{
	local log=$1 text=$2 before

	before=$(marker_count "$shell_prompt" "$log")
	send_text "$text" || return 1
	wait_for_pattern "$shell_prompt" "$log" "$command_timeout" \
		$((before + 1))
}

shell_marker()
{
	local log=$1 marker=$2

	send_shell "$log" "echo $marker" || return 1
	wait_for_pattern "^${marker}\\r?$" "$log" "$command_timeout"
}

send_net()
{
	local log=$1 text=$2 prompt=$3 before

	before=$(marker_count "$prompt" "$log")
	send_text "$text" || return 1
	wait_for_pattern "$prompt" "$log" "$command_timeout" $((before + 1))
}

enter_net()
{
	local log=$1 before

	before=$(marker_count "$operational_prompt" "$log")
	send_text net || return 1
	wait_for_pattern "$operational_prompt" "$log" "$command_timeout" \
		$((before + 1))
}

observe_state()
{
	local log=$1 marker=$2

	shell_marker "$log" "$marker-begin" || return 1
	send_shell "$log" 'cksum /etc/net.conf' || return 1
	send_shell "$log" 'ifconfig ne0' || return 1
	send_shell "$log" route || return 1
	send_shell "$log" 'cat /etc/resolv.conf' || return 1
	send_shell "$log" 'ping -c 1 10.0.2.2' || return 1
	shell_marker "$log" "$marker-end"
}

change_and_arm()
{
	local log=$1 address=$2 before

	enter_net "$log" || return 1
	send_net "$log" configure "$configuration_prompt" || return 1
	send_net "$log" 'interface ne0' "$interface_prompt" || return 1
	send_net "$log" "static ipv4 $address prefix-length 24" \
		"$interface_prompt" || return 1
	send_net "$log" exit "$configuration_prompt" || return 1
	before=$(marker_count 'Confirmed commit applied; rollback is armed for 1 minute\.' "$log")
	send_net "$log" 'commit confirmed 1' "$configuration_prompt" || return 1
	wait_for_pattern 'Confirmed commit applied; rollback is armed for 1 minute\.' \
		"$log" "$command_timeout" $((before + 1))
}

leave_pending_net()
{
	local log=$1 before

	send_net "$log" end "$operational_prompt" || return 1
	send_net "$log" 'show running-config' "$operational_prompt" || return 1
	before=$(marker_count "$discard_prompt" "$log")
	send_text exit || return 1
	wait_for_pattern "$discard_prompt" "$log" "$command_timeout" \
		$((before + 1)) || return 1
	before=$(marker_count "$shell_prompt" "$log")
	send_text y || return 1
	wait_for_pattern "$shell_prompt" "$log" "$command_timeout" \
		$((before + 1))
}

confirm_and_leave_net()
{
	local log=$1 before

	before=$(marker_count 'Commit complete\.' "$log")
	send_net "$log" commit "$configuration_prompt" || return 1
	wait_for_pattern 'Commit complete\.' "$log" "$command_timeout" \
		$((before + 1)) || return 1
	send_net "$log" end "$operational_prompt" || return 1
	before=$(marker_count "$shell_prompt" "$log")
	send_text exit || return 1
	wait_for_pattern "$shell_prompt" "$log" "$command_timeout" \
		$((before + 1))
}

timeout_controller()
{
	local log=$1

	trap '' PIPE
	login_guest "$log" 1 || return 1
	observe_state "$log" ncom-t020-old || return 1
	change_and_arm "$log" "$timeout_address" || return 1
	leave_pending_net "$log" || return 1
	observe_state "$log" ncom-t020-temporary || return 1
	sleep "$rollback_wait"
	observe_state "$log" ncom-t020-restored || return 1
}

confirm_controller()
{
	local log=$1 login_before shell_before

	trap '' PIPE
	login_guest "$log" 1 || return 1
	observe_state "$log" ncom-t021-old || return 1
	change_and_arm "$log" "$confirm_address" || return 1
	send_net "$log" 'show startup-config' "$configuration_prompt" || return 1
	confirm_and_leave_net "$log" || return 1
	observe_state "$log" ncom-t021-confirmed || return 1
	sleep "$rollback_wait"
	observe_state "$log" ncom-t021-after-deadline || return 1
	login_before=$(marker_count "$login_prompt" "$log")
	shell_before=$(marker_count "$shell_prompt" "$log")
	send_text reboot || return 1
	wait_for_pattern "$login_prompt" "$log" "$boot_timeout" \
		$((login_before + 1)) || return 1
	login_guest "$log" $((login_before + 1)) || return 1
	if (( $(marker_count "$shell_prompt" "$log") <= shell_before )); then
		return 1
	fi
	observe_state "$log" ncom-t021-rebooted || return 1
}

run_cell()
{
	local name=$1 controller=$2 guest=$3 qemu_log=$4
	local image statuses

	image=$temporary/$name.img

	cp --reflink=auto --sparse=always "$source_image" "$image"
	set +e
	controller_wrapper "$controller" "$guest" |
		timeout --foreground --kill-after=5 "${cell_timeout}s" \
		"$qemu" -machine pc -m 512 -smp 4 \
		-drive "file=$image,format=raw,if=ide" \
		-netdev user,id=net0,restrict=on \
		-device "ne2k_isa,netdev=net0,iobase=0x300,irq=10,mac=$synthetic_mac" \
		-display none -serial none -debugcon "file:$guest" \
		-monitor stdio >"$qemu_log" 2>&1
	statuses=("${PIPESTATUS[@]}")
	set -e
	if [[ ${statuses[0]} -ne 0 || ${statuses[1]} -ne 0 ]]; then
		echo "$name failed: controller=${statuses[0]} qemu=${statuses[1]}" >&2
		return 1
	fi
}

controller_wrapper()
{
	local controller=$1 guest=$2 status

	set +e
	"$controller" "$guest"
	status=$?
	printf 'quit\n'
	return "$status"
}

extract_section()
{
	local input=$1 begin=$2 end=$3 output_file=$4

	awk -v begin="$begin" -v end="$end" '
		$0 == begin { active = 1; next }
		$0 == end { if (active) exit }
		active { print }
	' "$input" >"$output_file"
}

require_state()
{
	local logical=$1 marker=$2 address=$3 section

	section=$output/$marker.log

	extract_section "$logical" "$marker-begin" "$marker-end" "$section"
	rg -q "inet $address netmask 255\\.255\\.255\\.0" "$section" || {
		echo "$marker missing address $address" >&2; return 1;
	}
	rg -q '^default[[:blank:]]+10\\.0\\.2\\.2[[:blank:]]+UG' "$section" || {
		echo "$marker missing default route" >&2; return 1;
	}
	rg -q '^nameserver 10\\.0\\.2\\.3$' "$section" || {
		echo "$marker missing resolver" >&2; return 1;
	}
	rg -q '^1 packets transmitted, 1 packets received, 0% packet loss$' \
		"$section" || {
		echo "$marker missing successful gateway ping" >&2; return 1;
	}
}

validate_no_fatal()
{
	local guest=$1 qemu_log=$2 label=$3
	local guest_pattern='fatal:|FATAL:|kernel panic|panic:|amd64 fault v=|assert(ion)? failed|Segmentation fault|VFS initialization failed|networkd exited before readiness|readiness timeout'
	local qemu_pattern='unknown command|invalid parameter|qemu-system-[^:]+:.*([Ee]rror|[Ff]ailed|Could not|cannot)|[Aa]ssertion .* failed|Segmentation fault|Aborted'

	if rg -a -i -q -- "$guest_pattern" "$guest"; then
		echo "$label guest fatal diagnostic found" >&2
		rg -a -i -n -- "$guest_pattern" "$guest" >&2 || true
		return 1
	fi
	if rg -a -i -q -- "$qemu_pattern" "$qemu_log"; then
		echo "$label QEMU diagnostic found" >&2
		rg -a -i -n -- "$qemu_pattern" "$qemu_log" >&2 || true
		return 1
	fi
}

if [[ $cell_selection != t021 ]]; then
	run_cell ncom-t020 timeout_controller "$timeout_guest" "$timeout_qemu"
	tr -d '\r' <"$timeout_guest" >"$timeout_logical"
	require_state "$timeout_logical" ncom-t020-old "$old_address"
	require_state "$timeout_logical" ncom-t020-temporary "$timeout_address"
	require_state "$timeout_logical" ncom-t020-restored "$old_address"
	mapfile -t timeout_sums < <(rg '^[0-9]+[[:blank:]]+[0-9]+[[:blank:]]+/etc/net.conf$' "$timeout_logical")
	if [[ ${#timeout_sums[@]} -ne 3 ||
	    ${timeout_sums[0]} != "${timeout_sums[1]}" ||
	    ${timeout_sums[0]} != "${timeout_sums[2]}" ]]; then
		echo "NCOM-T020 startup bytes changed" >&2
		exit 1
	fi
	rg -q 'Confirmed commit applied; rollback is armed for 1 minute\.' \
		"$timeout_logical"
	validate_no_fatal "$timeout_logical" "$timeout_qemu" ncom-t020
	printf 'NCOM-T020\tpass\tncom-t020-guest-logical.log,ncom-t020-qemu.log\n' \
		>>"$results"
fi

if [[ $cell_selection != t020 ]]; then
	startup_before_confirm=$output/ncom-t021-startup-before-confirm.log
	run_cell ncom-t021 confirm_controller "$confirm_guest" "$confirm_qemu"
	tr -d '\r' <"$confirm_guest" >"$confirm_logical"
	awk '
		$0 == "net(config)> show startup-config" { active = 1; next }
		active && $0 == "net(config)> commit" { found_end = 1; exit }
		active { print }
		END { if (!active || !found_end) exit 1 }
	' "$confirm_logical" >"$startup_before_confirm"
	rg -q '^[[:blank:]]*- address: 10\.0\.2\.15$' \
		"$startup_before_confirm" || {
		echo "NCOM-T021 startup view changed before confirmation" >&2
		exit 1
	}
	require_state "$confirm_logical" ncom-t021-old "$old_address"
	require_state "$confirm_logical" ncom-t021-confirmed "$confirm_address"
	require_state "$confirm_logical" ncom-t021-after-deadline "$confirm_address"
	require_state "$confirm_logical" ncom-t021-rebooted "$confirm_address"
	mapfile -t confirm_sums < <(rg '^[0-9]+[[:blank:]]+[0-9]+[[:blank:]]+/etc/net.conf$' "$confirm_logical")
	if [[ ${#confirm_sums[@]} -ne 4 ||
	    ${confirm_sums[0]} == "${confirm_sums[1]}" ||
	    ${confirm_sums[1]} != "${confirm_sums[2]}" ||
	    ${confirm_sums[1]} != "${confirm_sums[3]}" ]]; then
		echo "NCOM-T021 persistence checksum transition is invalid" >&2
		exit 1
	fi
	rg -q 'Confirmed commit applied; rollback is armed for 1 minute\.' \
		"$confirm_logical"
	rg -q 'Commit complete\.' "$confirm_logical"
	[[ $(marker_count 'init: system running' "$confirm_logical") -eq 2 ]] || {
		echo "NCOM-T021 did not reach exactly two running boots" >&2
		exit 1
	}
	validate_no_fatal "$confirm_logical" "$confirm_qemu" ncom-t021
	printf 'NCOM-T021\tpass\tncom-t021-guest-logical.log,ncom-t021-qemu.log\n' \
		>>"$results"
fi

tracked_after=$(tracked_digest)
config_after=$(path_hash "$production_config")
production_image_after=$(path_hash "$production_image")
build_config_after=$(path_hash "$build_config")
source_after=$(path_hash "$source_image")
integrity=pass
if [[ $tracked_after != "$tracked_before" || $config_after != "$config_before" ||
    $production_image_after != "$production_image_before" ||
    $build_config_after != "$build_config_before" ||
    $source_after != "$source_hash" ]]; then
	integrity=fail
fi
{
	printf 'end_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'tracked_tree_sha256_after=%s\n' "$tracked_after"
	printf 'config_sha256_after=%s\n' "$config_after"
	printf 'production_image_sha256_after=%s\n' "$production_image_after"
	printf 'test_config_sha256_after=%s\n' "$build_config_after"
	printf 'source_image_sha256_after=%s\n' "$source_after"
	printf 'input_integrity_result=%s\n' "$integrity"
} >>"$metadata"
printf 'input-integrity\t%s\trun-metadata.txt\n' "$integrity" >>"$results"
if [[ $integrity != pass ]]; then
	echo "source, configuration, or image input changed during acceptance" >&2
	exit 1
fi

echo "WS011 NCOM-T020/T021 QEMU acceptance: PASS ($output)"

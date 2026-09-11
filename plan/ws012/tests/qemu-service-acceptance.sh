#!/usr/bin/env bash
# WS012 SVC-T006 production amd64 service-console QEMU acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../../.." && pwd)
image=$repo/build/amd64/hdd-image.img
config=$repo/config.mk
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-30}
action_timeout=${ACTION_TIMEOUT_SECONDS:-120}
cell_timeout=${CELL_TIMEOUT_SECONDS:-900}
key_delay=${KEY_DELAY_SECONDS:-0.015}

usage()
{
	cat <<EOF
usage: $0 OUTPUT-DIRECTORY

Boots a writable copy of build/amd64/hdd-image.img.  The main cell performs
two boots separated by a guest ZSV1 reboot, then halts.  A second fresh cell
checks poweroff.  The production image and config.mk are never modified.
EOF
}

if [[ $# -ne 1 ]]; then
	usage >&2
	exit 2
fi
output=$1

for timeout_value in "$boot_timeout" "$command_timeout" "$action_timeout" \
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

[[ -f $image ]] || {
	echo "production image not found: $image" >&2
	exit 2
}
if [[ -e $output ]]; then
	echo "output path already exists: $output" >&2
	exit 2
fi
command -v "$qemu" >/dev/null
command -v rg >/dev/null
command -v awk >/dev/null
command -v cp >/dev/null
command -v date >/dev/null
command -v sed >/dev/null
command -v sha256sum >/dev/null
command -v sleep >/dev/null
command -v tr >/dev/null
command -v timeout >/dev/null
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)

main_image=$output/main-run.img
poweroff_image=$output/poweroff-run.img
main_guest_log=$output/main-guest.log
main_logical_log=$output/main-guest-logical.log
main_qemu_log=$output/main-qemu.log
poweroff_guest_log=$output/poweroff-guest.log
poweroff_logical_log=$output/poweroff-guest-logical.log
poweroff_qemu_log=$output/poweroff-qemu.log
metadata=$output/run-metadata.txt
results=$output/results.tsv

base_hash=$(sha256sum "$image" | awk '{print $1}')
if [[ -e $config ]]; then
	config_state=present
	config_hash=$(sha256sum "$config" | awk '{print $1}')
else
	config_state=absent
	config_hash=-
fi

cp --reflink=auto --sparse=always "$image" "$main_image"
cp --reflink=auto --sparse=always "$image" "$poweroff_image"
: >"$main_guest_log"
: >"$main_qemu_log"
: >"$poweroff_guest_log"
: >"$poweroff_qemu_log"
printf 'case\tresult\tevidence\n' >"$results"

qemu_version=$("$qemu" --version | sed -n '1p')
{
	printf 'date_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'qemu=%s\n' "$qemu_version"
	printf 'base_image=%s\n' "$image"
	printf 'base_sha256=%s\n' "$base_hash"
	printf 'config_state=%s\n' "$config_state"
	printf 'config_sha256=%s\n' "$config_hash"
	printf 'boot_timeout_seconds=%s\n' "$boot_timeout"
	printf 'command_timeout_seconds=%s\n' "$command_timeout"
	printf 'action_timeout_seconds=%s\n' "$action_timeout"
	printf 'cell_timeout_seconds=%s\n' "$cell_timeout"
	printf 'main_command='
	printf '%q ' timeout --foreground --kill-after=5 "${cell_timeout}s" \
	    "$qemu" -machine pc -m 512 -smp 4 \
	    -drive "file=$main_image,format=raw,if=ide" -display none \
	    -serial none -debugcon "file:$main_guest_log" -monitor stdio
	printf '\n'
	printf 'poweroff_command='
	printf '%q ' timeout --foreground --kill-after=5 "${cell_timeout}s" \
	    "$qemu" -machine pc -m 512 -smp 4 \
	    -drive "file=$poweroff_image,format=raw,if=ide" -display none \
	    -serial none -debugcon "file:$poweroff_guest_log" -monitor stdio
	printf '\n'
} >"$metadata"

finish_acceptance()
{
	local status=$1 current_base current_config integrity=pass

	trap - EXIT
	set +e
	current_base=$(sha256sum "$image" 2>/dev/null | awk '{print $1}')
	if [[ -z $current_base || $current_base != "$base_hash" ]]; then
		echo "production input image changed during acceptance" >&2
		integrity=fail
		status=1
	fi
	if [[ $config_state == present ]]; then
		if [[ -f $config ]]; then
			current_config=$(sha256sum "$config" 2>/dev/null | awk '{print $1}')
		else
			current_config=missing
		fi
		if [[ $current_config != "$config_hash" ]]; then
			echo "config.mk changed during acceptance" >&2
			integrity=fail
			status=1
		fi
	else
		current_config=-
		if [[ -e $config ]]; then
			echo "acceptance created config.mk" >&2
			integrity=fail
			status=1
			current_config=created
		fi
	fi
	{
		printf 'base_sha256_after=%s\n' "${current_base:-missing}"
		printf 'config_sha256_after=%s\n' "$current_config"
		printf 'input_integrity_result=%s\n' "$integrity"
	} >>"$metadata" || status=1
	printf 'input-integrity\t%s\trun-metadata.txt\n' "$integrity" \
	    >>"$results" || status=1
	if [[ $status -eq 0 ]]; then
		rm -f -- "$main_image" "$poweroff_image" || status=1
	fi
	if [[ $status -eq 0 ]]; then
		echo "WS012 SVC-T006 QEMU acceptance: PASS ($output)"
	fi
	exit "$status"
}

trap 'finish_acceptance "$?"' EXIT

controller_deadline=0

marker_count()
{
	local pattern=$1 file=$2 count
	count=$(rg -a -c -- "$pattern" "$file" 2>/dev/null || true)
	printf '%s\n' "${count:-0}"
}

wait_for_pattern()
{
	local pattern=$1 file=$2 timeout=$3 minimum=${4:-1}
	local deadline count now

	now=$(date +%s)
	deadline=$((now + 10#$timeout))
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
	local text=$1 character key index

	for ((index = 0; index < ${#text}; index++)); do
		character=${text:index:1}
		case $character in
		' ') key=spc ;;
		/) key=slash ;;
		=) key=equal ;;
		-) key=minus ;;
		.) key=dot ;;
		_) key=shift-minus ;;
		\?) key=shift-slash ;;
		[a-z0-9]) key=$character ;;
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
service_prompt='service> '

login_guest()
{
	local log=$1 password_before shell_before

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

guest_marker()
{
	local log=$1 marker=$2

	send_shell "$log" "echo $marker" || return 1
	wait_for_pattern "^${marker}\\r?$" "$log" "$command_timeout"
}

enter_console()
{
	local log=$1 before

	before=$(marker_count "$service_prompt" "$log")
	send_text service || return 1
	wait_for_pattern "$service_prompt" "$log" "$command_timeout" \
	    $((before + 1))
}

send_console()
{
	local log=$1 text=$2 before

	before=$(marker_count "$service_prompt" "$log")
	send_text "$text" || return 1
	wait_for_pattern "$service_prompt" "$log" "$command_timeout" \
	    $((before + 1))
}

leave_console()
{
	local log=$1 before

	before=$(marker_count "$shell_prompt" "$log")
	send_text quit || return 1
	wait_for_pattern "$shell_prompt" "$log" "$command_timeout" \
	    $((before + 1))
}

main_controller_body()
{
	local log=$1 action_before login_before stop_before

	wait_for_pattern "$login_prompt" "$log" "$boot_timeout" || return 1
	login_guest "$log" || return 1
	guest_marker "$log" p006-boot-a || return 1

	guest_marker "$log" p006-argv-begin || return 1
	send_shell "$log" hostname || return 1
	guest_marker "$log" p006-init-socket-stat-begin || return 1
	send_shell "$log" 'stat /run/init.sock' || return 1
	guest_marker "$log" p006-init-socket-stat-end || return 1
	guest_marker "$log" p006-rcconf-stat-begin || return 1
	send_shell "$log" 'stat /etc/rc.conf' || return 1
	guest_marker "$log" p006-rcconf-stat-end || return 1
	send_shell "$log" 'cat /etc/rc.conf' || return 1
	guest_marker "$log" p006-argv-list-begin || return 1
	send_shell "$log" 'service list' || return 1
	guest_marker "$log" p006-argv-list-end || return 1
	guest_marker "$log" p006-argv-show-begin || return 1
	send_shell "$log" 'service show' || return 1
	guest_marker "$log" p006-argv-show-end || return 1
	guest_marker "$log" p006-argv-show-networkd-begin || return 1
	send_shell "$log" 'service show networkd' || return 1
	guest_marker "$log" p006-argv-show-networkd-end || return 1
	guest_marker "$log" p006-argv-status-networkd-begin || return 1
	send_shell "$log" 'service status networkd' || return 1
	guest_marker "$log" p006-argv-status-networkd-end || return 1
	guest_marker "$log" p006-argv-unknown-begin || return 1
	send_shell "$log" 'service status bogus' || return 1
	guest_marker "$log" p006-argv-unknown-end || return 1
	guest_marker "$log" p006-argv-end || return 1

	guest_marker "$log" p006-runtime-begin || return 1
	send_shell "$log" 'cksum /etc/rc.conf' || return 1
	send_shell "$log" 'service stop cron' || return 1
	send_shell "$log" 'service show cron' || return 1
	send_shell "$log" 'service start cron' || return 1
	send_shell "$log" 'service restart cron' || return 1
	send_shell "$log" 'service show cron' || return 1
	send_shell "$log" 'cksum /etc/rc.conf' || return 1
	guest_marker "$log" p006-runtime-end || return 1

	guest_marker "$log" p006-console-begin || return 1
	enter_console "$log" || return 1
	send_console "$log" '?' || return 1
	send_console "$log" help || return 1
	send_console "$log" '' || return 1
	send_console "$log" list || return 1
	send_console "$log" show || return 1
	send_console "$log" 'show networkd' || return 1
	send_console "$log" 'status networkd' || return 1
	send_console "$log" 'stop cron' || return 1
	send_console "$log" 'start cron' || return 1
	send_console "$log" 'restart cron' || return 1
	send_console "$log" 'enable cron' || return 1
	send_console "$log" 'disable cron' || return 1
	send_console "$log" reload || return 1
	send_console "$log" 'show cron' || return 1
	send_console "$log" 'status bogus' || return 1
	send_console "$log" 'show cron' || return 1
	leave_console "$log" || return 1
	guest_marker "$log" p006-console-end || return 1

	send_shell "$log" 'service show cron' || return 1
	send_shell "$log" 'stat /etc/rc.conf' || return 1
	send_shell "$log" 'cat /etc/rc.conf' || return 1
	guest_marker "$log" p006-malformed-begin || return 1
	send_shell "$log" 'cp /etc/rc.conf /tmp/rc-good' || return 1
	send_shell "$log" 'cp /etc/service.d/cron /etc/rc.conf' || return 1
	send_shell "$log" 'service reload' || return 1
	send_shell "$log" 'service show cron' || return 1
	send_shell "$log" 'cp /tmp/rc-good /etc/rc.conf' || return 1
	send_shell "$log" 'service reload' || return 1
	send_shell "$log" 'cat /etc/rc.conf' || return 1
	guest_marker "$log" p006-malformed-end || return 1

	guest_marker "$log" p006-reboot-begin || return 1
	login_before=$(marker_count "$login_prompt" "$log")
	stop_before=$(marker_count 'init: stopping services' "$log")
	action_before=$(marker_count 'init: executing system action reboot' "$log")
	send_text '/sbin/reboot' || return 1
	wait_for_pattern 'init: stopping services' "$log" "$action_timeout" \
	    $((stop_before + 1)) || return 1
	wait_for_pattern 'init: executing system action reboot' "$log" \
	    "$action_timeout" $((action_before + 1)) || return 1
	wait_for_pattern "$login_prompt" "$log" "$boot_timeout" \
	    $((login_before + 1)) || return 1
	login_guest "$log" || return 1

	guest_marker "$log" p006-boot-b || return 1
	send_shell "$log" 'service list' || return 1
	send_shell "$log" 'service show cron' || return 1
	send_shell "$log" 'cat /etc/rc.conf' || return 1
	guest_marker "$log" p006-persist-disabled || return 1
	send_shell "$log" 'service enable cron' || return 1
	send_shell "$log" 'service show cron' || return 1
	guest_marker "$log" p006-policy-only-enabled || return 1
	send_shell "$log" 'service start cron' || return 1
	send_shell "$log" 'service show cron' || return 1
	send_shell "$log" 'cat /etc/rc.conf' || return 1
	guest_marker "$log" p006-started || return 1

	guest_marker "$log" p006-halt-begin || return 1
	stop_before=$(marker_count 'init: stopping services' "$log")
	action_before=$(marker_count 'init: executing system action halt' "$log")
	send_text '/sbin/halt' || return 1
	wait_for_pattern 'init: stopping services' "$log" "$action_timeout" \
	    $((stop_before + 1)) || return 1
	wait_for_pattern 'init: executing system action halt' "$log" \
	    "$action_timeout" $((action_before + 1)) || return 1
	sleep 1
}

main_controller()
{
	local status

	trap '' PIPE
	controller_deadline=$(( $(date +%s) + 10#$cell_timeout ))
	set +e
	main_controller_body "$@"
	status=$?
	printf 'quit\n' || :
	return "$status"
}

poweroff_controller_body()
{
	local log=$1 action_before stop_before

	wait_for_pattern "$login_prompt" "$log" "$boot_timeout" || return 1
	login_guest "$log" || return 1
	guest_marker "$log" p006-poweroff-begin || return 1
	send_shell "$log" 'stat /run/init.sock' || return 1
	stop_before=$(marker_count 'init: stopping services' "$log")
	action_before=$(marker_count 'init: executing system action poweroff' "$log")
	send_text '/sbin/poweroff' || return 1
	wait_for_pattern 'init: stopping services' "$log" "$action_timeout" \
	    $((stop_before + 1)) || return 1
	wait_for_pattern 'init: executing system action poweroff' "$log" \
	    "$action_timeout" $((action_before + 1)) || return 1
	sleep 1
}

poweroff_controller()
{
	local status

	trap '' PIPE
	controller_deadline=$(( $(date +%s) + 10#$cell_timeout ))
	set +e
	poweroff_controller_body "$@"
	status=$?
	printf 'quit\n' || :
	return "$status"
}

run_qemu_cell()
{
	local name=$1 run_image=$2 guest_log=$3 qemu_log=$4 controller=$5
	local -a statuses
	local controller_status qemu_status
	local command=(
	    "$qemu" -machine pc -m 512 -smp 4
	    -drive "file=$run_image,format=raw,if=ide"
	    -display none -serial none -debugcon "file:$guest_log"
	    -monitor stdio
	)

	set +e
	"$controller" "$guest_log" | \
	    timeout --foreground --kill-after=5 "${cell_timeout}s" \
		"${command[@]}" >"$qemu_log" 2>&1
	statuses=("${PIPESTATUS[@]}")
	set -e
	controller_status=${statuses[0]}
	qemu_status=${statuses[1]}
	printf '%s_controller_status=%s\n' "$name" "$controller_status" \
	    >>"$metadata"
	printf '%s_qemu_status=%s\n' "$name" "$qemu_status" >>"$metadata"
	if [[ $controller_status -ne 0 || $qemu_status -ne 0 ]]; then
		echo "$name QEMU cell failed: controller=$controller_status qemu=$qemu_status" >&2
		return 1
	fi
}

extract_section()
{
	local source=$1 begin=$2 end=$3 destination=$4

	awk -v begin="$begin" -v end="$end" '
	    $0 == begin { active = 1; next }
	    $0 == end { active = 0; found_end = 1; exit }
	    active { print }
	    END { if (!found_end) exit 1 }
	' "$source" >"$destination"
}

extract_console_command()
{
	local source=$1 command=$2 occurrence=$3 destination=$4

	awk -v target="service> $command" -v wanted="$occurrence" '
	    $0 == target {
	        seen++
	        if (seen == wanted) {
	            active = 1
	            found = 1
	            next
	        }
	    }
	    active && /^service> / { found_end = 1; exit }
	    active { print }
	    END { if (!found || !found_end) exit 1 }
	' "$source" >"$destination"
}

require_fixed()
{
	local text=$1 file=$2 description=$3

	if ! rg -a -F -q -- "$text" "$file"; then
		echo "missing $description: $text ($file)" >&2
		return 1
	fi
}

require_regex()
{
	local pattern=$1 file=$2 description=$3

	if ! rg -a -q -- "$pattern" "$file"; then
		echo "missing $description: $pattern ($file)" >&2
		return 1
	fi
}

reject_regex()
{
	local pattern=$1 file=$2 description=$3 status

	set +e
	rg -a -q -- "$pattern" "$file"
	status=$?
	set -e
	if [[ $status -eq 0 ]]; then
		echo "unexpected $description: $pattern ($file)" >&2
		return 1
	fi
	if [[ $status -ne 1 ]]; then
		echo "cannot scan for $description: $file" >&2
		return 1
	fi
}

require_empty()
{
	local file=$1 description=$2

	if [[ -s $file ]]; then
		echo "unexpected output for $description ($file)" >&2
		return 1
	fi
}

require_cron_yaml()
{
	local file=$1 expected=$2

	awk -v expected="$expected" '
	    $0 == "  cron:" { in_cron = 1; next }
	    in_cron && $0 == "    enabled: " expected { found = 1 }
	    in_cron && /^  [^ ]/ { in_cron = 0 }
	    END { exit(found ? 0 : 1) }
	' "$file"
}

append_result()
{
	printf '%s\tpass\t%s\n' "$1" "$2" >>"$results"
}

fatal_pattern='fatal:|FATAL:|kernel panic|panic:|amd64 fault v=|assert(ion)? failed|Segmentation fault|double free|heap corruption|VFS initialization failed|init: control socket:|init: final system action failed|init: dependency cycle:|init: invalid service definition:|init: oneshot .* failed|init: skipped |readiness timeout|exited before readiness|service: invalid ZSV1|service: init request failed|service: persistent policy changed|usb-storage: BOT .*error=[1-9]|usb-storage: sd[a-z]+ .*error=[1-9]|loop[0-9]+: write .*error=[1-9]'
qemu_fatal_pattern='(^|[[:space:]])Error:|unknown command|invalid command|invalid parameter|qemu-system-[^:]+:.*([Ee]rror|[Ff]ailed|Could not|cannot)|[Aa]ssertion .* failed|Segmentation fault|Aborted'

validate_no_fatal()
{
	local file=$1 label=$2 scan=$output/$2-fatal-scan.txt status

	set +e
	rg -a -n -- "$fatal_pattern" "$file" >"$scan"
	status=$?
	set -e
	if [[ $status -eq 0 ]]; then
		echo "fatal diagnostic found in $file" >&2
		cat "$scan" >&2
		return 1
	fi
	if [[ $status -ne 1 ]]; then
		echo "cannot scan fatal diagnostics in $file" >&2
		return 1
	fi
	: >"$scan"
}

validate_no_qemu_fatal()
{
	local file=$1 label=$2 scan=$output/$2-fatal-scan.txt status

	set +e
	rg -a -n -- "$qemu_fatal_pattern" "$file" >"$scan"
	status=$?
	set -e
	if [[ $status -eq 0 ]]; then
		echo "QEMU or HMP diagnostic found in $file" >&2
		cat "$scan" >&2
		return 1
	fi
	if [[ $status -ne 1 ]]; then
		echo "cannot scan QEMU diagnostics in $file" >&2
		return 1
	fi
	: >"$scan"
}

validate_main()
{
	local argv=$output/argv-section.log
	local argv_list=$output/argv-list-section.log
	local argv_show=$output/argv-show-section.log
	local argv_show_networkd=$output/argv-show-networkd-section.log
	local argv_status_networkd=$output/argv-status-networkd-section.log
	local argv_unknown=$output/argv-unknown-section.log
	local init_socket_stat=$output/init-socket-stat-section.log
	local rcconf_stat=$output/rcconf-stat-section.log
	local runtime=$output/runtime-section.log
	local console=$output/console-section.log
	local console_question=$output/console-question.log
	local console_help=$output/console-help.log
	local console_blank=$output/console-blank.log
	local console_list=$output/console-list.log
	local console_show=$output/console-show.log
	local console_show_networkd=$output/console-show-networkd.log
	local console_status_networkd=$output/console-status-networkd.log
	local console_stop=$output/console-stop.log
	local console_start=$output/console-start.log
	local console_restart=$output/console-restart.log
	local console_enable=$output/console-enable.log
	local console_disable=$output/console-disable.log
	local console_reload=$output/console-reload.log
	local console_show_cron=$output/console-show-cron.log
	local console_unknown=$output/console-unknown.log
	local console_recovered=$output/console-recovered.log
	local malformed=$output/malformed-section.log
	local boot_b=$output/boot-b-section.log
	local policy=$output/policy-enabled-section.log
	local started=$output/started-section.log
	local detail
	local -a sums

	tr -d '\r' <"$main_guest_log" >"$main_logical_log"
	extract_section "$main_logical_log" p006-argv-begin \
	    p006-argv-end "$argv"
	extract_section "$main_logical_log" p006-argv-list-begin \
	    p006-argv-list-end "$argv_list"
	extract_section "$main_logical_log" p006-argv-show-begin \
	    p006-argv-show-end "$argv_show"
	extract_section "$main_logical_log" p006-argv-show-networkd-begin \
	    p006-argv-show-networkd-end "$argv_show_networkd"
	extract_section "$main_logical_log" p006-argv-status-networkd-begin \
	    p006-argv-status-networkd-end "$argv_status_networkd"
	extract_section "$main_logical_log" p006-argv-unknown-begin \
	    p006-argv-unknown-end "$argv_unknown"
	extract_section "$main_logical_log" p006-init-socket-stat-begin \
	    p006-init-socket-stat-end "$init_socket_stat"
	extract_section "$main_logical_log" p006-rcconf-stat-begin \
	    p006-rcconf-stat-end "$rcconf_stat"
	extract_section "$main_logical_log" p006-runtime-begin \
	    p006-runtime-end "$runtime"
	extract_section "$main_logical_log" p006-console-begin \
	    p006-console-end "$console"
	extract_section "$main_logical_log" p006-malformed-begin \
	    p006-malformed-end "$malformed"
	extract_section "$main_logical_log" p006-boot-b \
	    p006-persist-disabled "$boot_b"
	extract_section "$main_logical_log" p006-persist-disabled \
	    p006-policy-only-enabled "$policy"
	extract_section "$main_logical_log" p006-policy-only-enabled \
	    p006-started "$started"
	extract_console_command "$console" '?' 1 "$console_question"
	extract_console_command "$console" help 1 "$console_help"
	extract_console_command "$console" '' 1 "$console_blank"
	extract_console_command "$console" list 1 "$console_list"
	extract_console_command "$console" show 1 "$console_show"
	extract_console_command "$console" 'show networkd' 1 \
	    "$console_show_networkd"
	extract_console_command "$console" 'status networkd' 1 \
	    "$console_status_networkd"
	extract_console_command "$console" 'stop cron' 1 "$console_stop"
	extract_console_command "$console" 'start cron' 1 "$console_start"
	extract_console_command "$console" 'restart cron' 1 "$console_restart"
	extract_console_command "$console" 'enable cron' 1 "$console_enable"
	extract_console_command "$console" 'disable cron' 1 "$console_disable"
	extract_console_command "$console" reload 1 "$console_reload"
	extract_console_command "$console" 'show cron' 1 "$console_show_cron"
	extract_console_command "$console" 'status bogus' 1 "$console_unknown"
	extract_console_command "$console" 'show cron' 2 "$console_recovered"

	[[ $(marker_count 'init: system running' "$main_logical_log") -eq 2 ]] || {
		echo "main cell did not reach init exactly twice" >&2
		return 1
	}
	require_regex '^zedbsd$' "$argv" 'configured hostname'
	require_regex '^/run/init\.sock: type=socket mode=c180 .* uid=0 gid=0 ' \
	    "$init_socket_stat" 'root-only init socket mode'
	require_regex '^/etc/rc\.conf: type=regular mode=81a4 .* uid=0 gid=0 ' \
	    "$rcconf_stat" 'rc.conf mode and owner'
	for detail in "$argv_list" "$argv_show"; do
		require_fixed 'NAME        STATUS    ENABLED   PID' "$detail" \
		    'concise argv table header'
		require_regex '^networkd[[:blank:]]+running[[:blank:]]+yes[[:blank:]]+[1-9][0-9]*$' \
		    "$detail" 'networkd argv table row'
		require_regex '^net[[:blank:]]+completed[[:blank:]]+yes[[:blank:]]+-$' \
		    "$detail" 'completed argv oneshot row'
		require_regex '^ntpdate[[:blank:]]+stopped[[:blank:]]+no[[:blank:]]+-$' \
		    "$detail" 'disabled argv service row'
	done
	for detail in "$argv_show_networkd" "$argv_status_networkd"; do
		require_fixed 'NAME        STATUS    ENABLED   PID' "$detail" \
		    'argv detail header'
		require_regex '^networkd[[:blank:]]+running[[:blank:]]+yes[[:blank:]]+[1-9][0-9]*$' \
		    "$detail" 'argv detail networkd row'
		require_fixed 'TYPE        daemon' "$detail" 'argv detail type'
		require_fixed 'COMMAND     /sbin/networkd' "$detail" \
		    'argv detail command'
		require_fixed 'RESTART     on-failure' "$detail" \
		    'argv detail restart policy'
		require_fixed 'AFTER       syslogd' "$detail" \
		    'argv detail direct dependency'
	done
	require_regex 'unknown-service \(errno [1-9][0-9]*\)' \
	    "$argv_unknown" 'typed argv unknown-service error'
	append_result argv-list-show \
	    'argv-list-section.log,argv-show-section.log,argv-show-networkd-section.log,argv-status-networkd-section.log,argv-unknown-section.log'

	mapfile -t sums < <(rg -a '^[0-9]+[[:blank:]]+[0-9]+[[:blank:]]+/etc/rc.conf$' "$runtime")
	if [[ ${#sums[@]} -ne 2 || ${sums[0]} != "${sums[1]}" ]]; then
		echo "runtime operations changed rc.conf" >&2
		return 1
	fi
	require_fixed 'OK stopped' "$runtime" 'runtime stop'
	require_fixed 'OK started' "$runtime" 'runtime start'
	require_fixed 'OK restarted' "$runtime" 'runtime restart'
	require_regex '^cron[[:blank:]]+stopped[[:blank:]]+yes[[:blank:]]+-$' \
	    "$runtime" 'stopped runtime state'
	require_regex '^cron[[:blank:]]+running[[:blank:]]+yes[[:blank:]]+[1-9][0-9]*$' \
	    "$runtime" 'running runtime state'
	append_result runtime-only runtime-section.log

	require_fixed 'zedBSD Service Console' "$console" 'console banner'
	require_fixed "Type '?' for help." "$console" 'console hint'
	require_fixed 'Commands:' "$console_question" 'question-mark help'
	require_fixed '  list           show all services' "$console_question" \
	    'question-mark list help'
	reject_regex '(^|[[:blank:]])(save|commit)([[:blank:]]|$)' \
	    "$console_question" 'candidate command in question-mark help'
	require_fixed 'Commands:' "$console_help" 'help command output'
	require_fixed '  list           show all services' "$console_help" \
	    'help command list output'
	reject_regex '(^|[[:blank:]])(save|commit)([[:blank:]]|$)' \
	    "$console_help" 'candidate command in help output'
	require_empty "$console_blank" 'blank console command'

	require_fixed 'NAME        STATUS    ENABLED   PID' "$console_list" \
	    'interactive list header'
	require_regex '^networkd[[:blank:]]+running[[:blank:]]+yes[[:blank:]]+[1-9][0-9]*$' \
	    "$console_list" 'interactive list row'
	reject_regex '^service:' "$console_list" 'interactive list error'
	require_fixed 'NAME        STATUS    ENABLED   PID' "$console_show" \
	    'interactive show-all header'
	require_regex '^networkd[[:blank:]]+running[[:blank:]]+yes[[:blank:]]+[1-9][0-9]*$' \
	    "$console_show" 'interactive show-all row'
	reject_regex '^service:' "$console_show" 'interactive show-all error'

	for detail in "$console_show_networkd" "$console_status_networkd"; do
		require_fixed 'NAME        STATUS    ENABLED   PID' "$detail" \
		    'interactive detail header'
		require_regex '^networkd[[:blank:]]+running[[:blank:]]+yes[[:blank:]]+[1-9][0-9]*$' \
		    "$detail" 'interactive networkd detail row'
		require_fixed 'TYPE        daemon' "$detail" \
		    'interactive detail type'
		require_fixed 'COMMAND     /sbin/networkd' "$detail" \
		    'interactive detail command'
		require_fixed 'AFTER       syslogd' "$detail" \
		    'interactive detail dependency'
		reject_regex '^service:' "$detail" 'interactive detail error'
	done

	require_regex '^OK stopped$' "$console_stop" 'interactive stop'
	reject_regex '^service:' "$console_stop" 'interactive stop error'
	require_regex '^OK started$' "$console_start" 'interactive start'
	reject_regex '^service:' "$console_start" 'interactive start error'
	require_regex '^OK restarted$' "$console_restart" 'interactive restart'
	reject_regex '^service:' "$console_restart" 'interactive restart error'
	require_regex '^OK enabled cron$' "$console_enable" 'interactive enable'
	reject_regex '^service:' "$console_enable" 'interactive enable error'
	require_regex '^OK disabled cron$' "$console_disable" \
	    'interactive disable'
	reject_regex '^service:' "$console_disable" 'interactive disable error'
	require_regex '^OK reloaded$' "$console_reload" 'interactive reload'
	reject_regex '^service:' "$console_reload" 'interactive reload error'
	require_regex '^cron[[:blank:]]+running[[:blank:]]+no[[:blank:]]+[1-9][0-9]*$' \
	    "$console_show_cron" 'policy-only interactive state'
	reject_regex '^service:' "$console_show_cron" \
	    'interactive policy detail error'
	require_regex 'unknown-service \(errno [1-9][0-9]*\)' \
	    "$console_unknown" 'interactive expected error'
	require_regex '^cron[[:blank:]]+running[[:blank:]]+no[[:blank:]]+[1-9][0-9]*$' \
	    "$console_recovered" 'interactive recovery state'
	reject_regex '^service:' "$console_recovered" \
	    'interactive recovery error'
	append_result interactive-console console-section.log
	append_result policy-only console-section.log

	require_regex 'init: cannot reload /etc/rc.conf:' "$malformed" \
	    'malformed reload rejection'
	require_regex 'reload-failed \(errno [1-9][0-9]*\)' "$malformed" \
	    'typed reload failure'
	require_regex '^cron[[:blank:]]+running[[:blank:]]+no[[:blank:]]+[1-9][0-9]*$' \
	    "$malformed" 'preserved runtime policy after bad reload'
	require_fixed 'OK reloaded' "$malformed" 'reload after restoration'
	require_cron_yaml "$malformed" false || {
		echo "restored rc.conf did not retain disabled cron policy" >&2
		return 1
	}
	append_result malformed-reload malformed-section.log

	require_regex '^cron[[:blank:]]+stopped[[:blank:]]+no[[:blank:]]+-$' \
	    "$boot_b" 'disabled cron after reboot'
	require_cron_yaml "$boot_b" false || {
		echo "disabled cron policy did not survive reboot" >&2
		return 1
	}
	append_result reboot-persistence boot-b-section.log

	require_fixed 'OK enabled cron' "$policy" 'persistent enable'
	require_regex '^cron[[:blank:]]+stopped[[:blank:]]+yes[[:blank:]]+-$' \
	    "$policy" 'enable did not start cron'
	append_result policy-enable-only policy-enabled-section.log

	require_fixed 'OK started' "$started" 'explicit start after enable'
	require_regex '^cron[[:blank:]]+running[[:blank:]]+yes[[:blank:]]+[1-9][0-9]*$' \
	    "$started" 'explicitly started cron'
	require_cron_yaml "$started" true || {
		echo "enabled cron policy was not canonical" >&2
		return 1
	}
	append_result explicit-start started-section.log

	require_fixed 'p006-reboot-begin' "$main_logical_log" 'reboot marker'
	require_fixed 'p006-halt-begin' "$main_logical_log" 'halt marker'
	require_fixed 'init: executing system action reboot' "$main_logical_log" \
	    'reboot handoff'
	require_fixed 'init: executing system action halt' "$main_logical_log" \
	    'halt handoff'
	[[ $(marker_count 'init: stopping services' "$main_logical_log") -ge 2 ]] || {
		echo "reboot and halt actions were not both scheduled" >&2
		return 1
	}
	append_result reboot-action main-guest.log
	append_result halt-action main-guest.log
	validate_no_fatal "$main_logical_log" main
	validate_no_qemu_fatal "$main_qemu_log" main-qemu
}

validate_poweroff()
{
	tr -d '\r' <"$poweroff_guest_log" >"$poweroff_logical_log"
	require_fixed 'init: system running' "$poweroff_logical_log" \
	    'poweroff-cell boot'
	require_fixed 'p006-poweroff-begin' "$poweroff_logical_log" \
	    'poweroff marker'
	require_fixed '/sbin/poweroff' "$poweroff_logical_log" \
	    'poweroff command'
	require_fixed 'init: stopping services' "$poweroff_logical_log" \
	    'poweroff action'
	require_fixed 'init: executing system action poweroff' \
	    "$poweroff_logical_log" 'poweroff handoff'
	validate_no_fatal "$poweroff_logical_log" poweroff
	validate_no_qemu_fatal "$poweroff_qemu_log" poweroff-qemu
	append_result poweroff-action poweroff-guest.log
}

run_qemu_cell main "$main_image" "$main_guest_log" "$main_qemu_log" \
    main_controller
validate_main
run_qemu_cell poweroff "$poweroff_image" "$poweroff_guest_log" \
    "$poweroff_qemu_log" poweroff_controller
validate_poweroff
append_result main-fatal-scan main-fatal-scan.txt
append_result main-qemu-fatal-scan main-qemu-fatal-scan.txt
append_result poweroff-fatal-scan poweroff-fatal-scan.txt
append_result poweroff-qemu-fatal-scan poweroff-qemu-fatal-scan.txt

#!/bin/sh
# ws004-p031 legacy-HCD concurrent HID/storage and root-hotplug acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 AUX-IMAGE OUTPUT-DIRECTORY" >&2
	exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
source_auxiliary=$1
output=$2
make_command=${MAKE:-make}
phase_config=plan/ws004-hardware/tests/config-amd64-legacy-hcd.mk
canonical_image=build/amd64/hdd-image.img
source_boot=$root/$canonical_image
temporary_root=${TMPDIR:-"$root/build/q047-tmp"}
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-45}
hotplug_timeout=${HOTPLUG_TIMEOUT_SECONDS:-30}
cell_timeout=${CELL_TIMEOUT_SECONDS:-600}
stage_delay=${STAGE_DELAY_SECONDS:-30}
key_delay=${KEY_DELAY_SECONDS:-0.03}
# Keep the phase acceptance default at both cells while allowing a failure
# investigation to execute one named cell without retrying or cloning the
# runner.  Evidence records the selected topology explicitly.
diagnostic_cells=${LEGACY_HCD_CELLS:-"uhci paired"}
# The first read admitted by QEMU's token bucket can complete immediately.
# A 4-KiB priming read immediately before the observed read therefore leaves
# two seconds of nominal debt at 2 KiB/s.  This is deliberately much longer
# than the debug-console/monitor polling latency, making DD-START..DD-DONE an
# actual concurrent-I/O interval instead of a host scheduling race, while an
# 8-KiB filesystem probe still completes inside the 5-second BOT timeout.
storage_read_bps=2048
storage_prime_bytes=4096
storage_nominal_delay_ms=2000

case $boot_timeout:$command_timeout:$hotplug_timeout:$cell_timeout:$stage_delay in
	*[!0-9:]* | 0:* | *:0:* | *:0:* | *:0:* | *:0)
		echo "timeouts and STAGE_DELAY_SECONDS must be positive integers" >&2
		exit 2
		;;
esac
case $key_delay in
	'' | *[!0-9.]* | .* | *.*.*)
		echo "KEY_DELAY_SECONDS must be a non-negative decimal" >&2
		exit 2
	;;
esac
case $diagnostic_cells in
	uhci)
		cell_description=standalone-UHCI
		;;
	paired)
		cell_description=paired-EHCI-with-three-UHCI-companions
		;;
	'uhci paired')
		cell_description=standalone-UHCI,paired-EHCI-with-three-UHCI-companions
		;;
	*)
		echo "LEGACY_HCD_CELLS must be 'uhci', 'paired', or 'uhci paired'" >&2
		exit 2
		;;
esac

test -f "$ovmf_code" || { echo "OVMF code not found: $ovmf_code" >&2; exit 2; }
test -f "$ovmf_vars" || { echo "OVMF vars not found: $ovmf_vars" >&2; exit 2; }
for command in "$make_command" "$qemu" awk cp cut date dd find mkdir rg sed sha256sum \
	    sleep sort tail timeout tr truncate wc; do
	command -v "$command" >/dev/null || {
		echo "required command not found: $command" >&2
		exit 2
	}
done
# Build the canonical image from the current checkout before hashing it.  -B
# prevents an older config-compatible object or image from satisfying HW-T25.
mkdir -p "$temporary_root"
TMPDIR="$temporary_root" "$make_command" -B -j16 -C "$root" \
	ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-amd64-legacy-hcd.mk \
	disk-image
test -f "$source_boot" || {
	echo "canonical amd64 HDD image not found after rebuild: $source_boot" >&2
	exit 2
}
test -f "$source_auxiliary" || {
	echo "auxiliary image not found after canonical rebuild: $source_auxiliary" >&2
	exit 2
}
if [ "$(wc -c <"$source_auxiliary")" -lt 4096 ]; then
	echo "auxiliary image is smaller than the required 4 KiB payload" >&2
	exit 2
fi
config_digest=$(sha256sum "$root/$phase_config" | awk '{print $1}')

if [ -e "$output" ]; then
	test -d "$output" || {
		echo "output path is not a directory: $output" >&2
		exit 2
	}
	if [ -n "$(find "$output" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
		echo "output directory is not empty: $output" >&2
		exit 2
	fi
else
	mkdir -p "$output"
fi
output=$(CDPATH= cd -- "$output" && pwd)

boot_digest=$(sha256sum "$source_boot" | awk '{print $1}')
auxiliary_digest=$(sha256sum "$source_auxiliary" | awk '{print $1}')
summary_metadata=$output/metadata.txt
results=$output/results.tsv
printf 'cell\tresult\tevidence\n' >"$results"
{
	echo "test=HW-T25 ws004-p031"
	echo "start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "qemu=$($qemu --version | sed -n '1p')"
	echo "source_provenance=forced canonical rebuild"
	echo "source_build_command=$make_command -B -j16 -C $root ZEDBSD_CONFIG=$phase_config disk-image"
	echo "source_build_config=$root/$phase_config"
	echo "source_build_config_sha256=$config_digest"
	echo "source_build_target=$canonical_image"
	echo "source_boot=$source_boot"
	echo "source_boot_sha256_before=$boot_digest"
	echo "source_auxiliary=$source_auxiliary"
	echo "source_auxiliary_sha256_before=$auxiliary_digest"
	echo "cells=$cell_description"
	echo "hotplug_cycles_per_cell=11"
	echo "storage_bytes_per_read=4096"
	echo "storage_throttle_read_bps=$storage_read_bps"
	echo "storage_throttle_prime_bytes=$storage_prime_bytes"
	echo "storage_throttle_nominal_delay_ms=$storage_nominal_delay_ms"
	echo "paired_hid_runtime_owner=UHCI companion (low/full speed)"
	echo "paired_storage_runtime_owner=EHCI (high speed QEMU usb-storage)"
	echo "ehci_high_speed_periodic_runtime_evidence=not claimed"
	echo "ehci_periodic_evidence_boundary=production source and HW-T25 model"
} >"$summary_metadata"

finish()
{
	finish_status=$1
	trap - EXIT
	set +e
	boot_after=$(sha256sum "$source_boot" 2>/dev/null | awk '{print $1}')
	auxiliary_after=$(sha256sum "$source_auxiliary" 2>/dev/null |
	    awk '{print $1}')
	integrity=pass
	if [ "$boot_after" != "$boot_digest" ] ||
	    [ "$auxiliary_after" != "$auxiliary_digest" ]; then
		echo "a pristine input image changed during HW-T25" >&2
		integrity=fail
		finish_status=1
	fi
	{
		echo "end_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		echo "source_boot_sha256_after=${boot_after:-missing}"
		echo "source_auxiliary_sha256_after=${auxiliary_after:-missing}"
		echo "input_integrity=$integrity"
		echo "acceptance_exit_status=$finish_status"
	} >>"$summary_metadata"
	if [ "$finish_status" -eq 0 ]; then
		echo "HW-T25 legacy HCD concurrent/hotplug QEMU: PASS ($output)"
	else
		echo "HW-T25 legacy HCD concurrent/hotplug QEMU: FAIL ($output)" >&2
	fi
	exit "$finish_status"
}
trap 'finish "$?"' EXIT

marker_count()
{
	marker_pattern=$1
	marker_file=$2
	marker_value=$(rg -a -c -- "$marker_pattern" "$marker_file" \
	    2>/dev/null || true)
	printf '%s\n' "${marker_value:-0}"
}

run_cell()
{
	cell_kind=$1
	cell=$output/$cell_kind
	boot_copy=$cell/boot.img
	auxiliary_copy=$cell/auxiliary.img
	expected_payload=$cell/expected-payload.bin
	vars_copy=$cell/OVMF_VARS.fd
	guest_log=$cell/guest.log
	qemu_log=$cell/qemu.log
	controller_result=$cell/controller-result.txt
	action_boundaries=$cell/hid-action-boundaries.tsv
	cell_metadata=$cell/metadata.txt
	mkdir -p "$cell"
	cp --reflink=auto --sparse=always "$source_boot" "$boot_copy"
	cp --reflink=auto --sparse=always "$source_auxiliary" "$auxiliary_copy"
	# Add three identical test lines beyond the source image plus a one-MiB
	# guard on each side.  Partition/filesystem probing cannot populate these
	# lines in the guest buffer cache.  Prime, concurrent target, and
	# post-hotplug verification therefore each issue a physical READ(10).
	source_auxiliary_bytes=$(wc -c <"$source_auxiliary")
	test_base_bytes=$((
	    ((source_auxiliary_bytes + 1048575) / 1048576 + 1) * 1048576))
	test_prime_skip=$((test_base_bytes / 4096))
	test_target_skip=$((test_prime_skip + 1))
	test_post_skip=$((test_prime_skip + 2))
	test_target_lba=$((test_target_skip * 8))
	test_post_lba=$((test_post_skip * 8))
	truncate -s $((test_base_bytes + 1048576)) "$auxiliary_copy"
	dd if="$source_auxiliary" of="$expected_payload" bs=4096 count=1 \
	    status=none
	if [ "$(wc -c <"$expected_payload")" -ne 4096 ]; then
		echo "$cell_kind auxiliary payload preparation was short" >&2
		return 1
	fi
	for test_seek in "$test_prime_skip" "$test_target_skip" "$test_post_skip"; do
		dd if="$expected_payload" of="$auxiliary_copy" bs=4096 \
		    seek="$test_seek" count=1 conv=notrunc status=none
	done
	auxiliary_copy_digest=$(sha256sum "$auxiliary_copy" | awk '{print $1}')
	expected_payload_digest=$(sha256sum "$expected_payload" | awk '{print $1}')
	cp "$ovmf_vars" "$vars_copy"
	: >"$guest_log"
	: >"$qemu_log"
	: >"$controller_result"
	printf 'generation\taction\tguest-line-boundary\n' >"$action_boundaries"

	case $cell_kind in
	uhci)
		topology='OVMF q35 IDE root; standalone piix3 UHCI; port 1 hotplug usb-mouse after PS/2 login; port 2 read-only usb-storage'
		hid_add='device_add usb-mouse,bus=uhci.0,port=1,id=hid,serial=hw-t25-hid,usb_version=1'
		controller_pattern='uhci: concurrent per-endpoint scheduling active'
		root_worker_pattern='uhci: root hotplug worker active'
		retirement_pattern='uhci: checked frame retirement active'
		shutdown_pattern='uhci: checked shutdown workers joined'
		expected_uhci=1
		expected_ehci=0
		;;
	paired)
		topology='OVMF q35 IDE root; ICH9 EHCI plus three UHCI companions; shared port 1 hotplug low/full-speed usb-mouse after PS/2 login; shared port 6 high-speed read-only usb-storage'
		hid_add='device_add usb-mouse,bus=ehci.0,port=1,id=hid,serial=hw-t25-hid,usb_version=1'
		controller_pattern='ehci: concurrent async/periodic scheduling active'
		root_worker_pattern='ehci: root hotplug worker active'
		retirement_pattern='ehci: checked request-local retirement active'
		shutdown_pattern='ehci: checked shutdown workers joined'
		expected_uhci=3
		expected_ehci=1
		;;
	*)
		echo "unsupported cell: $cell_kind" >&2
		return 2
		;;
	esac

	guest_failure_pattern='fatal:|kernel panic|panic:|amd64 fault v=|VFS initialization failed|Input/output error|controller quarantined|fatal IRQ status|root-port register unavailable|frame retirement failed|request retirement failed|retaining QH/TD/bounce DMA|request and DMA retained|refusing to release DMA|host controller stop failed|driver shutdown failed|teardown failed|unmatched .*completion|duplicate .*completion|stale-generation|usb-storage: .*error=[1-9]|loop[0-9]+: .*error=[1-9]|usb-hid-checkpoint: submit .*error=[1-9]|usb-hid-checkpoint: completion .*status=[0145]|usb-hid-checkpoint: completion .*drain-error=[1-9]|usb-hid-checkpoint: detach .*(drain|join)-error=[1-9]'
	login_pattern='(^|[[:blank:]])login:[[:blank:]]*$'
	password_pattern='Password:'
	shell_pattern='root@[^[:space:]]*:[^$]*\$([[:blank:]]|$)'
	attach_pattern='usb-hid-checkpoint: attach generation='
	completion_pattern='usb-hid-checkpoint: completion generation='
	detach_pattern='usb-hid-checkpoint: detach generation='
	storage_pattern='usb-storage: sd[a-z]+ blocks=[0-9]+ block-size=[0-9]+'
	storage_submit_pattern='usb-storage-checkpoint: accepted disk=sd[a-z]+ generation=[0-9]+ usb[0-9]+ device=[0-9]+ lba=[0-9]+ blocks=8 bytes=4096 status=pending'
	storage_completed_pattern='usb-storage-checkpoint: completed disk=sd[a-z]+ generation=[0-9]+ usb[0-9]+ device=[0-9]+ lba=[0-9]+ blocks=8 bytes=4096 status=2 actual=4096 error=0'
	disconnect_pattern='usb[0-9]+: device [0-9]+ port [0-9]+ disconnected'
	monitor_failure_pattern='unknown command|invalid parameter|duplicate id|device .* not found|property .* not found|Error:|"error"[[:blank:]]*:'
	cell_deadline=$(($(date +%s) + cell_timeout))

	{
		echo "cell=$cell_kind"
		echo "start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		echo "qemu=$($qemu --version | sed -n '1p')"
		echo "source_provenance=forced canonical rebuild"
		echo "source_build_config=$root/$phase_config"
		echo "source_build_config_sha256=$config_digest"
		echo "source_build_target=$canonical_image"
		echo "topology=$topology"
		echo "source_boot=$source_boot"
		echo "source_boot_sha256=$boot_digest"
		echo "source_auxiliary=$source_auxiliary"
		echo "source_auxiliary_sha256=$auxiliary_digest"
		echo "boot_copy=$boot_copy"
		echo "auxiliary_copy=$auxiliary_copy"
		echo "auxiliary_copy_mode=QEMU readonly"
		echo "auxiliary_copy_sha256=$auxiliary_copy_digest"
		echo "expected_payload_sha256=$expected_payload_digest"
		echo "storage_prime_offset=$((test_prime_skip * 4096))"
		echo "pre_hotplug_payload_offset=$((test_target_skip * 4096))"
		echo "post_hotplug_payload_offset=$((test_post_skip * 4096))"
		echo 'runtime_monitor_protocol=QMP'
		echo 'keyboard_injection=QMP input-send-event with explicit key-down/key-up pairs'
		echo "qmp_hmp_delete_command=device_del hid"
		echo "qmp_hmp_add_command=$hid_add"
		echo "qmp_hmp_hid_event_command=mouse_move 1 0"
		echo "qmp_hmp_topology_command=info usb"
		echo "hid_action_boundaries=$action_boundaries"
		echo "hotplug_cycles=11"
		echo "stage_delay_seconds=$stage_delay"
		echo "storage_throttle_read_bps=$storage_read_bps"
		echo "storage_throttle_prime_bytes=$storage_prime_bytes"
		echo "storage_throttle_nominal_delay_ms=$storage_nominal_delay_ms"
		echo "failure_oracle=$guest_failure_pattern"
		if [ "$cell_kind" = paired ]; then
			echo "runtime_hid_evidence=low/full-speed companion-UHCI interrupt-IN"
			echo "runtime_storage_evidence=high-speed EHCI control/bulk"
			echo "ehci_high_speed_periodic_runtime_evidence=not claimed"
		fi
	} >"$cell_metadata"

	parser_preflight()
	{
		parser_log=$cell/qemu-parser-preflight.log
		set +e
		if [ "$cell_kind" = uhci ]; then
			printf 'info block\ninfo usb\nquit\n' | timeout --foreground \
			    --kill-after=2 15 "$qemu" -nodefaults \
			    -machine q35,usb=off -m 64 \
			    -object throttle-group,id=aux_throttle,x-bps-read="$storage_read_bps",x-bps-read-max="$storage_read_bps",x-bps-read-max-length=1 \
			    -blockdev driver=file,filename="$auxiliary_copy",node-name=aux_file,read-only=on \
			    -blockdev driver=raw,file=aux_file,node-name=aux_raw,read-only=on \
			    -blockdev driver=throttle,throttle-group=aux_throttle,file=aux_raw,node-name=auxdisk,read-only=on \
			    -device piix3-usb-uhci,id=uhci \
			    -device usb-storage,bus=uhci.0,port=2,drive=auxdisk,id=stick,serial=hw-t25-storage \
			    -device usb-mouse,bus=uhci.0,port=1,id=hid,serial=hw-t25-hid,usb_version=1 \
			    -display none -serial none -monitor stdio \
			    >"$parser_log" 2>&1
			parser_status=$?
		else
			printf 'info block\ninfo usb\nquit\n' | timeout --foreground \
			    --kill-after=2 15 "$qemu" -nodefaults \
			    -machine q35,usb=off -m 64 \
			    -object throttle-group,id=aux_throttle,x-bps-read="$storage_read_bps",x-bps-read-max="$storage_read_bps",x-bps-read-max-length=1 \
			    -blockdev driver=file,filename="$auxiliary_copy",node-name=aux_file,read-only=on \
			    -blockdev driver=raw,file=aux_file,node-name=aux_raw,read-only=on \
			    -blockdev driver=throttle,throttle-group=aux_throttle,file=aux_raw,node-name=auxdisk,read-only=on \
			    -device ich9-usb-ehci1,id=ehci \
			    -device ich9-usb-uhci1,id=uhci1,masterbus=ehci.0,firstport=0 \
			    -device ich9-usb-uhci2,id=uhci2,masterbus=ehci.0,firstport=2 \
			    -device ich9-usb-uhci3,id=uhci3,masterbus=ehci.0,firstport=4 \
			    -device usb-storage,bus=ehci.0,port=6,drive=auxdisk,id=stick,serial=hw-t25-storage \
			    -device usb-mouse,bus=ehci.0,port=1,id=hid,serial=hw-t25-hid,usb_version=1 \
			    -display none -serial none -monitor stdio \
			    >"$parser_log" 2>&1
			parser_status=$?
		fi
		set -e
		if [ "$parser_status" -ne 0 ]; then
			echo "$cell_kind QEMU topology/parser preflight failed with status $parser_status" >&2
			return 1
		fi
		if rg -a -i -q -- "$monitor_failure_pattern" "$parser_log"; then
			echo "$cell_kind QEMU topology/parser preflight rejected an argument" >&2
			rg -a -i -m 1 -- "$monitor_failure_pattern" "$parser_log" >&2 || true
			return 1
		fi
		if ! rg -a -q -- 'auxdisk: .*\(throttle, read-only\)' \
		    "$parser_log"; then
			echo "$cell_kind QEMU preflight did not instantiate the read throttle" >&2
			return 1
		fi
		if [ "$cell_kind" = uhci ]; then
			if ! rg -a -q -- \
			    'Port 1, Speed 12 Mb/s, Product QEMU USB Mouse, ID: hid' \
			    "$parser_log" || ! rg -a -q -- \
			    'Port 2, Speed 12 Mb/s, Product QEMU USB MSD, ID: stick' \
			    "$parser_log"; then
				echo 'standalone UHCI parser preflight topology mismatch' >&2
				return 1
			fi
		else
			if ! rg -a -q -- \
			    'Port 1, Speed 12 Mb/s, Product QEMU USB Mouse, ID: hid' \
			    "$parser_log" || ! rg -a -q -- \
			    'Port 6, Speed 480 Mb/s, Product QEMU USB MSD, ID: stick' \
			    "$parser_log"; then
				echo 'paired EHCI/UHCI parser preflight topology mismatch' >&2
				return 1
			fi
		fi
		{
			echo 'qemu_topology_parser_preflight=pass'
			echo "qemu_topology_parser_log=$parser_log"
		} >>"$cell_metadata"
		return 0
	}

	parser_preflight || return 1

	first_unexpected_enumeration_failure()
	{
		enumeration_lines=$(rg -a -- \
		    '^usb[0-9]+: port [0-9]+ enumeration failed [(][0-9]+[)]\r?$' \
		    "$guest_log" 2>/dev/null || true)
		if [ -z "$enumeration_lines" ]; then
			return 0
		fi
		if [ "$cell_kind" != paired ]; then
			printf '%s\n' "$enumeration_lines" | sed -n '1p'
			return 0
		fi
		ehci_bus=$(rg -a -m 1 -o '^usb[0-9]+: EHCI,' "$guest_log" \
		    2>/dev/null | sed 's/^usb//; s/: EHCI,$//' || true)
		case $ehci_bus in
		'' | *[!0-9]*)
			printf '%s\n' "$enumeration_lines" | sed -n '1p'
			;;
		*)
			# QEMU's full/low-speed device first appears on EHCI port 1.
			# EHCI returns ENODEV (13) after setting PORT_OWNER, then the
			# companion UHCI enumerates it.  Only that exact handoff is
			# non-fatal; verify_uhci_owner() proves every resulting attach.
			printf '%s\n' "$enumeration_lines" | awk \
			    -v allowed="usb$ehci_bus: port 1 enumeration failed (13)" \
			    '{ sub(/\r$/, ""); if ($0 != allowed) { print; exit } }'
			;;
		esac
	}

	first_guest_failure()
	{
		failure=$(rg -a -m 1 -- "$guest_failure_pattern" "$guest_log" \
		    2>/dev/null || true)
		if [ -z "$failure" ]; then
			failure=$(first_unexpected_enumeration_failure)
		fi
		# Preserve the empty sentinel.  A terminating printf newline would
		# become one space through tr and make every healthy poll look fatal.
		printf '%s' "$failure" | tr '\t\r\n' '   '
	}

	fail_controller()
	{
		fail_label=$1
		printf 'fail\t%s\n' "$fail_label" >"$controller_result"
		return 1
	}

	wait_for_count()
	{
		wait_pattern=$1
		wait_minimum=$2
		wait_seconds=$3
		wait_label=$4
		wait_deadline=$(($(date +%s) + wait_seconds))
		if [ "$cell_deadline" -lt "$wait_deadline" ]; then
			wait_deadline=$cell_deadline
		fi
		while :; do
			wait_failure=$(first_guest_failure)
			if [ -n "$wait_failure" ]; then
				fail_controller "$wait_label: $wait_failure"
				return 1
			fi
			wait_count=$(marker_count "$wait_pattern" "$guest_log")
			if [ "$wait_count" -ge "$wait_minimum" ]; then
				return 0
			fi
			if [ "$(date +%s)" -ge "$wait_deadline" ]; then
				fail_controller \
				    "$wait_label timeout ($wait_count/$wait_minimum markers)"
				return 1
			fi
			sleep 0.02
		done
	}

	wait_for_line_after()
	{
		wait_pattern=$1
		wait_after=$2
		wait_seconds=$3
		wait_label=$4
		wait_deadline=$(($(date +%s) + wait_seconds))
		if [ "$cell_deadline" -lt "$wait_deadline" ]; then
			wait_deadline=$cell_deadline
		fi
		while :; do
			wait_failure=$(first_guest_failure)
			if [ -n "$wait_failure" ]; then
				fail_controller "$wait_label: $wait_failure"
				return 1
			fi
			wait_match_line=$(rg -a -n -- "$wait_pattern" "$guest_log" \
			    2>/dev/null | awk -F: -v after="$wait_after" \
			    '$1 > after { print $1; exit }' || true)
			if [ -n "$wait_match_line" ]; then
				return 0
			fi
			if [ "$(date +%s)" -ge "$wait_deadline" ]; then
				fail_controller "$wait_label timeout after line $wait_after"
				return 1
			fi
			sleep 0.02
			done
	}

	guest_line_count()
	{
		wc -l <"$guest_log" | tr -d '[:blank:]'
	}

	qmp_hmp()
	{
		qmp_hmp_command=$1
		qmp_hmp_escaped=$(printf '%s' "$qmp_hmp_command" |
		    sed 's/\\/\\\\/g; s/"/\\"/g')
		printf '%s\n' \
		    "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"$qmp_hmp_escaped\"}}"
	}

	qmp_send_key()
	{
		qmp_key=$1
		qmp_shifted=$2
		if [ "$qmp_shifted" = yes ]; then
			printf '%s\n' \
			    "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":true,\"key\":{\"type\":\"qcode\",\"data\":\"shift\"}}},{\"type\":\"key\",\"data\":{\"down\":true,\"key\":{\"type\":\"qcode\",\"data\":\"$qmp_key\"}}},{\"type\":\"key\",\"data\":{\"down\":false,\"key\":{\"type\":\"qcode\",\"data\":\"$qmp_key\"}}},{\"type\":\"key\",\"data\":{\"down\":false,\"key\":{\"type\":\"qcode\",\"data\":\"shift\"}}}]}}"
		else
			printf '%s\n' \
			    "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":true,\"key\":{\"type\":\"qcode\",\"data\":\"$qmp_key\"}}},{\"type\":\"key\",\"data\":{\"down\":false,\"key\":{\"type\":\"qcode\",\"data\":\"$qmp_key\"}}}]}}"
		fi
		# The down/up pair is one ordered QMP command.  This delay only paces
		# the guest keyboard; it is not responsible for releasing the key.
		sleep "$key_delay"
	}

	qmp_release_modifiers()
	{
		# Every key above already has an explicit key-up.  These idempotent
		# releases also recover cleanly if a long command is interrupted.
		printf '%s\n' \
		    '{"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"down":false,"key":{"type":"qcode","data":"shift"}}},{"type":"key","data":{"down":false,"key":{"type":"qcode","data":"ctrl"}}},{"type":"key","data":{"down":false,"key":{"type":"qcode","data":"alt"}}}]}}'
	}

	send_text()
	{
		send_value=$1
		while [ -n "$send_value" ]; do
			send_character=$(printf '%s' "$send_value" | cut -c 1)
			send_value=$(printf '%s' "$send_value" | cut -c 2-)
			send_shifted=no
			case $send_character in
			[a-z0-9]) send_key=$send_character ;;
			[A-Z])
				send_lower=$(printf '%s' "$send_character" |
				    tr 'A-Z' 'a-z')
				send_key=$send_lower
				send_shifted=yes
				;;
			' ') send_key=spc ;;
			/) send_key=slash ;;
			-) send_key=minus ;;
			_) send_key=minus; send_shifted=yes ;;
			.) send_key=dot ;;
			=) send_key=equal ;;
			';') send_key=semicolon ;;
			'&') send_key=7; send_shifted=yes ;;
			'$') send_key=4; send_shifted=yes ;;
			'?') send_key=slash; send_shifted=yes ;;
			*)
				echo "unsupported sendkey character: $send_character" >&2
				qmp_release_modifiers
				return 1
				;;
			esac
			qmp_send_key "$send_key" "$send_shifted"
		done
		qmp_send_key ret no
		qmp_release_modifiers
	}

	send_shell()
	{
		shell_command=$1
		shell_before=$(marker_count "$shell_pattern" "$guest_log")
		send_text "$shell_command" || return 1
		wait_for_count "$shell_pattern" $((shell_before + 1)) \
		    "$command_timeout" "guest command: $shell_command"
	}

	verify_uhci_owner()
	{
		owner_generation=$1
		owner_line=$(rg -a -m 1 -- \
		    "usb-hid-checkpoint: attach generation=$owner_generation " \
		    "$guest_log" 2>/dev/null || true)
		owner_bus=$(printf '%s\n' "$owner_line" |
		    sed -n 's/.* usb\([0-9][0-9]*\) device=.*/\1/p')
		case $owner_bus in
		'' | *[!0-9]*)
			fail_controller "generation $owner_generation has no parseable USB owner"
			return 1
			;;
		esac
		if ! rg -a -q -- "usb$owner_bus: UHCI, 2 root ports" "$guest_log"; then
			fail_controller \
			    "generation $owner_generation was not owned by companion UHCI usb$owner_bus"
			return 1
		fi
		return 0
	}

	wait_generation_attach()
	{
		generation=$1
		wait_for_count \
		    "usb-hid-checkpoint: attach generation=$generation " 1 \
		    "$hotplug_timeout" "HID attach generation $generation" || return 1
		wait_for_count \
		    "usb-hid-checkpoint: submit generation=$generation .*error=0" 1 \
		    "$hotplug_timeout" "HID submit generation $generation" || return 1
		verify_uhci_owner "$generation"
	}

	wait_generation_completion()
	{
		generation=$1
		completion_before_line=$(guest_line_count)
		# A relative pointer event produces a real HID interrupt report without
		# sharing key state with the PS/2 keyboard used to control the shell.
		qmp_hmp 'mouse_move 1 0'
		wait_for_line_after \
		    "usb-hid-checkpoint: completion generation=$generation .*status=2 actual=[1-9][0-9]* drain-error=0" \
		    "$completion_before_line" "$command_timeout" \
		    "HID completion generation $generation" || return 1
		generation_completion_line=$wait_match_line
		sleep 0.05
	}

	wait_generation_detach()
	{
		generation=$1
		disconnect_before=$(marker_count "$disconnect_pattern" "$guest_log")
		device_del_boundary=$(guest_line_count)
		preexisting_terminal=$(rg -a -n -- \
		    "usb-hid-checkpoint: completion generation=$generation .*status=[367] " \
		    "$guest_log" 2>/dev/null | awk -F: \
		    -v boundary="$device_del_boundary" \
		    '$1 <= boundary { print $1; exit }' || true)
		if [ -n "$preexisting_terminal" ]; then
			fail_controller \
			    "generation $generation terminal error preceded device_del boundary"
			return 1
		fi
		printf '%s\tdevice-del\t%s\n' "$generation" \
		    "$device_del_boundary" >>"$action_boundaries"
		qmp_hmp 'device_del hid'
		wait_for_count \
		    "usb-hid-checkpoint: detach generation=$generation .*cancel-error=(0|17) drain-error=0 join-error=0" \
		    1 "$hotplug_timeout" "HID detach generation $generation" || return 1
		wait_for_count "$disconnect_pattern" $((disconnect_before + 1)) \
		    "$hotplug_timeout" "USB disconnect generation $generation"
	}

	controller_body()
	{
		if [ "$cell_kind" = uhci ]; then
			wait_for_count "$controller_pattern" 1 "$boot_timeout" \
			    'standalone UHCI concurrent marker' || return 1
			wait_for_count "$root_worker_pattern" 1 "$boot_timeout" \
			    'standalone UHCI root worker' || return 1
		else
			wait_for_count "$controller_pattern" 1 "$boot_timeout" \
			    'paired EHCI concurrent marker' || return 1
			wait_for_count "$root_worker_pattern" 1 "$boot_timeout" \
			    'paired EHCI root worker' || return 1
			wait_for_count 'uhci: concurrent per-endpoint scheduling active' \
			    3 "$boot_timeout" 'three companion UHCI controllers' || return 1
			wait_for_count 'uhci: root hotplug worker active' 3 \
			    "$boot_timeout" 'three companion UHCI root workers' || return 1
		fi
		wait_for_count "$storage_pattern" 1 "$boot_timeout" \
		    'independent USB storage registration' || return 1
		wait_for_count "$login_pattern" 1 "$boot_timeout" 'login prompt' || return 1

		password_before=$(marker_count "$password_pattern" "$guest_log")
		login_shell_before=$(marker_count "$shell_pattern" "$guest_log")
		send_text root || return 1
		wait_for_count "$password_pattern" $((password_before + 1)) \
		    "$command_timeout" 'password prompt' || return 1
		send_text '' || return 1
		wait_for_count "$shell_pattern" $((login_shell_before + 1)) \
		    "$command_timeout" 'root shell prompt' || return 1

		# Keep the USB HID checkpoint absent while logging in.  Keyboard input
		# is injected through PS/2 with explicit QMP key-down/key-up pairs;
		# every runtime USB generation exists only for HCD evidence.
		qmp_hmp "$hid_add"
		wait_generation_attach 1 || return 1
		# Persist QEMU's negotiated-speed view.  In the paired cell this is
		# the independent evidence that the HID mouse was handed to a UHCI
		# companion while storage remained on high-speed EHCI.
		qmp_hmp 'info usb'

		# Explicit post-login completion before storage I/O.
		wait_generation_completion 1 || return 1
		wait_generation_detach 1 || return 1

		disk=$(rg -a -o 'usb-storage: sd[a-z]+ blocks=' "$guest_log" |
		    tail -n 1 | sed 's/^usb-storage: //; s/ blocks=$//')
		case $disk in
		sd[a-z]*) ;;
		*) fail_controller 'cannot identify the USB storage disk'; return 1 ;;
		esac
		stage_command="echo HW-T25-STAGED; sleep $stage_delay; /bin/dd if=/dev/$disk of=/tmp/hw-t25-prime.bin bs=4096 skip=$test_prime_skip count=1 && echo HW-T25-THROTTLE-PRIMED && echo HW-T25-DD-START && /bin/dd if=/dev/$disk of=/tmp/hw-t25-before.bin bs=4096 skip=$test_target_skip count=1 && echo HW-T25-DD-DONE"
		echo "guest_stage_command=$stage_command" >>"$cell_metadata"
		stage_shell_before=$(marker_count "$shell_pattern" "$guest_log")
		send_text "$stage_command" || return 1
		wait_for_count '^HW-T25-STAGED\r?$' 1 "$command_timeout" \
		    'staged storage command' || return 1
		# Cycle 1: reinsert while the guest delays the second, uncached 4-KiB
		# cache line.  Do not inject HID merely after DD-START: first require
		# proof that the HCD accepted the READ(10) data URB as pending.
		qmp_hmp "$hid_add"
		wait_generation_attach 2 || return 1
		wait_for_count '^HW-T25-THROTTLE-PRIMED\r?$' 1 \
		    $((stage_delay + command_timeout)) \
		    '4-KiB throttle priming read' || return 1
		wait_for_count '^HW-T25-DD-START\r?$' 1 \
		    $((stage_delay + command_timeout)) '4-KiB read start' || return 1
		start_line=$(rg -a -n '^HW-T25-DD-START\r?$' "$guest_log" |
		    tail -n 1 | cut -d: -f1)
		wait_for_line_after \
		    "usb-storage-checkpoint: accepted disk=$disk generation=[0-9]+ usb[0-9]+ device=[0-9]+ lba=$test_target_lba blocks=8 bytes=4096 status=pending" \
		    "$start_line" "$command_timeout" \
		    'pending READ(10) data URB after DD-START' || return 1
		storage_submit_line=$wait_match_line
		storage_sequence=$(sed -n "${storage_submit_line}p" "$guest_log" |
		    sed -n 's/.* generation=\([0-9][0-9]*\) usb.*/\1/p')
		case $storage_sequence in
		'' | *[!0-9]*)
			fail_controller 'cannot parse storage checkpoint generation'
			return 1
			;;
		esac
		during_before=$(marker_count \
		    'usb-hid-checkpoint: completion generation=2 ' "$guest_log")
		qmp_hmp 'mouse_move 1 0'
		wait_for_count \
		    'usb-hid-checkpoint: completion generation=2 .*status=2 actual=[1-9][0-9]* drain-error=0' \
		    $((during_before + 1)) "$command_timeout" \
		    'HID completion during 4-KiB read' || return 1
		during_line=$(rg -a -n \
		    'usb-hid-checkpoint: completion generation=2 .*status=2 .*drain-error=0' \
		    "$guest_log" | awk -F: -v submit="$storage_submit_line" \
		    '$1 > submit { print $1; exit }')
		if [ -z "$during_line" ]; then
			fail_controller \
			    'HID completion did not follow pending storage submit'
			return 1
		fi
		wait_for_line_after \
		    "usb-storage-checkpoint: completed disk=$disk generation=$storage_sequence usb[0-9]+ device=[0-9]+ lba=$test_target_lba blocks=8 bytes=4096 status=2 actual=4096 error=0" \
		    "$during_line" "$command_timeout" \
		    'READ(10) completion after concurrent HID completion' || return 1
		storage_completed_line=$wait_match_line
		wait_for_count '^HW-T25-DD-DONE\r?$' 1 "$command_timeout" \
		    '4-KiB read completion' || return 1
		wait_for_count "$shell_pattern" $((stage_shell_before + 1)) \
		    "$command_timeout" 'staged read shell return' || return 1

		done_line=$(rg -a -n '^HW-T25-DD-DONE\r?$' "$guest_log" |
		    tail -n 1 | cut -d: -f1)
		if [ "$storage_completed_line" -ge "$done_line" ]; then
			fail_controller \
			    'storage completion was not ordered before DD-DONE'
			return 1
		fi
		echo "dd_start_line=$start_line" >>"$cell_metadata"
		echo "storage_submit_line=$storage_submit_line" >>"$cell_metadata"
		echo "storage_checkpoint_generation=$storage_sequence" >>"$cell_metadata"
		echo "hid_during_dd_line=$during_line" >>"$cell_metadata"
		echo "storage_completed_line=$storage_completed_line" >>"$cell_metadata"
		echo "dd_done_line=$done_line" >>"$cell_metadata"

		# Explicit completion after the first storage read.
		wait_generation_completion 2 || return 1
		generation=2
		cycle=2
		while [ "$cycle" -le 10 ]; do
			wait_generation_detach "$generation" || return 1
			qmp_hmp "$hid_add"
			generation=$((generation + 1))
			wait_generation_attach "$generation" || return 1
			wait_generation_completion "$generation" || return 1
			cycle=$((cycle + 1))
		done
		# Release the final HID owner before entering more shell text so those
		# monitor key events are delivered through the PS/2 console path.
		wait_generation_detach "$generation" || return 1

		send_shell "echo HW-T25-REREAD-START && /bin/dd if=/dev/$disk of=/tmp/hw-t25-after.bin bs=4096 skip=$test_post_skip count=1 && echo HW-T25-REREAD-OK" || return 1
		wait_for_count '^HW-T25-REREAD-OK\r?$' 1 "$command_timeout" \
		    'post-hotplug storage re-read' || return 1
		reread_start_line=$(rg -a -n '^HW-T25-REREAD-START\r?$' "$guest_log" |
		    tail -n 1 | cut -d: -f1)
		wait_for_line_after \
		    "usb-storage-checkpoint: accepted disk=$disk generation=[0-9]+ usb[0-9]+ device=[0-9]+ lba=$test_post_lba blocks=8 bytes=4096 status=pending" \
		    "$reread_start_line" "$command_timeout" \
		    'fresh post-hotplug READ(10) submit' || return 1
		reread_submit_line=$wait_match_line
		reread_sequence=$(sed -n "${reread_submit_line}p" "$guest_log" |
		    sed -n 's/.* generation=\([0-9][0-9]*\) usb.*/\1/p')
		case $reread_sequence in
		'' | *[!0-9]*)
			fail_controller 'cannot parse post-hotplug storage generation'
			return 1
			;;
		esac
		wait_for_line_after \
		    "usb-storage-checkpoint: completed disk=$disk generation=$reread_sequence usb[0-9]+ device=[0-9]+ lba=$test_post_lba blocks=8 bytes=4096 status=2 actual=4096 error=0" \
		    "$reread_submit_line" "$command_timeout" \
		    'fresh post-hotplug READ(10) completion' || return 1
		send_shell '/bin/cmp /tmp/hw-t25-before.bin /tmp/hw-t25-after.bin && echo HW-T25-COMPARE-OK' || return 1
		wait_for_count '^HW-T25-COMPARE-OK\r?$' 1 "$command_timeout" \
		    'storage payload comparison' || return 1
		wait_for_count "$retirement_pattern" 1 "$command_timeout" \
		    "$cell_kind checked retirement" || return 1
		if [ "$cell_kind" = paired ]; then
			wait_for_count 'uhci: checked frame retirement active' 1 \
			    "$command_timeout" 'companion UHCI checked retirement' || return 1
		fi

		shutdown_before=$(marker_count "$shutdown_pattern" "$guest_log")
		uhci_shutdown_before=$(marker_count \
		    'uhci: checked shutdown workers joined' "$guest_log")
		# The final generation completes one report and must then re-arm
		# sequence 2.  Only after that exact submit is observed do we request
		# reboot.  This removes the former fixed-sleep race and proves that the
		# shutdown path, rather than ordinary device_del, owns its pending URB.
		qmp_hmp "$hid_add"
		generation=$((generation + 1))
		wait_generation_attach "$generation" || return 1
		wait_generation_completion "$generation" || return 1
		final_completion_line=$generation_completion_line
		if ! sed -n "${final_completion_line}p" "$guest_log" | rg -q -- \
		    "usb-hid-checkpoint: completion generation=$generation .*sequence=1 status=2 "; then
			fail_controller \
			    "final generation $generation did not complete sequence 1"
			return 1
		fi
		wait_for_line_after \
		    "usb-hid-checkpoint: submit generation=$generation .*sequence=2 error=0" \
		    "$final_completion_line" "$command_timeout" \
		    'final HID re-arm after successful completion' || return 1
		final_rearm_submit_line=$wait_match_line
		pre_reboot_terminal=$(rg -a -n -- \
		    "usb-hid-checkpoint: completion generation=$generation .*status=[367] " \
		    "$guest_log" 2>/dev/null | awk -F: \
		    -v boundary="$final_rearm_submit_line" \
		    '$1 > boundary { print $1; exit }' || true)
		if [ -n "$pre_reboot_terminal" ]; then
			fail_controller \
			    "final generation $generation terminated before reboot"
			return 1
		fi
		reboot_command_boundary=$(guest_line_count)
		send_text reboot || return 1
		wait_for_line_after 'init: executing system action reboot' \
		    "$reboot_command_boundary" "$command_timeout" 'normal reboot' || return 1
		reboot_action_line=$wait_match_line
		pre_action_terminal=$(rg -a -n -- \
		    "usb-hid-checkpoint: completion generation=$generation .*status=[367] " \
		    "$guest_log" 2>/dev/null | awk -F: \
		    -v boundary="$reboot_action_line" \
		    '$1 <= boundary { print $1; exit }' || true)
		if [ -n "$pre_action_terminal" ]; then
			fail_controller \
			    "final generation $generation terminal error preceded reboot boundary"
			return 1
		fi
		printf '%s\treboot\t%s\n' "$generation" "$reboot_action_line" \
		    >>"$action_boundaries"
		# checkpoint_detach() closes and joins its reporter worker before it
		# cancels/drains the URB.  The cancel callback may therefore publish a
		# terminal state without a completion marker, and a failed dequeue may
		# also race a valid natural COMPLETE.  The USB API's ownership proof is
		# the successful drain recorded by the clean detach, not a particular
		# terminal status or an optional reporter marker.
		wait_for_line_after \
		    "usb-hid-checkpoint: detach generation=$generation .*cancel-error=(0|17) drain-error=0 join-error=0" \
		    "$reboot_action_line" "$command_timeout" \
		    'final HID clean detach after pending reboot boundary' || return 1
		final_detach_line=$wait_match_line
		final_terminal_line=$(rg -a -n -- \
		    "usb-hid-checkpoint: completion generation=$generation .*sequence=2 status=[367] " \
		    "$guest_log" 2>/dev/null | awk -F: \
		    -v reboot="$reboot_action_line" -v detach="$final_detach_line" \
		    '$1 > reboot && $1 < detach { line = $1 } END { print line }' || true)
		wait_for_count "$shutdown_pattern" $((shutdown_before + 1)) \
		    "$command_timeout" "$cell_kind HCD shutdown worker joins" || return 1
		if [ "$cell_kind" = paired ]; then
			wait_for_count 'uhci: checked shutdown workers joined' \
			    $((uhci_shutdown_before + 3)) "$command_timeout" \
			    'three companion UHCI shutdown worker joins' || return 1
		fi
		{
			echo "final_hid_completion_line=$final_completion_line"
			echo "final_hid_rearm_submit_line=$final_rearm_submit_line"
			echo "reboot_action_line=$reboot_action_line"
			echo "final_hid_terminal_line=${final_terminal_line:-not-reported}"
			echo "final_hid_detach_line=$final_detach_line"
		} >>"$cell_metadata"
		printf 'pass\tgenerations=12 cycles=11 final-generation=%s\n' \
		    "$generation" >"$controller_result"
		return 0
	}

	controller_driver()
	{
		trap '' PIPE
		set +e
		printf '%s\n' '{"execute":"qmp_capabilities"}'
		controller_body
		controller_status=$?
		if [ "$controller_status" -ne 0 ]; then
			if [ ! -s "$controller_result" ]; then
				fail_controller 'controller exited without detailed evidence'
			fi
			qmp_hmp quit
		fi
		return "$controller_status"
	}

	set +e
	if [ "$cell_kind" = uhci ]; then
		echo "qemu_command=$qemu -machine q35,usb=off -m 512 -smp 2 -drive if=pflash,format=raw,readonly=on,file='$ovmf_code' -drive if=pflash,format=raw,file='$vars_copy' -device piix3-ide,id=legacyide -drive if=none,id=bootdisk,file='$boot_copy',format=raw -device ide-hd,bus=legacyide.0,drive=bootdisk,bootindex=1 -device piix3-usb-uhci,id=uhci -object throttle-group,id=aux_throttle,x-bps-read=$storage_read_bps,x-bps-read-max=$storage_read_bps,x-bps-read-max-length=1 -blockdev driver=file,filename='$auxiliary_copy',node-name=aux_file,read-only=on -blockdev driver=raw,file=aux_file,node-name=aux_raw,read-only=on -blockdev driver=throttle,throttle-group=aux_throttle,file=aux_raw,node-name=auxdisk,read-only=on -device usb-storage,bus=uhci.0,port=2,drive=auxdisk,id=stick,serial=hw-t25-storage -display none -serial none -debugcon file:'$guest_log' -qmp stdio -no-reboot" >>"$cell_metadata"
		controller_driver | timeout --foreground --kill-after=5 \
		    "${cell_timeout}s" "$qemu" \
		    -machine q35,usb=off -m 512 -smp 2 \
		    -drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
		    -drive if=pflash,format=raw,file="$vars_copy" \
		    -device piix3-ide,id=legacyide \
		    -drive if=none,id=bootdisk,file="$boot_copy",format=raw \
		    -device ide-hd,bus=legacyide.0,drive=bootdisk,bootindex=1 \
		    -device piix3-usb-uhci,id=uhci \
		    -object throttle-group,id=aux_throttle,x-bps-read="$storage_read_bps",x-bps-read-max="$storage_read_bps",x-bps-read-max-length=1 \
		    -blockdev driver=file,filename="$auxiliary_copy",node-name=aux_file,read-only=on \
		    -blockdev driver=raw,file=aux_file,node-name=aux_raw,read-only=on \
		    -blockdev driver=throttle,throttle-group=aux_throttle,file=aux_raw,node-name=auxdisk,read-only=on \
		    -device usb-storage,bus=uhci.0,port=2,drive=auxdisk,id=stick,serial=hw-t25-storage \
		    -display none -serial none -debugcon "file:$guest_log" \
		    -qmp stdio -no-reboot >"$qemu_log" 2>&1
		qemu_status=$?
	else
		echo "qemu_command=$qemu -machine q35,usb=off -m 512 -smp 2 -drive if=pflash,format=raw,readonly=on,file='$ovmf_code' -drive if=pflash,format=raw,file='$vars_copy' -device piix3-ide,id=legacyide -drive if=none,id=bootdisk,file='$boot_copy',format=raw -device ide-hd,bus=legacyide.0,drive=bootdisk,bootindex=1 -device ich9-usb-ehci1,id=ehci -device ich9-usb-uhci1,id=uhci1,masterbus=ehci.0,firstport=0 -device ich9-usb-uhci2,id=uhci2,masterbus=ehci.0,firstport=2 -device ich9-usb-uhci3,id=uhci3,masterbus=ehci.0,firstport=4 -object throttle-group,id=aux_throttle,x-bps-read=$storage_read_bps,x-bps-read-max=$storage_read_bps,x-bps-read-max-length=1 -blockdev driver=file,filename='$auxiliary_copy',node-name=aux_file,read-only=on -blockdev driver=raw,file=aux_file,node-name=aux_raw,read-only=on -blockdev driver=throttle,throttle-group=aux_throttle,file=aux_raw,node-name=auxdisk,read-only=on -device usb-storage,bus=ehci.0,port=6,drive=auxdisk,id=stick,serial=hw-t25-storage -display none -serial none -debugcon file:'$guest_log' -qmp stdio -no-reboot" >>"$cell_metadata"
		controller_driver | timeout --foreground --kill-after=5 \
		    "${cell_timeout}s" "$qemu" \
		    -machine q35,usb=off -m 512 -smp 2 \
		    -drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
		    -drive if=pflash,format=raw,file="$vars_copy" \
		    -device piix3-ide,id=legacyide \
		    -drive if=none,id=bootdisk,file="$boot_copy",format=raw \
		    -device ide-hd,bus=legacyide.0,drive=bootdisk,bootindex=1 \
		    -device ich9-usb-ehci1,id=ehci \
		    -device ich9-usb-uhci1,id=uhci1,masterbus=ehci.0,firstport=0 \
		    -device ich9-usb-uhci2,id=uhci2,masterbus=ehci.0,firstport=2 \
		    -device ich9-usb-uhci3,id=uhci3,masterbus=ehci.0,firstport=4 \
		    -object throttle-group,id=aux_throttle,x-bps-read="$storage_read_bps",x-bps-read-max="$storage_read_bps",x-bps-read-max-length=1 \
		    -blockdev driver=file,filename="$auxiliary_copy",node-name=aux_file,read-only=on \
		    -blockdev driver=raw,file=aux_file,node-name=aux_raw,read-only=on \
		    -blockdev driver=throttle,throttle-group=aux_throttle,file=aux_raw,node-name=auxdisk,read-only=on \
		    -device usb-storage,bus=ehci.0,port=6,drive=auxdisk,id=stick,serial=hw-t25-storage \
		    -display none -serial none -debugcon "file:$guest_log" \
		    -qmp stdio -no-reboot >"$qemu_log" 2>&1
		qemu_status=$?
	fi
	set -e

	if [ "$qemu_status" -ne 0 ]; then
		echo "$cell_kind QEMU exited with status $qemu_status" >&2
		return 1
	fi
	if [ ! -s "$controller_result" ] ||
	    ! rg -q '^pass\t' "$controller_result"; then
		echo "$cell_kind controller failed: $(tr '\t\r\n' '   ' <"$controller_result")" >&2
		return 1
	fi
	if rg -a -i -q -- "$monitor_failure_pattern" "$qemu_log"; then
		echo "$cell_kind QEMU monitor rejected a command" >&2
		rg -a -i -m 1 -- "$monitor_failure_pattern" "$qemu_log" >&2 || true
		return 1
	fi
	# A physical disconnect can make UHCI publish CANCELLED, DISCONNECTED,
	# or IO_ERROR before the polling root worker observes CCS clear.  Such a
	# terminal is valid only after the controller recorded the device_del or
	# reboot boundary for that generation and before its clean detach.  This
	# prevents a pre-existing terminal failure from being hidden by a later
	# clean-detach exception.
	action_boundary_count=$(awk 'NR > 1 { count++ } END { print count + 0 }' \
	    "$action_boundaries")
	if [ "$action_boundary_count" -ne 12 ]; then
		echo "$cell_kind expected 12 HID action boundaries, got $action_boundary_count" >&2
		return 1
	fi
	disconnect_terminal_count=0
	disconnect_terminal_generations=$(sed -n \
	    's/.*usb-hid-checkpoint: completion generation=\([0-9][0-9]*\).*status=[367] .*/\1/p' \
	    "$guest_log" | sort -u)
	for terminal_generation in $disconnect_terminal_generations; do
		boundary_records=$(awk -F '\t' -v generation="$terminal_generation" \
		    'NR > 1 && $1 == generation { print $2 ":" $3 }' \
		    "$action_boundaries")
		boundary_record_count=$(printf '%s\n' "$boundary_records" |
		    awk 'NF { count++ } END { print count + 0 }')
		if [ "$boundary_record_count" -ne 1 ]; then
			echo "$cell_kind HID terminal generation $terminal_generation has $boundary_record_count action boundaries" >&2
			return 1
		fi
		terminal_action=${boundary_records%%:*}
		terminal_boundary=${boundary_records#*:}
		case $terminal_boundary in
		'' | *[!0-9]*)
			echo "$cell_kind malformed HID action boundary for generation $terminal_generation" >&2
			return 1
			;;
		esac
		first_terminal_line=$(rg -a -n -- \
		    "usb-hid-checkpoint: completion generation=$terminal_generation .*status=[367] " \
		    "$guest_log" | sed -n '1s/:.*//p')
		last_terminal_line=$(rg -a -n -- \
		    "usb-hid-checkpoint: completion generation=$terminal_generation .*status=[367] " \
		    "$guest_log" | tail -n 1 | cut -d: -f1)
		detach_line=$(rg -a -n -- \
		    "usb-hid-checkpoint: detach generation=$terminal_generation .*cancel-error=(0|17) drain-error=0 join-error=0" \
		    "$guest_log" | tail -n 1 | cut -d: -f1)
		if [ -z "$first_terminal_line" ] || [ -z "$last_terminal_line" ] ||
		    [ -z "$detach_line" ] ||
		    [ "$first_terminal_line" -le "$terminal_boundary" ] ||
		    [ "$detach_line" -le "$last_terminal_line" ]; then
			echo "$cell_kind HID error terminal escaped $terminal_action boundary/clean detach generation $terminal_generation" >&2
			return 1
		fi
		disconnect_terminal_count=$((disconnect_terminal_count + 1))
	done
	final_generation=12
	final_success_line=$(rg -a -n -- \
	    "usb-hid-checkpoint: completion generation=$final_generation .*sequence=1 status=2 " \
	    "$guest_log" 2>/dev/null | tail -n 1 | cut -d: -f1 || true)
	final_rearm_line=$(rg -a -n -- \
	    "usb-hid-checkpoint: submit generation=$final_generation .*sequence=2 error=0" \
	    "$guest_log" 2>/dev/null | tail -n 1 | cut -d: -f1 || true)
	final_reboot_boundary=$(awk -F '\t' -v generation="$final_generation" \
	    'NR > 1 && $1 == generation && $2 == "reboot" { print $3; exit }' \
	    "$action_boundaries")
	final_terminal_line=$(rg -a -n -- \
	    "usb-hid-checkpoint: completion generation=$final_generation .*sequence=2 status=[367] " \
	    "$guest_log" 2>/dev/null | tail -n 1 | cut -d: -f1 || true)
	final_detach_line=$(rg -a -n -- \
	    "usb-hid-checkpoint: detach generation=$final_generation .*cancel-error=(0|17) drain-error=0 join-error=0" \
	    "$guest_log" 2>/dev/null | tail -n 1 | cut -d: -f1 || true)
	final_pre_reboot_completion=$(rg -a -n -- \
	    "usb-hid-checkpoint: completion generation=$final_generation .*sequence=2 status=2 " \
	    "$guest_log" 2>/dev/null | awk -F: \
	    -v submit="$final_rearm_line" -v reboot="$final_reboot_boundary" \
	    '$1 > submit && $1 <= reboot { print $1; exit }' || true)
	for evidence_line in "$final_success_line" "$final_rearm_line" \
	    "$final_reboot_boundary" "$final_detach_line"; do
		case $evidence_line in
		'' | *[!0-9]*)
			echo "$cell_kind final HID pending-shutdown evidence is incomplete" >&2
			return 1
			;;
		esac
	done
	if [ -n "$final_pre_reboot_completion" ] ||
	    [ "$final_rearm_line" -le "$final_success_line" ] ||
	    [ "$final_reboot_boundary" -le "$final_rearm_line" ] ||
	    [ "$final_detach_line" -le "$final_reboot_boundary" ]; then
		echo "$cell_kind final HID was not pending across the reboot boundary" >&2
		return 1
	fi
	if [ "$cell_kind" = paired ]; then
		if ! rg -a -q -- \
		    'Port 1, Speed 12 Mb/s, Product QEMU USB Mouse, ID: hid' \
		    "$qemu_log" ||
		    ! rg -a -q -- \
		    'Port 6, Speed 480 Mb/s, Product QEMU USB MSD, ID: stick' \
		    "$qemu_log"; then
			echo 'paired QEMU topology did not prove UHCI HID handoff and high-speed EHCI storage' >&2
			return 1
		fi
	fi
	final_failure=$(first_guest_failure)
	if [ -n "$final_failure" ]; then
		echo "$cell_kind guest reported a lifecycle failure" >&2
		printf '%s\n' "$final_failure" >&2
		return 1
	fi

	uhci_count=$(marker_count \
	    'uhci: concurrent per-endpoint scheduling active' "$guest_log")
	uhci_root_count=$(marker_count 'uhci: root hotplug worker active' \
	    "$guest_log")
	ehci_count=$(marker_count \
	    'ehci: concurrent async/periodic scheduling active' "$guest_log")
	ehci_root_count=$(marker_count 'ehci: root hotplug worker active' \
	    "$guest_log")
	attach_count=$(marker_count "$attach_pattern" "$guest_log")
	detach_count=$(marker_count "$detach_pattern" "$guest_log")
	completion_count=$(marker_count "$completion_pattern" "$guest_log")
	storage_count=$(marker_count "$storage_pattern" "$guest_log")
	storage_submit_count=$(marker_count "$storage_submit_pattern" "$guest_log")
	storage_completed_count=$(marker_count "$storage_completed_pattern" \
	    "$guest_log")
	uhci_shutdown_count=$(marker_count \
	    'uhci: checked shutdown workers joined' "$guest_log")
	ehci_shutdown_count=$(marker_count \
	    'ehci: checked shutdown workers joined' "$guest_log")
	dd_count=$(marker_count '1[+]0 records in' "$guest_log")
	if [ "$uhci_count" -ne "$expected_uhci" ] ||
	    [ "$uhci_root_count" -ne "$expected_uhci" ] ||
	    [ "$ehci_count" -ne "$expected_ehci" ] ||
	    [ "$ehci_root_count" -ne "$expected_ehci" ] ||
	    [ "$uhci_shutdown_count" -ne "$expected_uhci" ] ||
	    [ "$ehci_shutdown_count" -ne "$expected_ehci" ] ||
	    [ "$attach_count" -ne 12 ] || [ "$detach_count" -ne 12 ] ||
	    [ "$completion_count" -lt 12 ] || [ "$storage_count" -lt 1 ] ||
	    [ "$storage_submit_count" -lt 3 ] ||
	    [ "$storage_completed_count" -ne "$storage_submit_count" ] ||
	    [ "$dd_count" -lt 3 ]; then
		echo "$cell_kind marker cardinality failed" >&2
		return 1
	fi
	if [ "$(sha256sum "$source_boot" | awk '{print $1}')" != \
	    "$boot_digest" ] ||
	    [ "$(sha256sum "$source_auxiliary" | awk '{print $1}')" != \
	    "$auxiliary_digest" ] ||
	    [ "$(sha256sum "$auxiliary_copy" | awk '{print $1}')" != \
	    "$auxiliary_copy_digest" ]; then
		echo "$cell_kind mutated a pristine input image" >&2
		return 1
	fi

	{
		echo "end_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		echo "qemu_exit_status=$qemu_status"
		echo "controller_result=$(tr '\t\r\n' '   ' <"$controller_result")"
		echo "uhci_concurrent_markers=$uhci_count"
		echo "uhci_root_worker_markers=$uhci_root_count"
		echo "ehci_concurrent_markers=$ehci_count"
		echo "ehci_root_worker_markers=$ehci_root_count"
		echo "uhci_shutdown_worker_join_markers=$uhci_shutdown_count"
		echo "ehci_shutdown_worker_join_markers=$ehci_shutdown_count"
		echo "hid_attach_markers=$attach_count"
		echo "hid_completion_markers=$completion_count"
		echo "hid_detach_markers=$detach_count"
		echo "hid_disconnect_terminal_generations=$disconnect_terminal_count"
		echo "hid_action_boundary_records=$action_boundary_count"
		echo 'final_hid_rearm_pending_at_reboot=pass'
		echo "usb_storage_registration_markers=$storage_count"
		echo "usb_storage_submit_markers=$storage_submit_count"
		echo "usb_storage_completed_markers=$storage_completed_count"
		echo "dd_single_4096_record_markers=$dd_count"
		echo "input_integrity=pass"
		echo "bounded_reboot_and_worker_teardown=pass"
		if [ "$cell_kind" = paired ]; then
			echo "qemu_usb_hid_speed=12 Mb/s (UHCI companion handoff)"
			echo "qemu_usb_storage_speed=480 Mb/s (EHCI high-speed)"
			echo "ehci_high_speed_periodic_runtime_evidence=not claimed"
		fi
	} >>"$cell_metadata"
	printf '%s\tpass\t%s\n' "$cell_kind" \
	    "$cell_kind/metadata.txt;$cell_kind/guest.log;$cell_kind/qemu.log;$cell_kind/hid-action-boundaries.tsv" \
	    >>"$results"
	rm -f "$boot_copy" "$auxiliary_copy" "$vars_copy"
	echo "HW-T25 QEMU $cell_kind: PASS"
}

for diagnostic_cell in $diagnostic_cells; do
	run_cell "$diagnostic_cell"
done
exit 0

#!/bin/sh
# ws004-p016 non-interactive UHCI/EHCI lifecycle acceptance.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

if [ "$#" -ne 3 ]; then
	echo "usage: $0 AMD64-HDD-IMAGE AUXILIARY-IMAGE OUTPUT-DIRECTORY" >&2
	exit 2
fi

source_image=$1
auxiliary_image=$2
output=$3
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-90}
command_timeout=${COMMAND_TIMEOUT_SECONDS:-30}
cell_timeout=${CELL_TIMEOUT_SECONDS:-180}
key_delay=${KEY_DELAY_SECONDS:-0.01}

case $boot_timeout:$command_timeout:$cell_timeout in
	*[!0-9:]* | 0:* | *:0:* | *:0)
		echo "time bounds must be positive integers" >&2
		exit 2
		;;
esac
test -f "$source_image" || { echo "image not found: $source_image" >&2; exit 2; }
test -f "$auxiliary_image" || {
	echo "auxiliary image not found: $auxiliary_image" >&2
	exit 2
}
test -f "$ovmf_code" || { echo "OVMF code not found: $ovmf_code" >&2; exit 2; }
test -f "$ovmf_vars" || { echo "OVMF vars not found: $ovmf_vars" >&2; exit 2; }
command -v "$qemu" >/dev/null
command -v rg >/dev/null
command -v sha256sum >/dev/null
command -v timeout >/dev/null

mkdir -p "$output"
source_digest=$(sha256sum "$source_image" | awk '{print $1}')
auxiliary_digest=$(sha256sum "$auxiliary_image" | awk '{print $1}')

marker_count()
{
	count=$(rg -a -c -- "$1" "$2" 2>/dev/null || true)
	printf '%s\n' "${count:-0}"
}

run_cell()
{
	kind=$1
	cell=$output/$kind
	boot_copy=$cell/boot.img
	auxiliary_copy=$cell/auxiliary.img
	vars_copy=$cell/OVMF_VARS.fd
	guest_log=$cell/guest.log
	qemu_log=$cell/qemu.log
	result_file=$cell/controller-result.txt
	metadata=$cell/metadata.txt

	mkdir -p "$cell"
	cp --reflink=auto --sparse=always "$source_image" "$boot_copy"
	cp --reflink=auto --sparse=always "$auxiliary_image" "$auxiliary_copy"
	cp "$ovmf_vars" "$vars_copy"
	: >"$guest_log"
	: >"$qemu_log"
	: >"$result_file"

	case $kind in
	uhci)
		controller='piix3-usb-uhci,id=hcd'
		controller_pattern='uhci: PCI controller'
		retirement_pattern='uhci: checked frame retirement active'
		;;
	ehci)
		controller='usb-ehci,id=hcd'
		controller_pattern='ehci: PCI controller'
		retirement_pattern='ehci: checked async-advance retirement active'
		;;
	*) echo "unsupported HCD: $kind" >&2; return 2 ;;
	esac

	{
		echo "qemu=$($qemu --version | sed -n '1p')"
		echo "hcd=$kind"
		echo "source_image=$source_image"
		echo "source_sha256=$source_digest"
		echo "auxiliary_image=$auxiliary_image"
		echo "auxiliary_sha256=$auxiliary_digest"
		echo "topology=OVMF q35 IDE root plus $controller and read-only usb-storage"
		echo "runtime_model=enumeration, bounded bulk read, and checked reboot teardown"
		echo "not_injected=legacy hot-unplug/cancel, stalled UHCI FRNUM, and stale/duplicate/missing EHCI IAA"
	} >"$metadata"

	wait_for_pattern()
	{
		pattern=$1
		minimum=$2
		seconds=$3
		deadline=$(($(date +%s) + seconds))
		while :; do
			count=$(marker_count "$pattern" "$guest_log")
			[ "$count" -ge "$minimum" ] && return 0
			[ "$(date +%s)" -ge "$deadline" ] && return 1
			sleep 0.1
		done
	}

	send_text()
	{
		text_value=$1
		while [ -n "$text_value" ]; do
			character=$(printf '%s' "$text_value" | cut -c 1)
			text_value=$(printf '%s' "$text_value" | cut -c 2-)
			case $character in
				[a-z0-9]) key=$character ;;
				' ') key=spc ;;
				'/') key=slash ;;
				'=') key=equal ;;
				'-') key=minus ;;
				'_') key=shift-minus ;;
				'.') key=dot ;;
				*) echo "unsupported sendkey character: $character" >&2; return 1 ;;
			esac
			printf 'sendkey %s\n' "$key"
			sleep "$key_delay"
		done
		printf 'sendkey ret\n'
	}

	controller_driver()
	{
		trap '' PIPE
		set +e
		failure=
		login_pattern='(^|[[:blank:]])login:[[:blank:]]*$'
		password_pattern='Password:'
		prompt_pattern='root@[^[:space:]]*:[^$]*\$([[:blank:]]|$)'

		if ! wait_for_pattern "$controller_pattern" 1 "$boot_timeout" ||
		    ! wait_for_pattern 'usb-storage: sd[a-z]+ blocks=' 1 "$boot_timeout" ||
		    ! wait_for_pattern "$retirement_pattern" 1 "$boot_timeout" ||
		    ! wait_for_pattern "$login_pattern" 1 "$boot_timeout"; then
			failure='enumeration, checked-retirement, or login marker timeout'
			printf 'fail: %s\n' "$failure" >"$result_file"
			printf 'quit\n'
			return 1
		fi

		password_before=$(marker_count "$password_pattern" "$guest_log")
		prompt_before=$(marker_count "$prompt_pattern" "$guest_log")
		send_text root || return 1
		if ! wait_for_pattern "$password_pattern" $((password_before + 1)) \
		    "$command_timeout"; then
			printf 'fail: password prompt timeout\n' >"$result_file"
			printf 'quit\n'
			return 1
		fi
		send_text '' || return 1
		if ! wait_for_pattern "$prompt_pattern" $((prompt_before + 1)) \
		    "$command_timeout"; then
			printf 'fail: shell prompt timeout\n' >"$result_file"
			printf 'quit\n'
			return 1
		fi

		disk=$(rg -a -o 'usb-storage: sd[a-z]+ blocks=' "$guest_log" |
		    tail -n 1 | sed 's/^usb-storage: //; s/ blocks=$//')
		case $disk in sd[a-z]*) ;; *)
			printf 'fail: cannot identify USB disk\n' >"$result_file"
			printf 'quit\n'
			return 1
			;;
		esac

		# Legacy root-hub change dispatch is not yet a runtime hotplug worker, so
		# do not pretend device_del supplies cancellation evidence.  Exercise real
		# control and bulk traffic here; the phase-owned model supplies the bounded
		# cancel/fault interleavings which QEMU cannot inject through this path.
		prompt_before=$(marker_count "$prompt_pattern" "$guest_log")
		command="/bin/dd if=/dev/$disk of=/tmp/legacy-hcd-read.bin bs=512 count=8"
		send_text "$command" || return 1
		if ! wait_for_pattern "$command" 1 "$command_timeout"; then
			printf 'fail: dd command was not echoed\n' >"$result_file"
			printf 'quit\n'
			return 1
		fi
		if ! wait_for_pattern "$prompt_pattern" $((prompt_before + 1)) \
		    "$command_timeout"; then
			printf 'fail: bounded bulk read did not complete\n' >"$result_file"
			printf 'quit\n'
			return 1
		fi

		reboot_before=$(marker_count 'init: executing system action reboot' \
		    "$guest_log")
		send_text reboot || return 1
		if ! wait_for_pattern 'init: executing system action reboot' \
		    $((reboot_before + 1)) "$command_timeout"; then
			printf 'fail: reboot action timeout\n' >"$result_file"
			printf 'quit\n'
			return 1
		fi
		printf 'pass\n' >"$result_file"
		# EOF only closes HMP input; it does not stop QEMU.  The pipeline now
		# waits for /dev/system to finish USB/HCD shutdown and reset the guest.
		# If reset does not occur, the outer hard timeout fails the cell.
		return 0
	}

	if controller_driver | timeout --foreground --kill-after=5 \
	    "${cell_timeout}s" "$qemu" \
	    -machine q35 -m 512 -smp 2 \
	    -drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
	    -drive if=pflash,format=raw,file="$vars_copy" \
	    -device piix3-ide,id=legacyide \
	    -drive if=none,id=bootdisk,file="$boot_copy",format=raw \
	    -device ide-hd,bus=legacyide.0,drive=bootdisk,bootindex=1 \
	    -device "$controller" \
	    -drive if=none,id=usbdisk,file="$auxiliary_copy",format=raw,readonly=on \
	    -device usb-storage,bus=hcd.0,drive=usbdisk,id=stick \
	    -display none -serial none -debugcon "file:$guest_log" \
	    -monitor stdio -no-reboot >"$qemu_log" 2>&1; then
		qemu_status=0
	else
		qemu_status=$?
	fi

	if [ "$qemu_status" -ne 0 ]; then
		echo "$kind QEMU exited with status $qemu_status" >&2
		return 1
	fi
	if [ ! -s "$result_file" ] || [ "$(cat "$result_file")" != pass ]; then
		echo "$kind controller failed: $(cat "$result_file" 2>/dev/null || echo no-result)" >&2
		return 1
	fi
	monitor_failure='unknown command|invalid parameter|device .* not found|property .* not found|Error:'
	if rg -a -i -q -- "$monitor_failure" "$qemu_log"; then
		echo "$kind QEMU monitor rejected a lifecycle command" >&2
		rg -a -i -m 1 -- "$monitor_failure" "$qemu_log" >&2 || true
		return 1
	fi
	guest_failure='fatal:|kernel panic|panic:|amd64 fault v=|VFS initialization failed|request retirement failed|frame retirement failed|DMA retained|retaining QH/TD/bounce DMA|refusing to release DMA|host controller stop failed|driver shutdown failed'
	if rg -a -i -q -- "$guest_failure" "$guest_log"; then
		echo "$kind guest reported unsafe retirement or shutdown" >&2
		rg -a -i -m 1 -- "$guest_failure" "$guest_log" >&2 || true
		return 1
	fi
	for required in "$controller_pattern" 'usb-storage: sd[a-z]+ blocks=' \
	    "$retirement_pattern" '8\+0 records in' \
	    'init: executing system action reboot'; do
		if ! rg -a -q -- "$required" "$guest_log"; then
			echo "$kind missing runtime marker: $required" >&2
			return 1
		fi
	done
	if [ "$(sha256sum "$source_image" | awk '{print $1}')" != \
	    "$source_digest" ] ||
	    [ "$(sha256sum "$auxiliary_image" | awk '{print $1}')" != \
	    "$auxiliary_digest" ]; then
		echo "$kind mutated a pristine input image" >&2
		return 1
	fi

	{
		echo "qemu_status=$qemu_status"
		echo "result=pass"
	} >>"$metadata"
	rm -f "$boot_copy" "$auxiliary_copy" "$vars_copy"
	echo "legacy HCD QEMU $kind lifecycle: PASS"
}

run_cell uhci
run_cell ehci
echo "legacy HCD QEMU lifecycle: PASS"

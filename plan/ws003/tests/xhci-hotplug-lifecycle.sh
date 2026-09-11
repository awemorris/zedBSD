#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
	echo "usage: $0 BOOT-IMAGE AUXILIARY-IMAGE OUTPUT-DIRECTORY" >&2
	exit 2
fi

boot_image=$1
auxiliary_image=$2
output=$3
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-90}
hotplug_timeout=${HOTPLUG_TIMEOUT_SECONDS:-20}
settle_seconds=${SETTLE_SECONDS:-2}
smp_cpus=${SMP_CPUS:-2}

case $boot_timeout:$hotplug_timeout:$settle_seconds:$smp_cpus in
	*[!0-9:]* | 0:* | *:0:* | *:0:* | *:0)
		echo "time bounds and SMP_CPUS must be positive integers" >&2
		exit 2
		;;
esac

test -f "$boot_image" || {
	echo "boot image not found: $boot_image" >&2
	exit 2
}
test -f "$auxiliary_image" || {
	echo "auxiliary image not found: $auxiliary_image" >&2
	exit 2
}
command -v "$qemu" >/dev/null
command -v rg >/dev/null
command -v sha256sum >/dev/null

mkdir -p "$output"
boot_copy=$output/boot.img
auxiliary_initial_copy=$output/auxiliary-initial.img
auxiliary_readd_copy=$output/auxiliary-readd.img
guest_log=$output/guest.log
monitor_log=$output/monitor.log
controller_result=$output/controller-result.tsv
metadata=$output/metadata.txt
boot_digest=$(sha256sum "$boot_image" | awk '{print $1}')
auxiliary_digest=$(sha256sum "$auxiliary_image" | awk '{print $1}')
qemu_pid=

cleanup()
{
	if [ -n "$qemu_pid" ]; then
		kill "$qemu_pid" 2>/dev/null || true
		wait "$qemu_pid" 2>/dev/null || true
	fi
}
trap cleanup 0
trap 'exit 130' HUP INT TERM

cp "$boot_image" "$boot_copy"
cp "$auxiliary_image" "$auxiliary_initial_copy"
cp "$auxiliary_image" "$auxiliary_readd_copy"
: >"$guest_log"
: >"$monitor_log"
: >"$controller_result"

guest_failure_pattern='fatal:|kernel panic|panic:|amd64 fault v=|boot-storage wait expired|xhci: command [0-9][0-9]* timed out|xhci: command [0-9][0-9]* failed|xhci: ignored command completion|xhci: unmatched command completion|xhci: .*retain|usb[0-9][0-9]*: .*DMA retained|usb[0-9][0-9]*: .*teardown failed|usb[0-9][0-9]*: port [0-9][0-9]* enumeration failed|usb-storage: BOT .*error=[1-9]|usb-storage: sd[a-z][a-z]* op=.*error=[1-9]|loop[0-9][0-9]*: .*error=[1-9]'
configured_pattern='usb[0-9][0-9]*: device [0-9][0-9]* port [0-9][0-9]* .* configured'
disconnected_pattern='usb[0-9][0-9]*: device [0-9][0-9]* port [0-9][0-9]* disconnected'
storage_pattern='usb-storage: sd[a-z][a-z]* blocks=[0-9][0-9]* block-size=[0-9][0-9]*'

{
	echo "boot_image=$boot_image"
	echo "boot_sha256=$boot_digest"
	echo "auxiliary_image=$auxiliary_image"
	echo "auxiliary_sha256=$auxiliary_digest"
	echo "qemu=$($qemu --version | sed -n '1p')"
	echo "boot_timeout_seconds=$boot_timeout"
	echo "hotplug_timeout_seconds=$hotplug_timeout"
	echo "settle_seconds=$settle_seconds"
	echo "smp_cpus=$smp_cpus"
	echo "topology=q35 legacy piix3-ide root plus read-only qemu-xhci usb-storage"
} >"$metadata"

monitor_driver()
{
	line_count()
	{
		if [ ! -f "$guest_log" ]; then
			echo 0
			return
		fi
		line_count_value=$(rg -a -c -- "$1" "$guest_log" 2>/dev/null || true)
		if [ -z "$line_count_value" ]; then
			line_count_value=0
		fi
		echo "$line_count_value"
	}

	first_guest_failure()
	{
		rg -a -m 1 -- "$guest_failure_pattern" "$guest_log" 2>/dev/null |
		    tr '\t\r\n' '   ' || true
	}

	wait_for_count()
	{
		wait_pattern=$1
		wait_minimum=$2
		wait_seconds=$3
		wait_label=$4
		wait_deadline=$(($(date +%s) + wait_seconds))
		while :; do
			if rg -a -q -- "$guest_failure_pattern" "$guest_log" 2>/dev/null; then
				wait_failure=$(first_guest_failure)
				printf 'fail\t%s: %s\n' "$wait_label" "$wait_failure" \
				    >"$controller_result"
				return 1
			fi
			wait_count=$(line_count "$wait_pattern")
			if [ "$wait_count" -ge "$wait_minimum" ]; then
				return 0
			fi
			if [ "$(date +%s)" -ge "$wait_deadline" ]; then
				printf 'fail\t%s timed out (%s/%s markers)\n' \
				    "$wait_label" "$wait_count" "$wait_minimum" \
				    >"$controller_result"
				return 1
			fi
			sleep 1
		done
	}

	if ! wait_for_count 'login:' 1 "$boot_timeout" 'initial IDE-root boot'; then
		printf 'quit\n'
		return
	fi
	if ! wait_for_count "$configured_pattern" 1 "$hotplug_timeout" \
	    'initial auxiliary USB configuration'; then
		printf 'quit\n'
		return
	fi
	if ! wait_for_count "$storage_pattern" 1 "$hotplug_timeout" \
	    'initial auxiliary USB storage registration'; then
		printf 'quit\n'
		return
	fi

	configured_before=$(line_count "$configured_pattern")
	disconnected_before=$(line_count "$disconnected_pattern")
	storage_before=$(line_count "$storage_pattern")
	printf 'device_del stick\n'
	if ! wait_for_count "$disconnected_pattern" \
	    $((disconnected_before + 1)) "$hotplug_timeout" \
	    'auxiliary USB removal'; then
		printf 'quit\n'
		return
	fi

	printf '%s\n' \
	    'device_add usb-storage,bus=xhci.0,drive=auxdisk2,id=stick2,serial=br-t29-readd'
	if ! wait_for_count "$configured_pattern" $((configured_before + 1)) \
	    "$hotplug_timeout" 'auxiliary USB reconfiguration'; then
		printf 'quit\n'
		return
	fi
	if ! wait_for_count "$storage_pattern" $((storage_before + 1)) \
	    "$hotplug_timeout" 'auxiliary USB storage re-registration'; then
		printf 'quit\n'
		return
	fi

	sleep "$settle_seconds"
	if rg -a -q -- "$guest_failure_pattern" "$guest_log" 2>/dev/null; then
		wait_failure=$(first_guest_failure)
		printf 'fail\tpost-reconnect settle: %s\n' "$wait_failure" \
		    >"$controller_result"
	else
		printf 'pass\tconfigured=%s disconnected=%s storage=%s\n' \
		    "$(line_count "$configured_pattern")" \
		    "$(line_count "$disconnected_pattern")" \
		    "$(line_count "$storage_pattern")" \
		    >"$controller_result"
	fi
	printf 'quit\n'
}

monitor_driver | "$qemu" \
	-machine q35 \
	-m 512 \
	-smp "$smp_cpus" \
	-device piix3-ide,id=legacyide \
	-drive if=none,id=bootdisk,file="$boot_copy",format=raw \
	-device ide-hd,bus=legacyide.0,drive=bootdisk \
	-device qemu-xhci,id=xhci \
	-drive if=none,id=auxdisk1,file="$auxiliary_initial_copy",format=raw,readonly=on \
	-drive if=none,id=auxdisk2,file="$auxiliary_readd_copy",format=raw,readonly=on \
	-device usb-storage,bus=xhci.0,drive=auxdisk1,id=stick,serial=br-t29-initial \
	-display none \
	-serial none \
	-debugcon file:"$guest_log" \
	-monitor stdio \
	-no-reboot >"$monitor_log" 2>&1 &
qemu_pid=$!

if wait "$qemu_pid"; then
	qemu_status=0
else
	qemu_status=$?
fi
qemu_pid=

monitor_failure_pattern='unknown command|invalid parameter|duplicate id|device .* not found|property .* not found|error:|failed to'
if [ "$qemu_status" -ne 0 ]; then
	echo "BR-T29 FAIL: QEMU exited with status $qemu_status" >&2
	exit 1
fi
if [ ! -s "$controller_result" ]; then
	echo "BR-T29 FAIL: monitor controller produced no result" >&2
	exit 1
fi
if rg -a -i -q -- "$monitor_failure_pattern" "$monitor_log"; then
	echo "BR-T29 FAIL: QEMU monitor rejected a hotplug command" >&2
	rg -a -i -m 1 -- "$monitor_failure_pattern" "$monitor_log" >&2 || true
	exit 1
fi
if ! rg -q '^pass\t' "$controller_result"; then
	echo "BR-T29 FAIL: $(tr '\t\r\n' '   ' <"$controller_result")" >&2
	exit 1
fi
if rg -a -q -- "$guest_failure_pattern" "$guest_log"; then
	echo "BR-T29 FAIL: guest reported an xHCI lifecycle failure" >&2
	rg -a -m 1 -- "$guest_failure_pattern" "$guest_log" >&2 || true
	exit 1
fi
if [ "$(sha256sum "$boot_image" | awk '{print $1}')" != "$boot_digest" ] ||
    [ "$(sha256sum "$auxiliary_image" | awk '{print $1}')" != "$auxiliary_digest" ]; then
	echo "BR-T29 FAIL: a pristine input image changed" >&2
	exit 1
fi

rm -f "$boot_copy" "$auxiliary_initial_copy" "$auxiliary_readd_copy"
echo "BR-T29 PASS: IDE root remained live across xHCI USB remove/re-add"

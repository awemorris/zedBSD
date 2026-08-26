#!/bin/sh
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
	echo "usage: $0 IMAGE OUTPUT-DIRECTORY [COUNT]" >&2
	exit 2
fi

image=$1
output=$2
count=${3:-500}
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-60}
settle_seconds=${SETTLE_SECONDS:-2}
fingerprint=${USB_CONTRACT_FINGERPRINT:-q009-release-acquire-v1}
smp_cpus=${SMP_CPUS:-4}
storage_mode=${STORAGE_MODE:-usb}

case $count:$boot_timeout:$settle_seconds:$smp_cpus in
	*[!0-9:]* | 0:* | *:0:* | *:0 )
		echo "count, time bounds, and SMP_CPUS must be positive integers" >&2
		exit 2
		;;
esac
case $storage_mode in
usb | ide) ;;
*)
	echo "STORAGE_MODE must be usb or ide" >&2
	exit 2
	;;
esac

test -f "$image" || {
	echo "image not found: $image" >&2
	exit 2
}
command -v "$qemu" >/dev/null
command -v sha256sum >/dev/null
command -v rg >/dev/null

mkdir -p "$output"
base_digest=$(sha256sum "$image" | awk '{print $1}')
results=$output/results.tsv
metadata=$output/metadata.txt
failure_pattern='loop1: write .*error=[1-9]|usb-storage: BOT .*error=[1-9]|usb-storage: BOT .*actual=0.*expected=[1-9]|usb-storage: sda op=2a .*error=[1-9]|xhci: transfer completion=|syslogd: .*Input/output error'
kernel_failure_pattern='amd64 fault v=|fatal: .*unhandled amd64 fault|kernel panic|panic:'

{
	echo "base_image=$image"
	echo "base_sha256=$base_digest"
	echo "qemu=$($qemu --version | sed -n '1p')"
	echo "count=$count"
	echo "boot_timeout_seconds=$boot_timeout"
	echo "settle_seconds=$settle_seconds"
	echo "fingerprint=$fingerprint"
	echo "smp_cpus=$smp_cpus"
	echo "storage_mode=$storage_mode"
	if [ "$storage_mode" = usb ]; then
		echo "topology=-machine q35 -m 512 -smp $smp_cpus -device qemu-xhci,id=xhci -device usb-storage,bus=xhci.0 -netdev user,id=net0 -device ne2k_isa,netdev=net0,iobase=0x300,irq=10"
	else
		echo "topology=-machine pc -m 512 -smp $smp_cpus -drive if=ide -netdev user,id=net0 -device ne2k_isa,netdev=net0,iobase=0x300,irq=10"
	fi
} >"$metadata"
printf 'run\tclass\telapsed_seconds\tfirst_failure\n' >"$results"

passes=0
usb_failures=0
harness_failures=0
kernel_failures=0
run=1
while [ "$run" -le "$count" ]; do
	id=$(printf '%04d' "$run")
	run_image=$output/run-$id.img
	log=$output/run-$id.log
	start=$(date +%s)
	deadline=$((start + boot_timeout))
	login_time=0
	class=
	first_failure=

	cp --reflink=auto --sparse=always "$image" "$run_image"
	if [ "$storage_mode" = usb ]; then
		"$qemu" \
			-machine q35 \
			-m 512 \
			-smp "$smp_cpus" \
			-device qemu-xhci,id=xhci \
			-drive if=none,id=usbboot,file="$run_image",format=raw \
			-device usb-storage,bus=xhci.0,drive=usbboot,id=bootstick,bootindex=1 \
			-netdev user,id=net0 \
			-device ne2k_isa,netdev=net0,iobase=0x300,irq=10 \
			-display none \
			-monitor none \
			-serial none \
			-debugcon file:"$log" \
			-no-reboot >/dev/null 2>&1 &
	else
		"$qemu" \
			-machine pc \
			-m 512 \
			-smp "$smp_cpus" \
			-drive file="$run_image",format=raw,if=ide \
			-netdev user,id=net0 \
			-device ne2k_isa,netdev=net0,iobase=0x300,irq=10 \
			-display none \
			-monitor none \
			-serial none \
			-debugcon file:"$log" \
			-no-reboot >/dev/null 2>&1 &
	fi
	pid=$!

	while :; do
		now=$(date +%s)
		if [ -f "$log" ] && rg -a -q "$kernel_failure_pattern" "$log"; then
			class=kernel-failure
			first_failure=$(rg -a -m 1 "$kernel_failure_pattern" "$log" |
			    tr '\t\r\n' '   ')
			break
		fi
		if [ -f "$log" ] && rg -a -q "$failure_pattern" "$log"; then
			class=usb-storage-failure
			first_failure=$(rg -a -m 1 "$failure_pattern" "$log" |
			    tr '\t\r\n' '   ')
			break
		fi
		if [ "$login_time" -eq 0 ] && [ -f "$log" ] &&
		    rg -a -q 'login:' "$log"; then
			login_time=$now
		fi
		if [ "$login_time" -ne 0 ] &&
		    [ "$now" -ge $((login_time + settle_seconds)) ]; then
			class=pass
			break
		fi
		if [ "$now" -ge "$deadline" ]; then
			class=boot-timeout
			break
		fi
		if ! kill -0 "$pid" 2>/dev/null; then
			class=early-qemu-exit
			break
		fi
		sleep 0.1
	done

	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	end=$(date +%s)

	if [ ! -f "$log" ] || ! rg -a -q "$fingerprint" "$log"; then
		class=missing-fingerprint
	fi
	if [ "$class" = pass ] && rg -a -q "$failure_pattern" "$log"; then
		class=usb-storage-failure
		first_failure=$(rg -a -m 1 "$failure_pattern" "$log" |
		    tr '\t\r\n' '   ')
	fi
	if [ "$class" = pass ] && rg -a -q "$kernel_failure_pattern" "$log"; then
		class=kernel-failure
		first_failure=$(rg -a -m 1 "$kernel_failure_pattern" "$log" |
		    tr '\t\r\n' '   ')
	fi
	if [ "$class" = pass ]; then
		passes=$((passes + 1))
		rm -f "$run_image"
	elif [ "$class" = usb-storage-failure ]; then
		usb_failures=$((usb_failures + 1))
	elif [ "$class" = kernel-failure ]; then
		kernel_failures=$((kernel_failures + 1))
	else
		harness_failures=$((harness_failures + 1))
	fi
	printf '%s\t%s\t%s\t%s\n' "$run" "$class" "$((end - start))" \
	    "$first_failure" >>"$results"
	printf 'HW-T12 run %s/%s: %s (pass=%s usb-failure=%s kernel-failure=%s harness=%s)\n' \
	    "$run" "$count" "$class" "$passes" "$usb_failures" \
	    "$kernel_failures" "$harness_failures"
	run=$((run + 1))
done

final_digest=$(sha256sum "$image" | awk '{print $1}')
if [ "$final_digest" != "$base_digest" ]; then
	echo "pristine base image changed during the run" >&2
	exit 1
fi
if [ "$passes" -ne "$count" ]; then
	echo "HW-T12 FAIL: pass=$passes usb-failure=$usb_failures kernel-failure=$kernel_failures harness-failure=$harness_failures" >&2
	exit 1
fi
echo "HW-T12 PASS: $passes/$count pristine-copy boots"

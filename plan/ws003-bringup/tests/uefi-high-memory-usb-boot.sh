#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 IMAGE OUTPUT-DIRECTORY" >&2
	exit 2
fi

image=$1
output=$2
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
memory_list=${MEMORY_MIB_LIST:-4096 8192 16384}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-120}
settle_seconds=${SETTLE_SECONDS:-1}
smp_cpus=${SMP_CPUS:-4}

case $boot_timeout:$settle_seconds:$smp_cpus in
	*[!0-9:]* | 0:* | *:0:* | *:0)
	echo "time bounds and SMP_CPUS must be positive integers" >&2
	exit 2
	;;
esac
for memory in $memory_list; do
	case $memory in
	'' | *[!0-9]* | 0)
		echo "MEMORY_MIB_LIST must contain positive MiB integers" >&2
		exit 2
		;;
	esac
done

test -f "$image" || {
	echo "image not found: $image" >&2
	exit 2
}
test -f "$ovmf_code" || {
	echo "OVMF code image not found: $ovmf_code" >&2
	exit 2
}
test -f "$ovmf_vars" || {
	echo "OVMF variables image not found: $ovmf_vars" >&2
	exit 2
}
command -v "$qemu" >/dev/null
command -v rg >/dev/null
command -v sha256sum >/dev/null

mkdir -p "$output"
base_digest=$(sha256sum "$image" | awk '{print $1}')
results=$output/results.tsv
failure_pattern='fatal:|kernel panic|panic:|amd64 fault v=|loop1: write .*error=[1-9]|usb-storage: BOT .*error=[1-9]|usb-storage: BOT .*actual=0.*expected=[1-9]|usb-storage: sda op=2a .*error=[1-9]|usb-storage: sda flush .*error=[1-9]|xhci: transfer completion=|xhci: control |xhci: command [0-9][0-9]* failed|xhci: .*retain|syslogd: .*Input/output error'

{
	echo "base_image=$image"
	echo "base_sha256=$base_digest"
	echo "qemu=$($qemu --version | sed -n '1p')"
	echo "memory_mib_list=$memory_list"
	echo "boot_timeout_seconds=$boot_timeout"
	echo "settle_seconds=$settle_seconds"
	echo "smp_cpus=$smp_cpus"
	echo "topology=OVMF q35 xHCI USB storage NE2000"
} >"$output/metadata.txt"
printf 'memory_mib\tclass\trsdp\telapsed_seconds\tfirst_failure\n' >"$results"

passes=0
for memory in $memory_list; do
	run_image=$output/boot-${memory}m.img
	vars=$output/vars-${memory}m.fd
	log=$output/boot-${memory}m.log
	qemu_log=$output/qemu-${memory}m.log
	start=$(date +%s)
	deadline=$((start + boot_timeout))
	login_time=0
	class=
	first_failure=
	rsdp=

	cp --reflink=auto --sparse=always "$image" "$run_image"
	cp "$ovmf_vars" "$vars"
	# Reusing an evidence directory must never let a previous guest log satisfy
	# this run's markers before QEMU has opened its debugcon output.
	: >"$log"
	: >"$qemu_log"
	"$qemu" \
		-machine q35 \
		-m "$memory" \
		-smp "$smp_cpus" \
		-drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
		-drive if=pflash,format=raw,file="$vars" \
		-device qemu-xhci,id=xhci \
		-drive if=none,id=usbboot,file="$run_image",format=raw \
		-device usb-storage,bus=xhci.0,drive=usbboot,id=bootstick,bootindex=1 \
		-netdev user,id=net0 \
		-device ne2k_isa,netdev=net0,iobase=0x300,irq=10 \
		-display none \
		-monitor none \
		-serial none \
		-debugcon file:"$log" \
		-no-reboot >"$qemu_log" 2>&1 &
	pid=$!

	while :; do
		now=$(date +%s)
		if [ -f "$log" ] && rg -a -q "$failure_pattern" "$log"; then
			class=boot-failure
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

	if [ -f "$log" ]; then
		rsdp=$(sed -n 's/^A64 RSDP 0x\([0-9A-Fa-f][0-9A-Fa-f]*\)$/\1/p' \
		    "$log" | tail -n 1 | tr 'A-F' 'a-f')
	fi
	if [ "$class" = pass ]; then
		for marker in \
		    'A64 UEFI BOOT SERVICES EXITED' \
		    'A64 UEFI EXIT' \
		    'A64 ENTRY PASS' \
		    'A64 PAGING PASS' \
		    'A64 ACPI RSDP PASS' \
		    'A64 IRQ READY' \
		    "boot: CPUs ready: $smp_cpus" \
		    'login:'; do
			if ! rg -a -F -q "$marker" "$log"; then
				class=missing-marker
				first_failure="missing $marker"
				break
			fi
		done
	fi
	if [ "$class" = pass ]; then
		case $rsdp in
		????????????????) ;;
		*)
			class=invalid-rsdp
			first_failure="invalid RSDP diagnostic"
			;;
		esac
	fi
	if [ "$class" = pass ]; then
		lowest=$(printf '%s\n%s\n' 0000000040000000 "$rsdp" |
		    LC_ALL=C sort | sed -n '1p')
		if [ "$rsdp" = 0000000040000000 ] ||
		    [ "$lowest" != 0000000040000000 ]; then
			class=low-rsdp
			first_failure="RSDP not above 1 GiB"
		fi
	fi
	if [ "$class" = pass ] && rg -a -q "$failure_pattern" "$log"; then
		class=boot-failure
		first_failure=$(rg -a -m 1 "$failure_pattern" "$log" |
		    tr '\t\r\n' '   ')
	fi
	if [ "$class" = pass ]; then
		passes=$((passes + 1))
		rm -f "$run_image" "$vars"
	fi
	printf '%s\t%s\t%s\t%s\t%s\n' "$memory" "$class" "$rsdp" \
	    "$((end - start))" "$first_failure" >>"$results"
	printf 'BR-T24 %s MiB: %s (RSDP=0x%s)\n' "$memory" "$class" \
	    "${rsdp:-unknown}"
done

final_digest=$(sha256sum "$image" | awk '{print $1}')
if [ "$final_digest" != "$base_digest" ]; then
	echo "pristine base image changed during the run" >&2
	exit 1
fi
expected=$(printf '%s\n' $memory_list | awk 'NF { count++ } END { print count }')
if [ "$passes" -ne "$expected" ]; then
	echo "BR-T24 FAIL: pass=$passes expected=$expected" >&2
	exit 1
fi
echo "BR-T24 PASS: $passes/$expected high-memory OVMF USB boots"

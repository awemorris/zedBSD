#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 IMAGE OUTPUT-DIRECTORY" >&2
	exit 2
fi

image=$1
output=$2
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-90}
settle_seconds=${SETTLE_SECONDS:-2}
smp_cpus=${SMP_CPUS:-4}
usb_write_cache=${USB_STORAGE_WRITE_CACHE:-auto}
usb_commandlog=${USB_STORAGE_COMMANDLOG:-off}

case $boot_timeout:$settle_seconds:$smp_cpus in
	*[!0-9:]* | 0:* | *:0:* | *:0)
		echo "time bounds and SMP_CPUS must be positive integers" >&2
		exit 2
		;;
esac
case $usb_write_cache in
auto | on | off) ;;
*)
	echo "USB_STORAGE_WRITE_CACHE must be auto, on, or off" >&2
	exit 2
	;;
esac
case $usb_commandlog in
on | off) ;;
*)
	echo "USB_STORAGE_COMMANDLOG must be on or off" >&2
	exit 2
	;;
esac

test -f "$image" || {
	echo "image not found: $image" >&2
	exit 2
}
command -v "$qemu" >/dev/null
command -v rg >/dev/null
command -v sha256sum >/dev/null

mkdir -p "$output"
run_image=$output/usb-boot.img
guest_log=$output/guest.log
qemu_log=$output/qemu.log
metadata=$output/metadata.txt
base_digest=$(sha256sum "$image" | awk '{print $1}')
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

cp --reflink=auto --sparse=always "$image" "$run_image"
: >"$guest_log"
: >"$qemu_log"

failure_pattern='fatal:|FATAL:|kernel panic|panic:|amd64 fault v=|VFS initialization failed|boot-storage wait expired|xhci: control |xhci: transfer completion=|xhci: ignored command completion|xhci: unmatched command completion|xhci: command [0-9][0-9]* (failed|timed out)|xhci: .*recovery failed|xhci: .*cancel failed|xhci: .*quiesce failed|xhci: .*attach failed|xhci: .*retain|xhci: refusing |usb[0-9][0-9]*: port [0-9][0-9]* enumeration failed|usb-storage: BOT .*error=[1-9]|usb-storage: BOT .*actual=0.*expected=[1-9]|usb-storage: sd[a-z][a-z]* op=2a .*error=[1-9]|usb-storage: sd[a-z][a-z]* flush .*error=[1-9]|loop[0-9][0-9]*: write .*error=[1-9]'

{
	echo "base_image=$image"
	echo "base_sha256=$base_digest"
	echo "run_image=$run_image"
	echo "qemu=$($qemu --version | sed -n '1p')"
	echo "boot_timeout_seconds=$boot_timeout"
	echo "settle_seconds=$settle_seconds"
	echo "smp_cpus=$smp_cpus"
	echo "usb_storage_write_cache=$usb_write_cache"
	echo "usb_storage_commandlog=$usb_commandlog"
	echo "topology=SeaBIOS q35 qemu-xhci USB-storage-only system disk"
} >"$metadata"

"$qemu" \
	-machine q35 \
	-m 512 \
	-smp "$smp_cpus" \
	-device qemu-xhci,id=xhci \
	-drive if=none,id=usbboot,file="$run_image",format=raw \
	-device usb-storage,bus=xhci.0,drive=usbboot,id=bootstick,bootindex=1,write-cache="$usb_write_cache",commandlog="$usb_commandlog" \
	-display none \
	-monitor none \
	-serial none \
	-debugcon file:"$guest_log" \
	-no-reboot >"$qemu_log" 2>&1 &
qemu_pid=$!

start=$(date +%s)
deadline=$((start + boot_timeout))
login_time=0
class=
first_failure=

while :; do
	now=$(date +%s)
	if rg -a -q -- "$failure_pattern" "$guest_log" 2>/dev/null; then
		class=boot-failure
		first_failure=$(rg -a -m 1 -- "$failure_pattern" "$guest_log" |
		    tr '\t\r\n' '   ')
		break
	fi
	if [ "$login_time" -eq 0 ] && rg -a -q 'login:' "$guest_log" \
	    2>/dev/null; then
		login_time=$now
	fi
	if [ "$login_time" -ne 0 ] &&
	    [ "$now" -ge $((login_time + settle_seconds)) ]; then
		class=pass
		break
	fi
	if [ "$now" -ge "$deadline" ]; then
		class=boot-timeout
		first_failure="login was not reached within ${boot_timeout}s"
		break
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		class=early-qemu-exit
		first_failure="QEMU exited before login"
		break
	fi
	sleep 0.1
done

kill "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true
qemu_pid=

if [ "$class" = pass ]; then
	for marker in \
	    'xhci: PCI controller' \
	    'usb-storage: sda blocks=' \
	    '-> /dev/sda1 (private FAT)' \
	    'login:'; do
		if ! rg -a -F -q -- "$marker" "$guest_log"; then
			class=missing-marker
			first_failure="missing $marker"
			break
		fi
	done
fi
if [ "$class" = pass ] &&
    ! rg -a -q 'usb[0-9][0-9]*: device [0-9][0-9]* port [0-9][0-9]* .* configured' \
	"$guest_log"; then
	class=missing-marker
	first_failure="missing configured USB device"
fi
if [ "$class" = pass ] &&
    rg -a -q -- "$failure_pattern" "$guest_log"; then
	class=boot-failure
	first_failure=$(rg -a -m 1 -- "$failure_pattern" "$guest_log" |
	    tr '\t\r\n' '   ')
fi
if [ "$class" = pass ] && [ "$usb_write_cache" = off ]; then
	if ! rg -a -F -q \
	    'cache=disabled dpofua=no flush=write-through' "$guest_log"; then
		class=missing-marker
		first_failure="missing write-through flush-policy marker"
	elif [ "$usb_commandlog" = on ] &&
	    rg -a -q 'SYNCHRONIZE_CACHE' "$qemu_log"; then
		class=unexpected-sync-cache
		first_failure="write-through policy issued SYNCHRONIZE CACHE"
	fi
fi
if [ "$(sha256sum "$image" | awk '{print $1}')" != "$base_digest" ]; then
	class=input-image-mutated
	first_failure="pristine input image changed"
fi

{
	echo "class=$class"
	echo "elapsed_seconds=$(($(date +%s) - start))"
	echo "first_failure=$first_failure"
} >>"$metadata"

if [ "$class" != pass ]; then
	echo "legacy xHCI USB boot FAIL: $class: $first_failure" >&2
	echo "guest log: $guest_log" >&2
	echo "QEMU log: $qemu_log" >&2
	exit 1
fi

rm -f "$run_image"
echo "legacy xHCI USB boot PASS: USB-only BIOS root reached login"

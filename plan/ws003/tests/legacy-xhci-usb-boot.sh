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
usb_media_sectors=${USB_MEDIA_SECTORS:-}

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
case $usb_media_sectors in
'') ;;
*[!0-9]* | 0*)
	echo "USB_MEDIA_SECTORS must be a positive sector count" >&2
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
if [ -n "$usb_media_sectors" ]; then
	command -v truncate >/dev/null
	if [ "${#usb_media_sectors}" -gt 16 ]; then
		echo "USB_MEDIA_SECTORS exceeds the supported host size" >&2
		exit 2
	fi
	usb_media_bytes=$((usb_media_sectors * 512))
	source_bytes=$(wc -c <"$image")
	case $source_bytes in
	'' | *[!0-9]*)
		echo "could not determine image size: $image" >&2
		exit 2
		;;
	esac
	if [ $((source_bytes % 512)) -ne 0 ]; then
		echo "USB media image size must be a multiple of 512 bytes" >&2
		exit 2
	fi
	source_sectors=$((source_bytes / 512))
	if [ "$usb_media_sectors" -lt "$source_sectors" ]; then
		echo "USB_MEDIA_SECTORS must not truncate the source image" >&2
		exit 2
	fi
fi

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
if [ -n "$usb_media_sectors" ]; then
	truncate -s "$usb_media_bytes" -- "$run_image"
fi
: >"$guest_log"
: >"$qemu_log"

failure_pattern='fatal:|FATAL:|kernel panic|panic:|amd64 fault v=|A64 APIC PREFLIGHT FAIL|A64 IOAPIC .*FAIL|A64 TIMER CAL (TIMEOUT|INVALID)|VFS initialization failed|boot-storage wait expired|xhci: control |xhci: transfer completion=|xhci: ignored command completion|xhci: unmatched command completion|xhci: command [0-9][0-9]* (failed|timed out)|xhci: .*recovery failed|xhci: .*cancel failed|xhci: .*quiesce failed|xhci: .*attach failed|xhci: .*retain|xhci: refusing |usb[0-9][0-9]*: port [0-9][0-9]* enumeration failed|usb-storage: BOT .*error=[1-9]|usb-storage: BOT .*actual=0.*expected=[1-9]|usb-storage: sd[a-z][a-z]* op=2a .*error=[1-9]|usb-storage: sd[a-z][a-z]* flush .*error=[1-9]|loop[0-9][0-9]*: write .*error=[1-9]'

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
	echo "usb_media_sectors=${usb_media_sectors:-source-image}"
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
	    'A64 IDT READY' \
	    'A64 LAPIC READY' \
	    'A64 IOAPIC ROUTING READY' \
	    'A64 TIMER READY' \
	    'A64 XMM CONTEXT PASS' \
	    'boot: HAL initialized successfully.' \
	    'xhci: PCI controller' \
	    'usb-storage: sda blocks=' \
	    'login:'; do
		if ! rg -a -F -q -- "$marker" "$guest_log"; then
			class=missing-marker
			first_failure="missing $marker"
			break
		fi
	done
fi
if [ "$class" = pass ] &&
    ! rg -a -q 'vfs: boot0 .* -> /dev/sda[0-9][0-9]* \(private FAT\)' \
	"$guest_log"; then
	class=missing-marker
	first_failure="missing private FAT boot-device resolution"
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

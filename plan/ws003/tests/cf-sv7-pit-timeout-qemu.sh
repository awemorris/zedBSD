#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 IMAGE OUTPUT-DIRECTORY" >&2
	exit 2
fi

image=$1
output=$2
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
boot_timeout=${BOOT_TIMEOUT_SECONDS:-30}

case $boot_timeout in
'' | *[!0-9]* | 0)
	echo "BOOT_TIMEOUT_SECONDS must be a positive integer" >&2
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
run_image=$output/pit-off.img
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

{
	echo "base_image=$image"
	echo "base_sha256=$base_digest"
	echo "run_image=$run_image"
	echo "qemu=$($qemu --version | sed -n '1p')"
	echo "boot_timeout_seconds=$boot_timeout"
	echo "topology=SeaBIOS q35 one CPU PIT disabled IDE disk"
} >"$metadata"

"$qemu" \
	-machine q35,pit=off \
	-m 512 \
	-smp 1 \
	-drive file="$run_image",format=raw,if=ide \
	-display none \
	-monitor none \
	-serial none \
	-debugcon file:"$guest_log" \
	-no-reboot >"$qemu_log" 2>&1 &
qemu_pid=$!

start=$(date +%s)
deadline=$((start + boot_timeout))
class=
first_failure=

while :; do
	now=$(date +%s)
	if rg -a -F -q 'A64 TIMER CAL TIMEOUT stage=out-low' "$guest_log" \
	    2>/dev/null; then
		class=pass
		break
	fi
	if rg -a -q 'A64 IRQ READY|boot: HAL initialized successfully' \
	    "$guest_log" 2>/dev/null; then
		class=false-pass
		first_failure="PIT-disabled guest reached the IRQ/HAL pass marker"
		break
	fi
	if [ "$now" -ge "$deadline" ]; then
		class=boot-timeout
		first_failure="explicit PIT out-low timeout was not observed"
		break
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		class=early-qemu-exit
		first_failure="QEMU exited before the PIT timeout marker"
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
	    'A64 TIMER CAL BEGIN' \
	    'A64 TIMER CAL TIMEOUT stage=out-low'; do
		if ! rg -a -F -q "$marker" "$guest_log"; then
			class=missing-marker
			first_failure="missing $marker"
			break
		fi
	done
fi
if [ "$class" = pass ] &&
    rg -a -q 'A64 IRQ READY|boot: HAL initialized successfully' "$guest_log";
then
	class=false-pass
	first_failure="PIT-disabled guest reached the IRQ/HAL pass marker"
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
	echo "CF-SV7 PIT timeout regression FAIL: $class: $first_failure" >&2
	echo "guest log: $guest_log" >&2
	echo "QEMU log: $qemu_log" >&2
	exit 1
fi

echo "CF-SV7 PIT timeout regression: PASS"
echo "guest log: $guest_log"

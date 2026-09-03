#!/bin/sh
# Bounded exact-device AX211/CNVio2 VFIO development runner.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

set -eu
umask 077

if [ "$#" -ne 2 ]; then
	echo "usage: sudo AX211_VFIO_SAFE_ROUTE_DEVICE=IFACE $0 IMAGE WORK-DIR" >&2
	exit 2
fi

image=$1
work_dir=$2
bdf=${AX211_VFIO_BDF:-0000:00:14.3}
safe_route_device=${AX211_VFIO_SAFE_ROUTE_DEVICE:-}
route_probe=${AX211_VFIO_ROUTE_PROBE:-10.0.10.1}
ovmf_code=${AX211_VFIO_OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
ovmf_vars=${AX211_VFIO_OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
run_timeout=${AX211_VFIO_RUN_TIMEOUT:-600}
# VFIO guests are intentionally non-migratable.  Expose the host invariant
# TSC so the guest may use the same stable counter contract as bare metal.
cpu=${AX211_VFIO_CPU:-host,+invtsc}
device=/sys/bus/pci/devices/$bdf
qemu_pid=

fail()
{
	echo "AX211-VFIO: $*" >&2
	exit 1
}

current_driver()
{
	if [ ! -L "$device/driver" ]; then
		echo none
	else
		basename "$(readlink -f "$device/driver")"
	fi
}

probe_route_device()
{
	ip route get "$route_probe" | awk '
		/ dev / { for (i = 1; i <= NF; i++) if ($i == "dev") { print $(i + 1); exit } }
	'
}

restore_host()
{
	restore_failed=0
	set +e
	if [ -n "$qemu_pid" ]; then
		kill "$qemu_pid" 2>/dev/null
		wait "$qemu_pid" 2>/dev/null
		qemu_pid=
	fi
	if [ "$(current_driver)" = vfio-pci ]; then
		printf '%s' "$bdf" > /sys/bus/pci/drivers/vfio-pci/unbind ||
			restore_failed=1
	fi
	# A newline, rather than a zero-byte write, clears driver_override.
	printf '\n' > "$device/driver_override" || restore_failed=1
	modprobe iwlwifi || restore_failed=1
	if [ "$(current_driver)" != iwlwifi ]; then
		printf '%s' "$bdf" > /sys/bus/pci/drivers_probe ||
			restore_failed=1
	fi
	restore_wait=0
	while [ "$(current_driver)" != iwlwifi ] && [ "$restore_wait" -lt 10 ]; do
		sleep 1
		restore_wait=$((restore_wait + 1))
	done
	[ "$(current_driver)" = iwlwifi ] || restore_failed=1
	driver_override=$(cat "$device/driver_override" 2>/dev/null)
	if [ "$?" -ne 0 ] ||
	    { [ -n "$driver_override" ] && [ "$driver_override" != '(null)' ]; };
	then
		restore_failed=1
	fi
	[ "$(probe_route_device)" = "$safe_route_device" ] ||
		restore_failed=1
	if [ -e "/dev/vfio/$group" ] &&
	    fuser "/dev/vfio/$group" >/dev/null 2>&1; then
		restore_failed=1
	fi
	if [ "$restore_failed" -eq 0 ]; then
		echo "AX211-VFIO: restored $bdf to iwlwifi" >&2
	else
		echo "AX211-VFIO: ERROR: host driver, route, or VFIO ownership was not restored" >&2
	fi
	return "$restore_failed"
}

finish()
{
	status=$?
	trap - EXIT
	trap '' HUP INT TERM
	if ! restore_host; then
		status=1
	fi
	exit "$status"
}

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -n "$safe_route_device" ] || fail "AX211_VFIO_SAFE_ROUTE_DEVICE is required"
[ -f "$image" ] || fail "image is missing: $image"
[ -f "$ovmf_code" ] || fail "OVMF code is missing: $ovmf_code"
[ -f "$ovmf_vars" ] || fail "OVMF variables are missing: $ovmf_vars"
[ -d "$device" ] || fail "PCI device is missing: $bdf"
[ "$(cat "$device/vendor")" = 0x8086 ] || fail "$bdf has the wrong vendor"
[ "$(cat "$device/device")" = 0x51f0 ] || fail "$bdf is not an AX211"
[ "$(cat "$device/subsystem_vendor")" = 0x8086 ] ||
	fail "$bdf has the wrong subsystem vendor"
[ "$(cat "$device/subsystem_device")" = 0x4090 ] ||
	fail "$bdf has the wrong subsystem device"
[ "$(cat "$device/revision")" = 0x01 ] || fail "$bdf has the wrong revision"
[ "$(cat "$device/class")" = 0x028000 ] || fail "$bdf has the wrong PCI class"
[ "$(current_driver)" = iwlwifi ] || fail "$bdf is not owned by iwlwifi"
case $run_timeout in
	''|*[!0-9]*|0) fail "AX211_VFIO_RUN_TIMEOUT must be a positive integer" ;;
esac
command -v timeout >/dev/null 2>&1 || fail "timeout is missing"
command -v fuser >/dev/null 2>&1 || fail "fuser is missing"

route_device=$(probe_route_device)
[ "$route_device" = "$safe_route_device" ] ||
	fail "route to $route_probe uses $route_device, expected $safe_route_device"
[ ! -e "$device/net/$safe_route_device" ] ||
	fail "safe route uses the AX211 network interface"

group=$(basename "$(readlink -f "$device/iommu_group")")
group_devices=$(find "/sys/kernel/iommu_groups/$group/devices" \
	-mindepth 1 -maxdepth 1 -printf '%f\n')
[ "$group_devices" = "$bdf" ] || fail "IOMMU group $group is not a singleton"
[ -e "$device/reset" ] || fail "$bdf has no PCI reset operation"

mkdir -p "$work_dir"
chmod 0700 "$work_dir"
cp --sparse=always "$image" "$work_dir/guest.img"
cp "$ovmf_vars" "$work_dir/OVMF_VARS_4M.fd"
chmod 0600 "$work_dir/guest.img" "$work_dir/OVMF_VARS_4M.fd"

trap finish EXIT
trap 'exit 130' HUP INT TERM
modprobe vfio-pci
printf '%s' vfio-pci > "$device/driver_override"
printf '%s' "$bdf" > /sys/bus/pci/drivers/iwlwifi/unbind
printf '%s' "$bdf" > /sys/bus/pci/drivers_probe
[ "$(current_driver)" = vfio-pci ] || fail "cannot bind $bdf to vfio-pci"

echo "AX211-VFIO: $bdf assigned; QEMU monitor is $work_dir/monitor.sock" >&2
timeout --foreground --kill-after=10 "${run_timeout}s" "$qemu" \
	-machine q35,accel=kvm \
	-cpu "$cpu" -m 1024 -smp 4 \
	-drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
	-drive if=pflash,format=raw,file="$work_dir/OVMF_VARS_4M.fd" \
	-device qemu-xhci,id=xhci \
	-drive if=none,id=usbboot,file="$work_dir/guest.img",format=raw \
	-device usb-storage,bus=xhci.0,drive=usbboot,id=bootstick,bootindex=1 \
	-device vfio-pci,host="$bdf",id=ax211 \
	-vga std -display none -serial none \
	-debugcon file:"$work_dir/debugcon.log" \
	-monitor unix:"$work_dir/monitor.sock",server=on,wait=off \
	-nic none -no-reboot &
qemu_pid=$!
wait "$qemu_pid"
qemu_pid=

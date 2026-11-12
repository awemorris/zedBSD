#!/bin/sh
# BUG-423's analysis run: boots OUTPUT/hdd-image.img on q35 (KVM, SMP CPUs, NVMe, qemu-xhci with usb-kbd on its
# second USB 2 port), waits WAIT seconds, then stops the guest through gdbstub (port GDB) and runs the gdb
# commands in the file GDBCMDS (default OUTPUT/gdbcmds, made when missing: saves the kernel's log buffer to OUTPUT/klog.bin), and stops QEMU.
#   plan/bugs/BUG-423/debug.sh OUTPUT SMP [WAIT] [GDB]     QEMU_EXTRA: more QEMU options (split on spaces)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
repo=$(cd "$(dirname "$0")/../../.." && pwd)
output=$(cd "${1:?output dir}" && pwd)
smp=${2:?cpus}
wait=${3:-40}
port=${4:-14230}
work=$output/dbg-work
mkdir -p "$work"
qemu-img create -q -f qcow2 -F raw -b "$output/hdd-image.img" "$work/disk.qcow2"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$work/vars.fd"
timeout $((wait + 300)) qemu-system-x86_64 -machine q35 -m 8G -smp "$smp" -accel kvm -cpu host \
	-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	-drive "if=pflash,format=raw,file=$work/vars.fd" \
	-device qemu-xhci,id=xhci \
	-drive "if=none,id=boot,file=$work/disk.qcow2,format=qcow2" \
	-device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
	-device usb-kbd,bus=xhci.0,port=2 \
	-vga std -display none -no-reboot \
	-qmp "unix:$work/qmp.sock,server,nowait" \
	-gdb "tcp:127.0.0.1:$port" ${QEMU_EXTRA:-} >"$work/qemu.log" 2>&1 &
pid=$!
sleep "$wait"
cmds=${GDBCMDS:-$output/gdbcmds}
if [ ! -f "$cmds" ]; then
	printf '%s\n' "dump binary memory $output/klog.bin &klog_buffer ((char *)&klog_buffer)+0x80000" > "$cmds"
fi
timeout 120 gdb -q -batch -ex "file $output/vmunix" -ex "target remote 127.0.0.1:$port" -x "$cmds" \
	>"$output/gdb.log" 2>&1
echo "gdb rc=$?"
kill "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
sync

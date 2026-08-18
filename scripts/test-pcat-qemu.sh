#!/usr/bin/env bash
# PC/AT QEMU boot regression matrix.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

if test "$#" -ne 3; then
	echo "usage: $0 VMUNIX HDD_IMAGE GRUB_ISO" >&2
	exit 2
fi
vmunix="$1"
hdd="$2"
iso="$3"
qemu="${QEMU_SYSTEM_I386:-qemu-system-i386}"
work="${ZEDBSD_BUILD_DIR:-$(dirname "$hdd")}/qemu-test"
mkdir -p "$work"

test -x "$(command -v "$qemu")" || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -f "$vmunix" && test -f "$hdd" && test -f "$iso"

run_boot()
{
	local label="$1" memory="$2" marker="$3"; shift 3
	local log="$work/$label.log" pid found=0
	rm -f "$log"
	"$qemu" -M pc -cpu 486 -m "$memory" -display none -serial none \
		-monitor none -no-reboot -no-shutdown \
		-debugcon "file:$log" -global isa-debugcon.iobase=0xe9 "$@" &
	pid=$!
	trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true' RETURN
	for _ in $(seq 1 150); do
		if test -f "$log" && grep -Fq "$marker" "$log"; then found=1; break; fi
		if ! kill -0 "$pid" 2>/dev/null; then break; fi
		sleep 0.1
	done
	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	trap - RETURN
	if test "$found" -ne 1; then
		echo "PC/AT QEMU $label: FAIL" >&2
		test -f "$log" && cat "$log" >&2
		return 1
	fi
	echo "PC/AT QEMU $label: PASS"
}

for memory in 16M 32M 64M; do
	run_boot "custom-${memory}" "$memory" 'boot: starting init /bin/sh' \
		-boot c -drive "if=ide,index=0,media=disk,format=raw,file=$hdd"
done

run_boot grub-64M 64M 'boot: starting init /bin/sh' \
	-boot d -cdrom "$iso" \
	-drive "if=ide,index=0,media=disk,format=raw,file=$hdd"
grep -Fq 'boot: Multiboot loader GRUB' "$work/grub-64M.log" || {
	echo "PC/AT QEMU grub-64M did not use GRUB" >&2; exit 1;
}

second="$work/second-drive.img"
cp --reflink=auto "$hdd" "$second"
run_boot two-drives-64M 64M 'boot: starting init /bin/sh' \
	-boot c -drive "if=ide,index=0,media=disk,format=raw,file=$hdd" \
	-drive "if=ide,index=1,media=disk,format=raw,file=$second"
grep -Fq 'ata: ide1 blocks=' "$work/two-drives-64M.log" || {
	echo "PC/AT QEMU two-drives-64M did not enumerate ide1" >&2; exit 1;
}

# Exercise the 8042 path, line editing, and the root namespace from the
# ring-3 shell.  The injected keys still travel through the PC/AT keyboard
# ISR, the TTY input normalizer, and /dev/console.
interactive_log="$work/interactive.log"
interactive_qmp="$work/interactive.qmp"
rm -f "$interactive_log" "$interactive_qmp"
"$qemu" -M pc -cpu 486 -m 64M -display none -serial none \
	-qmp "unix:$interactive_qmp,server=on,wait=off" -no-reboot -no-shutdown \
	-debugcon "file:$interactive_log" -global isa-debugcon.iobase=0xe9 \
	-boot c -drive "if=ide,index=0,media=disk,format=raw,file=$hdd" &
interactive_pid=$!
trap 'kill "$interactive_pid" 2>/dev/null || true; wait "$interactive_pid" 2>/dev/null || true' EXIT
python3 - "$interactive_qmp" "$interactive_log" <<'PY'
import json
import os
import socket
import sys
import time

qmp_path, log_path = sys.argv[1:]
deadline = time.time() + 15
sock = None
while time.time() < deadline:
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(qmp_path)
        break
    except OSError:
        time.sleep(0.05)
if sock is None:
    raise SystemExit("PC/AT QMP socket did not appear")
stream = sock.makefile("rw", encoding="utf-8")
json.loads(stream.readline())

def command(name, arguments=None):
    request = {"execute": name}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write(json.dumps(request) + "\n")
    stream.flush()
    while True:
        response = json.loads(stream.readline())
        if "return" in response:
            return response["return"]
        if "error" in response:
            raise SystemExit("QMP command failed: " + repr(response["error"]))

def log_bytes():
    try:
        with open(log_path, "rb") as source:
            return source.read()
    except FileNotFoundError:
        return b""

def wait_for(marker):
    limit = time.time() + 15
    while time.time() < limit:
        if marker in log_bytes():
            return
        time.sleep(0.05)
    raise SystemExit("console marker not observed: " + repr(marker))

def wait_for_new_prompt(previous):
    limit = time.time() + 15
    while time.time() < limit:
        if log_bytes().count(b"/ $ ") > previous:
            return
        time.sleep(0.05)
    raise SystemExit("new shell prompt was not observed")

def wait_for_after(first, second):
    limit = time.time() + 15
    while time.time() < limit:
        data = log_bytes()
        position = data.rfind(first)
        if position >= 0 and second in data[position + len(first):]:
            return
        time.sleep(0.05)
    raise SystemExit("console sequence not observed: " + repr((first, second)))

def type_command(keys):
    # The prompt is emitted just before readline has restored canonical input;
    # leave a short handoff interval after an external foreground process.
    time.sleep(0.25)
    if isinstance(keys, str):
        names = {" ": "spc", "/": "slash", "=": "equal",
                 "-": "minus", ".": "dot"}
        keys = [names.get(key, key) for key in keys]
    for key in keys:
        command("human-monitor-command", {"command-line": "sendkey " + key})
        time.sleep(0.025)
    command("human-monitor-command", {"command-line": "sendkey ret"})

command("qmp_capabilities")
wait_for(b"/ $ ")
type_command(list("pwd"))
wait_for(b"\n/\r\n/ $ ")
type_command(["e", "c", "o", "left", "h", "end", "spc", "e", "d", "i", "t"])
wait_for(b"\nedit\r\n/ $ ")
before_history = log_bytes().count(b"\nedit\r\n")
type_command(["up"])
limit = time.time() + 15
while time.time() < limit:
    if log_bytes().count(b"\nedit\r\n") > before_history:
        break
    time.sleep(0.05)
else:
    raise SystemExit("shell history recall was not observed")
type_command(list("missingzed"))
wait_for(b"sh: missingzed: not found")
if b"\nerror\r\n" in log_bytes():
    raise SystemExit("legacy generic shell error diagnostic was observed")
type_command(["l", "s", "spc", "slash", "d", "i", "s", "k", "1"])
wait_for(b"ls: /disk1:")
type_command("ls -l /etc/shadow")
wait_for(b"-r--------")
type_command("dmesg")
wait_for(b"boot: kernel heap, process, and scheduler initialization")
wait_for_after(b"boot: starting init /bin/sh", b"/ $ ")
type_command("ls -lh /bin/noct")
wait_for_after(b"noct", b"/ $ ")
type_command("dd if=/etc/passwd of=/home/passwd bs=16")
wait_for(b"records out")
if b"dd: /home/passwd:" in log_bytes():
    raise SystemExit("dd failed to create its output")
type_command("cmp /etc/passwd /home/passwd")
wait_for_after(b"cmp /etc/passwd /home/passwd", b"/ $ ")
if b"cmp: /home/passwd:" in log_bytes():
    raise SystemExit("dd output differs from its input")
type_command("who")
wait_for_after(b"who", b"/ $ ")
type_command("more /etc/passwd")
wait_for(b"root:x:0:0:")
wait_for_after(b"root:x:0:0:", b"/ $ ")
type_command("less /etc/passwd")
wait_for_after(b"root:x:0:0:", b"/etc/passwd  1/1")
command("human-monitor-command", {"command-line": "sendkey q"})
time.sleep(0.1)
command("human-monitor-command", {"command-line": "sendkey ret"})
wait_for_after(b"/etc/passwd  1/1", b"/ $ ")
type_command("login root")
wait_for(b"Password: ")
type_command("incorrect")
wait_for(b"Login incorrect")
PY
kill "$interactive_pid" 2>/dev/null || true
wait "$interactive_pid" 2>/dev/null || true
trap - EXIT
rm -f "$interactive_qmp"
echo "PC/AT QEMU keyboard, line editor, and root namespace test: PASS"

echo "PC/AT QEMU boot matrix: PASS"

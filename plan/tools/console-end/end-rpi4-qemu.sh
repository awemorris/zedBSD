#!/bin/bash
# The console's end over the firmware's screen on QEMU raspi4b, as a display
# driver's first flip asks for it, judged by the screen and the debugger.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
#   plan/tools/console-end/end-rpi4-qemu.sh BUILD_DIR
#
# BUILD_DIR holds the image of plan/tools/console-end/config-rpi4-end.mk (hdd-image.img,
# vmunix and its symbols kernel.elf beside it):
#   make ZEDBSD_CONFIG=plan/tools/console-end/config-rpi4-end.mk BUILD=BUILD_DIR disk-image
#
# One run: the overlay of the image, QEMU raspi4b with its QMP socket and a
# gdbstub (port GDB_PORT, default 2221), then
#   1. the screen: the test owner's "GRAPHICS OWNER", the login prompt, and the
#      login prompt again once the owner gave the screen back (the redraw);
#   2. the debugger, every 10 seconds, until the timed end has returned
#      (fb_end_test_done in boot-fb.c);
#   3. the debugger: what the end returned, the hardware layer's window state,
#      its level-0 entry, the early console's view (pixels) and its given-up mark, a
#      read through the window (which must fail), and the scheduler's ticks
#      twice 5 seconds apart (the kernel still runs);
#   4. the screen after the end (end.png).
# The summary is the last lines; PASS when every check holds.  Nothing is
# removed: the work directory is a new one below build/tmp.
set -u
root=$(cd "$(dirname "$0")/../../.." && pwd)
build=${1:?build dir}
gdb_port=${GDB_PORT:-2221}
image=$build/hdd-image.img
kernel=$build/vmunix
elf=$build/kernel.elf
mkdir -p "$root/build/tmp"
work=$(mktemp -d "$root/build/tmp/end-rpi4.XXXXXX")
dtb=$(sh "$root/plan/tools/rpi4-dtb.sh" "$image") || exit 1
monitor=$work/qmp.sock
serial=$work/serial.sock
disk=$work/sd.qcow2

# The SD card's overlay, grown to a power of two as QEMU's card must be.
qemu-img create -q -f qcow2 -F raw -b "$(readlink -f -- "$image")" "$disk" || exit 1
size=$(stat -c %s -- "$image")
card=1048576
while ((card < size)); do
	card=$((card * 2))
done
qemu-img resize -q -f qcow2 "$disk" "$card" || exit 1

# The machine, for at most 15 minutes.
timeout 900 qemu-system-aarch64 -machine raspi4b -m 2G -kernel "$kernel" -dtb "$dtb" \
	-drive "if=sd,format=qcow2,file=$disk" \
	-display none -serial "unix:$serial,server,nowait" -serial null -no-reboot \
	-qmp "unix:$monitor,server,nowait" -gdb "tcp:127.0.0.1:$gdb_port" \
	>"$work/qemu.log" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null' EXIT

# The symbols the debugger reads, by address (a static name may repeat).
symbol() {
	"$root/build/llvm/bin/llvm-nm" "$elf" | awk -v name="$1" '$3 == name { print $1; exit }'
}
done_at=$(symbol fb_end_test_done)
result_at=$(symbol fb_end_test_result)
# The compiler keeps the window's record as one symbol per field (fb_window.0
# the state, fb_window.3 the address) or as one record (fb_window).
window_at=$(symbol fb_window.0)
window_address_at=$(symbol fb_window.3)
if [[ -z $window_at ]]; then
	window_at=$(symbol fb_window)
	[[ -n $window_at ]] && window_address_at=$(printf '%x' $((0x$window_at + 24)))
fi
pixels_at=$(symbol pixels)
l0_at=$(symbol system_kernel_l0)
ticks_at=$(symbol scheduler_ticks)
given_up_at=$(symbol pixels_given_up)
if [[ -z $done_at || -z $result_at || -z $window_at || -z $window_address_at || -z $l0_at || -z $ticks_at || -z $given_up_at || -z $pixels_at ]]; then
	echo "end-rpi4: FAIL a symbol is missing from $elf"
	exit 1
fi

# Runs debugger commands against the running guest, which continues afterwards.
debug() {
	local commands=()
	local line
	for line in "$@"; do
		commands+=(-ex "$line")
	done
	timeout 60 gdb-multiarch -nx -batch -ex 'set architecture aarch64' \
		-ex "target remote 127.0.0.1:$gdb_port" "${commands[@]}" -ex detach 2>&1
}

# Reads one 64-bit word at a kernel address, or "error".
word() {
	debug "x/1gx 0x$1" | awk '/^0x[0-9a-f]+.*:/ { print $NF; found = 1 } END { if (!found) print "error" }' | tail -n 1
}

# 1. The screen: the owner, the login prompt, and the login prompt after the owner.
status=0
python3 "$root/plan/tools/boot-test.py" --monitor "$monitor" --screenshot "$work/login.png" \
	--frame "$work/frame.png" --timeout 600 --interval 1 \
	--pattern 'GRAPHICS OWNER' --pattern '^(?:\S+ )?login: ?' \
	--serial "$serial" --then-expect='^(?:\S+ )?login: ?' >"$work/screen.log" 2>&1 || status=$?
echo "end-rpi4: screen reading status $status (owner, login, login again) $work/screen.log"
[[ $status -eq 0 ]] || { echo "end-rpi4: FAIL screen"; exit 1; }

# 2. Waits for the timed end, at most 5 minutes.
done_value=0
for _ in $(seq 1 30); do
	done_value=$(word "$done_at")
	if [[ $done_value == 0x* ]] && (( (done_value & 0xffffffff) == 1 )); then
		break
	fi
	sleep 10
done

# 3. What the end left.
result=$(word "$result_at")
state=$(word "$window_at")
address=$(word "$window_address_at")
pixels=$(word "$pixels_at")
l0_entry=$(word "$(printf '%x' $((0x$l0_at + 16)))")
given_up=$(word "$given_up_at")
through=$(debug "x/1wx $address" | tail -n 2 | tr '\n' ' ')
ticks_first=$(word "$ticks_at")
sleep 5
ticks_second=$(word "$ticks_at")

# 4. The screen after the end.
python3 - "$monitor" "$work/end.png" <<'EOF'
import json, socket, sys
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
f = s.makefile("rw")
f.readline()
for cmd in ({"execute": "qmp_capabilities"},
            {"execute": "screendump", "arguments": {"filename": sys.argv[2], "format": "png"}}):
    f.write(json.dumps(cmd) + "\n")
    f.flush()
    while True:
        reply = json.loads(f.readline())
        if "return" in reply or "error" in reply:
            break
EOF

echo "end-rpi4: done=$done_value result=$result window.state=$state window.address=$address l0[2]=$l0_entry given_up=$given_up pixels=$pixels"
echo "end-rpi4: read through the window: $through"
echo "end-rpi4: ticks $ticks_first then $ticks_second"
echo "end-rpi4: screens $work/login.png $work/login-after.png $work/end.png"
fail=0
(( (done_value & 0xffffffff) == 1 )) || { echo "end-rpi4: FAIL the end did not return"; fail=1; }
(( (result & 0xffffffff) == 0 )) || { echo "end-rpi4: FAIL the end returned $result"; fail=1; }
(( (state & 0xffffffff) == 3 )) || { echo "end-rpi4: FAIL the window is not removed ($state)"; fail=1; }
(( l0_entry == 0 )) || { echo "end-rpi4: FAIL the window's level-0 entry stays ($l0_entry)"; fail=1; }
(( (given_up & 0xffffffff) == 1 )) || { echo "end-rpi4: FAIL the early console did not give the framebuffer up"; fail=1; }
[[ $pixels == 0x0000000000000000 ]] || { echo "end-rpi4: FAIL the early console keeps a view ($pixels)"; fail=1; }
[[ $through == *"Cannot access memory"* ]] || { echo "end-rpi4: FAIL the window is still readable"; fail=1; }
(( ticks_second > ticks_first )) || { echo "end-rpi4: FAIL the kernel's ticks stopped"; fail=1; }
if ((fail == 0)); then
	echo "end-rpi4: PASS $work"
else
	echo "end-rpi4: FAIL $work"
fi
exit "$fail"

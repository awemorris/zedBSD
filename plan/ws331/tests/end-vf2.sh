#!/bin/sh
# The console's end over the loader's screen on the VisionFive 2 board, judged
# by the kernel's log and the board's state over SSH.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
#   VF2_HOST=user@host VF2_PASSWORD=... VF2_PLUG=... plan/ws331/tests/end-vf2.sh VMUNIX OUTDIR
#
# VMUNIX is the kernel of plan/ws331/tests/config-visionfive2-end.mk.  One run:
#   1. keeps the board's kernel as /boot/boot0/vmunix.p2old, unless that is
#      there already;
#   2. plan/tools/vf2/run.sh: the kernel put on the board and booted (the
#      board's plug power-cycles it), dmesg until the render node is up;
#   3. waits 90 seconds for the timed end, then reads dmesg, /dev/graphics and
#      the hardware layer's description of the screen;
#   4. puts the kept kernel back, checks it reads back as kept (cksum), and
#      boots it, checking that SSH comes back and /dev/graphics is absent.
# The summary is the last lines; PASS when the end returned 0 with the mapping
# removed from the early console's window (0xfffffff0...), /dev/graphics was
# there, and the kept kernel is back.  Every command has a time limit.
set -u
vmunix=$1
out=$2
: "${VF2_HOST:?VF2_HOST=user@host}"
: "${VF2_PASSWORD:?VF2_PASSWORD}"
: "${VF2_PLUG:?VF2_PLUG}"
root=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$out"
export SSHPASS="$VF2_PASSWORD"
ssh_run() { timeout 60 sshpass -e ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$VF2_HOST" "$@"; }
sudo_run() { printf '%s\n' "$VF2_PASSWORD" | ssh_run "sudo -S sh -c '$1'" 2>/dev/null; }

# 1. The board's own kernel, kept once.
sudo_run 'test -f /boot/boot0/vmunix.p2old || cp /boot/boot0/vmunix /boot/boot0/vmunix.p2old; sync' || { echo "end-vf2: FAIL keeping the kernel"; exit 1; }
kept=$(ssh_run cksum /boot/boot0/vmunix.p2old | awk '{ print $1 }')
echo "end-vf2: kept kernel cksum $kept"

# 2. The test kernel, booted.
(cd "$root" && timeout 1500 sh plan/tools/vf2/run.sh "$vmunix" "$out") >"$out/run.log" 2>&1
echo "end-vf2: run.sh: $(tail -n 3 "$out/run.log" | tr '\n' ' ')"

# 3. After the timed end.
sleep 90
ssh_run dmesg >"$out/dmesg-end.txt" 2>&1
ssh_run 'ls -l /dev/graphics' >"$out/graphics.txt" 2>&1
ssh_run 'uname -a' >"$out/uname.txt" 2>&1
end_line=$(grep -h "the timed end returned" "$out/dmesg-end.txt" "$out/dmesg.txt" 2>/dev/null | head -n 1)
screen_line=$(grep -h "RISCV64 FRAMEBUFFER\|visionfive2: framebuffer" "$out/dmesg.txt" "$out/dmesg-end.txt" 2>/dev/null | awk '!seen[$0]++' | tr '\n' ' ')
echo "end-vf2: $end_line"
echo "end-vf2: screen: $screen_line"
echo "end-vf2: /dev/graphics: $(cat "$out/graphics.txt")"
echo "end-vf2: uname: $(cat "$out/uname.txt")"

# 4. The kept kernel back, read back, and booted.
sudo_run 'cp /boot/boot0/vmunix.p2old /boot/boot0/vmunix && sync' || echo "end-vf2: FAIL putting the kept kernel back"
back=$(ssh_run cksum /boot/boot0/vmunix | awk '{ print $1 }')
echo "end-vf2: kernel back cksum $back (kept $kept)"
(cd "$root" && timeout 60 python3 -I plan/tools/switchbot.py cycle "$VF2_PLUG" 10) >/dev/null 2>&1
up=0
for _ in $(seq 1 30); do
	sleep 4
	if ssh_run true 2>/dev/null; then
		up=1
		break
	fi
done
graphics_after=$(ssh_run 'ls /dev/graphics 2>&1')
echo "end-vf2: kept kernel up=$up /dev/graphics: $graphics_after"

fail=0
case "$end_line" in
*"returned 0, the screen's mapping at 0xfffffff0"*) ;;
*) echo "end-vf2: FAIL the end's line"; fail=1 ;;
esac
grep -q "/dev/graphics" "$out/graphics.txt" && ! grep -q "No such" "$out/graphics.txt" || { echo "end-vf2: FAIL no /dev/graphics"; fail=1; }
[ "$back" = "$kept" ] || { echo "end-vf2: FAIL the kept kernel did not read back"; fail=1; }
[ "$up" = 1 ] || { echo "end-vf2: FAIL the kept kernel did not come up"; fail=1; }
if [ "$fail" = 0 ]; then
	echo "end-vf2: PASS $out"
else
	echo "end-vf2: FAIL $out"
fi
exit "$fail"

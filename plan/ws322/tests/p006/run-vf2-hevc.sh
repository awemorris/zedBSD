#!/bin/sh
# ws322-p006 on the VisionFive 2, in one run: chagall.bin put at the driver's path (when the board lacks it), BUILD's
# kernel (plan/ws322/tests/config-vf2-video-test.mk: the Wave511 as the render node's video engine, /dev/wave5-test)
# booted, then on that boot:
#   L   vkvideo-probe --list (the family's codec operations and the video extensions)
#   H   vkvideo-probe --expect on every stream of HEVC (make-hevc.sh), each run's whole output kept (OUT/h/<name>.txt),
#       with the engine's counts (/dev/wave5-test) and the kernel lines the run added
#   G   vkcompute (the GPU still computes after the decodes)
# At the end the files put on the board are removed, the board's own kernel goes back (plan/tools/vf2/run.sh) and the
# board's files are checked by their cksum.  Everything else the board gets is under /tmp.  The password comes from
# the environment, never from this file.
#   VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=... VF2_PLUG=AC276E432AE6 sh plan/ws322/tests/p006/run-vf2-hevc.sh OUT BUILD HEVC FIRMWARE
# OUT/summary.txt holds what is printed: per stream "H <name> exit=<e> ..." (0 every frame matches, 4 a frame differs,
# 5 a decode failed, 6 refused: a profile, depth or size the decoder does not take), and the counts at the end.
# NAMES (default every stream of HEVC/list.txt) narrows the streams.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: run-vf2-hevc.sh OUT BUILD HEVC FIRMWARE}
build=${2:?the build directory}
hevc=${3:?the HEVC streams}
firmware=${4:?the firmware directory}
: "${VF2_HOST:?VF2_HOST=user@host}"
: "${VF2_PASSWORD:?VF2_PASSWORD}"
: "${VF2_PLUG:?VF2_PLUG}"
export SSHPASS="$VF2_PASSWORD" VF2_PLUG
export PATH=/usr/bin:/bin:$PATH
SSHOPT="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8"
mkdir -p "$out/h"
summary=$out/summary.txt
: > "$summary"
say() { echo "$*" | tee -a "$summary"; }
b() { timeout "${BT:-60}" sshpass -e ssh $SSHOPT "$VF2_HOST" "$@"; }
bsudo() { printf '%s\n' "$VF2_PASSWORD" | b "sudo -S sh -c '$1'" 2> /dev/null; }
bcp() { timeout "${BT:-600}" sshpass -e scp $SSHOPT "$@"; }
old=q1825orig
kernel=$build/vmunix
H=/tmp/p6
F=/lib/firmware/wave511
names=${NAMES:-$(cut -f1 "$hevc/list.txt")}

say "start $(date +%H:%M) kernel $(cksum < "$kernel")"
say "board before $(b "cksum /boot/boot0/vmunix /boot/boot0/vmunix.$old /etc/group /boot/boot0/zedbsd.cfg; df -k / /tmp; ls $F" 2>&1 | tr '\n' ' ')"

# The firmware at the driver's path, before the boot; remembered when the board did not have it.
added=0
if ! b "test -f $F/chagall.bin" > /dev/null 2>&1; then
	added=1
	bcp "$firmware/chagall.bin" "$VF2_HOST:/tmp/chagall.bin" || { say "FAIL the firmware was not copied"; exit 1; }
	bsudo "mkdir -p $F && cp /tmp/chagall.bin $F/chagall.bin && rm -f /tmp/chagall.bin && sync"
fi
say "firmware added=$added $(b "cksum $F/chagall.bin" 2>&1)"

# 1. BUILD's kernel booted, the board's own read back first.
mkdir -p "$out/regress"
bcp "$VF2_HOST:/boot/boot0/vmunix.$old" "$out/regress/board-vmunix" || { say "FAIL the board's kernel was not read back"; exit 1; }
sync
timeout 900 sh plan/tools/vf2/run.sh "$kernel" "$out/boot" > "$out/boot.txt" 2>&1
say "boot $(tail -3 "$out/boot.txt" | tr '\n' ' ')"
b "mkdir -p $H/bin $H/lib $H/hevc $H/h"
bcp "$build/bin/vkvideo-probe" "$build/bin/vkcompute" "$VF2_HOST:$H/bin/" &&
	bcp "$build"/dynamic/*.so "$VF2_HOST:$H/lib/" ||
	say "FAIL the programs were not copied"
for n in $names; do
	bcp "$hevc/$n.h265" "$hevc/$n.sha256" "$VF2_HOST:$H/hevc/" > /dev/null || say "FAIL $n was not copied"
done

# 2. The checks on the board.
printf '%s\n' $names > "$out/names.txt"
bcp "$out/names.txt" "$VF2_HOST:$H/names.txt" || say "FAIL the names were not copied"
cat > "$out/board.sh" <<'BOARD'
H=/tmp/p6
export LD_LIBRARY_PATH=$H/lib
P=$H/bin/vkvideo-probe
T() { echo "$1 test $(cat /dev/wave5-test)"; }
mark() { dmesg > $H/k.before; echo clear > /dev/wave5-test; }
seen() { dmesg > $H/k.after; grep -F -x -v -f $H/k.before $H/k.after | grep -v dw-mmc | grep -iE 'wave5|vf2-vpu|video|fault|lost|fatal|panic|reset|hang|refus|fail|recover|stopped|differs' | head -${2:-8} | sed "s/^/$1 kernel /"; }
echo "S0 boot $(ls /boot/boot0/log | tail -1)"
dmesg | grep -E 'vf2-vpu|wave5' | head -4 | sed 's/^/S0 /'
$P --list 2>&1 | sed 's/^/L /'
for n in $(cat $H/names.txt); do
	s=$H/hevc/$n.h265
	[ -f "$s" ] || continue
	mark
	timeout 900 $P --time --expect=$H/hevc/$n.sha256 $s > $H/h/$n.txt 2>&1
	e=$?
	echo "H $n exit=$e reference=$(wc -l < $H/hevc/$n.sha256) $(grep -c MISMATCH $H/h/$n.txt) mismatches $(grep -c ' status ' $H/h/$n.txt) failed | $(grep -vE 'MISMATCH| match$| status |capabilities|session |parameters ' $H/h/$n.txt | tail -3 | tr '\n' ' ')"
	T "H-$n" | grep -v ' errors 0 '; seen "H-$n" 4
done
mark
timeout 120 $H/bin/vkcompute > $H/g.txt 2>&1
echo "G vkcompute exit=$? $(tail -1 $H/g.txt)"
seen G 4
dmesg > $H/dmesg-end.txt
BOARD
bcp "$out/board.sh" "$VF2_HOST:$H/check.sh" || say "FAIL the board's script was not copied"
printf '%s\n' "$VF2_PASSWORD" | BT=7000 b "sudo -S sh $H/check.sh" > "$out/board.txt" 2>&1
sed 's/^\[sudo\][^:]*: //' "$out/board.txt" | tee -a "$summary"
bcp "$VF2_HOST:$H/h/*.txt" "$out/h/" > /dev/null 2>&1
bcp "$VF2_HOST:$H/dmesg-end.txt" "$out/" > /dev/null 2>&1
say "H every frame: $(grep -c '^H .* exit=0 ' "$out/board.txt"), refused: $(grep -c '^H .* exit=6 ' "$out/board.txt"), other: $(grep '^H .* exit=' "$out/board.txt" | grep -vc ' exit=[06] '), of $(grep -c '^H .* exit=' "$out/board.txt")"

# 3. The board as it was: the files removed, its own kernel back, checked by cksum.
bsudo "rm -rf $H; sync"
[ "$added" = 1 ] && bsudo "rm -rf $F; sync"
sync
timeout 900 sh plan/tools/vf2/run.sh "$out/regress/board-vmunix" "$out/restore" > "$out/restore.txt" 2>&1
say "restore $(tail -2 "$out/restore.txt" | tr '\n' ' ')"
say "board after $(b "cksum /boot/boot0/vmunix /boot/boot0/vmunix.$old /etc/group /boot/boot0/zedbsd.cfg; ls /tmp $F; uname -sm" 2>&1 | tr '\n' ' ')"
say "own kernel $(cksum < "$out/regress/board-vmunix")"
say "end $(date +%H:%M)"
sync

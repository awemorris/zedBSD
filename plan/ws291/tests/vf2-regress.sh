#!/bin/sh
# ws291-p020: the VisionFive 2's PowerVR regression of one tree's kernel (the system level cache's setting before it
# becomes the default, the render's waits): vkmatrix (one process, three at once, three runs), vkcoherency, vkcompute
# and the desktop (the compositor with Files' desktop and a Terminal, the screen's PNG by keiland-shot), run from the
# top of the tree while the board is assigned:
#   VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=... VF2_PLUG=14C19FA80F86 [OLD=p2old] [KEEP=1] [REBOOT=1] [KERNEL=vmunix]
#       sh plan/ws291/tests/vf2-regress.sh OUT BUILD
# KERNEL is another build's kernel to run BUILD's programs on (BUILD's own by default).
# BUILD is the tree's build of plan/ws291/tests/config-vf2-shot.mk (its vmunix, bin/wayland, bin/files, bin/terminal,
# bin/keiland-shot, bin/vkmatrix, bin/vkcoherency, bin/vkcompute and the dynamic libraries).  The board's kernel is kept
# on the board as vmunix.OLD (when not kept already: the original) and read back (OUT/board-vmunix); BUILD's kernel is
# put in and the board power-cycled (plan/tools/vf2/run.sh) unless it runs it already; the programs go to the user's
# ~/p7 (the board's system files are not changed: they run with LD_LIBRARY_PATH) and run as root.  The kernel lines
# each step added (faults, lost sessions, resets) and whether the board restarted in between (the newest of the board's
# boot logs, /boot/boot0/log, at the start and the end) are kept.  KEEP=1 leaves BUILD's kernel on the board; otherwise the board's own kernel is put back,
# checked by reading it back, and the board power-cycled.  OUT/summary.txt holds what is printed.  The password comes
# from the environment, never from this file.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: vf2-regress.sh OUT BUILD}
build=${2:?the build directory}
kernel=${KERNEL:-$build/vmunix}
: "${VF2_HOST:?VF2_HOST=user@host}"
: "${VF2_PASSWORD:?VF2_PASSWORD}"
: "${VF2_PLUG:?VF2_PLUG (the board's SwitchBot plug)}"
export SSHPASS="$VF2_PASSWORD" VF2_PLUG
SSHOPT="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8"
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

b() {
	timeout "${BT:-60}" sshpass -e ssh $SSHOPT "$VF2_HOST" "$@"
}

bsudo() {
	printf '%s\n' "$VF2_PASSWORD" | b "sudo -S sh -c '$1'" 2> /dev/null
}

bcp() {
	timeout "${BT:-300}" sshpass -e scp $SSHOPT "$@"
}

say "start $(date +%H:%M)"
say "board $(b 'uname -sm; cksum /boot/boot0/vmunix' 2>&1 | tr '\n' ' ')"
old=/boot/boot0/vmunix.${OLD:-p2old}
bsudo "test -f $old || cp -p /boot/boot0/vmunix $old; sync"
bcp "$VF2_HOST:$old" "$out/board-vmunix" || { say "FAIL the board's kernel was not read back"; exit 1; }
say "kept $(b "cksum $old" 2>&1)"

running=$(b 'cksum < /boot/boot0/vmunix' 2>/dev/null | cut -d' ' -f1)
if [ "$running" = "$(cksum < "$kernel" | cut -d' ' -f1)" ] && [ "${REBOOT:-0}" != 1 ]; then
	say "boot skipped: the board already has BUILD's kernel"
else
	timeout 900 sh plan/tools/vf2/run.sh "$kernel" "$out/boot" > "$out/boot.txt" 2>&1
	say "boot $(tail -3 "$out/boot.txt" | tr '\n' ' ')"
fi
say "kernel $(cksum < "$kernel" | cut -d' ' -f1) on the board: $(b 'cksum /boot/boot0/vmunix; sysctl hw.gpu.powervr.stages' 2>&1 | tr '\n' ' ')"

# The programs and the libraries in the user's ~/p7.
b 'mkdir -p p7/bin p7/lib p7/run && chmod 700 p7/run'
bcp "$build/bin/wayland" "$build/bin/files" "$build/bin/terminal" "$build/bin/keiland-shot" "$build/bin/vkmatrix" \
    "$build/bin/vkcoherency" "$build/bin/vkcompute" "$VF2_HOST:p7/bin/" &&
    bcp "$build"/dynamic/*.so "$VF2_HOST:p7/lib/" || say "FAIL the files were not copied"
H=$(b 'echo $HOME')/p7

# The board's side: each step's verdict line, and the kernel lines it added.
cat > "$out/board.sh" <<EOF
H=$H
export LD_LIBRARY_PATH=\$H/lib XDG_RUNTIME_DIR=\$H/run
lines() { dmesg | wc -l | tr -d ' '; }
boot() { ls /boot/boot0/log | tail -1; }
echo "start boot \$(boot)"
step() {
	name=\$1; limit=\$2; shift 2
	before=\$(lines); dmesg > \$H/r-\$name.before
	timeout \$limit "\$@" > \$H/r-\$name.txt 2>&1; status=\$?
	after=\$(lines); dmesg > \$H/r-\$name.after
	echo "\$name exit=\$status dmesg \$before->\$after: \$(grep -E 'PASS|FAIL|passed|failed|ok|fail' \$H/r-\$name.txt | tail -2 | tr '\n' ' ')"
	grep -F -x -v -f \$H/r-\$name.before \$H/r-\$name.after | grep -v dw-mmc | grep -iE 'fault|lost|fatal|panic|reset|quarantine|hang|abandon|refus|fail' | head -5 | sed "s/^/\$name kernel /"
}
step vkmatrix 900 \$H/bin/vkmatrix
step vkmatrix-p3 1200 \$H/bin/vkmatrix -p 3
step vkmatrix-r3 1500 \$H/bin/vkmatrix -r 3
step vkcoherency 900 \$H/bin/vkcoherency
step vkcompute 300 \$H/bin/vkcompute
# The desktop: the compositor with Files' desktop, a Terminal after it, the screen's PNG.
before=\$(lines); dmesg > \$H/r-desk.before
rm -f \$H/run/wayland-0 \$H/desk.png
\$H/bin/wayland --socket=\$H/run/wayland-0 "--desktop-client=\$H/bin/files --desktop" > \$H/desk-wayland.log 2>&1 < /dev/null &
wpid=\$!
sleep 15
WAYLAND_DISPLAY=wayland-0 \$H/bin/terminal > \$H/desk-terminal.log 2>&1 < /dev/null &
tpid=\$!
sleep 12
timeout 60 \$H/bin/keiland-shot --socket \$H/run/keiland-shot.sock \$H/desk.png > \$H/desk-shot.txt 2>&1
echo "desk shot exit=\$? \$(tr '\n' ' ' < \$H/desk-shot.txt)"
kill \$tpid 2> /dev/null; sleep 2; kill \$wpid 2> /dev/null; sleep 3; kill -9 \$wpid 2> /dev/null
pkill -f "\$H/bin/files" 2> /dev/null
after=\$(lines); dmesg > \$H/r-desk.after
echo "desk dmesg \$before->\$after: \$(grep -cE 'KWL (FAILED|VULKAN_ERROR)' \$H/desk-wayland.log) compositor errors, \$(grep -c 'KWL DESKTOP start' \$H/desk-wayland.log) desktop starts"
grep -E 'KWL (FAILED|VULKAN_ERROR)' \$H/desk-wayland.log | head -3 | sed 's/^/desk compositor /'
grep -F -x -v -f \$H/r-desk.before \$H/r-desk.after | grep -v dw-mmc | grep -iE 'fault|lost|fatal|panic|reset|quarantine|hang|abandon|refus|fail' | head -5 | sed "s/^/desk kernel /"
echo "end dmesg \$(lines) lines, boot \$(boot)"
EOF
bcp "$out/board.sh" "$VF2_HOST:p7/regress.sh" || say "FAIL the board's script was not copied"
printf '%s\n' "$VF2_PASSWORD" | BT=6000 b "sudo -S sh $H/regress.sh" > "$out/board.txt" 2>&1
sed 's/^\[sudo\][^:]*: //' "$out/board.txt" | tee -a "$summary"
bcp "$VF2_HOST:p7/desk.png" "$out/" > /dev/null 2>&1 && say "desk png $(cksum < "$out/desk.png")"
for f in vkmatrix vkmatrix-p3 vkmatrix-r3 vkcoherency vkcompute; do bcp "$VF2_HOST:p7/r-$f.txt" "$out/" > /dev/null 2>&1; done
bcp "$VF2_HOST:p7/desk-wayland.log" "$out/" > /dev/null 2>&1
say "after $(b 'dmesg | wc -l; cksum /boot/boot0/vmunix' 2>&1 | tr '\n' ' ')"

if [ "${KEEP:-0}" = 1 ]; then
	say "kept BUILD's kernel on the board (KEEP=1); board $(b "cksum /boot/boot0/vmunix $old" 2>&1 | tr '\n' ' ')"
	say "end $(date +%H:%M)"
	exit 0
fi

timeout 900 sh plan/tools/vf2/run.sh "$out/board-vmunix" "$out/back" > "$out/back.txt" 2>&1
say "back $(tail -3 "$out/back.txt" | tr '\n' ' ')"
say "board now $(b "cksum /boot/boot0/vmunix $old; grep -c login /boot/boot0/zedbsd.cfg" 2>&1 | tr '\n' ' ') expected $(cksum < "$out/board-vmunix" | cut -d' ' -f1)"
say "end $(date +%H:%M)"

#!/bin/sh
# Notes on the RTX 2070 machine (GPU_PERF_MACHINE=rtx, 10.0.10.3, nvrtx), in one command: the test kernel (with the
# AAT's input injection, plan/tools/rtx-video/config-amd64-rtx-inject.mk) deployed and zedBSD up on its own userland,
# aat-input copied in, then Notes started as the session's user: a shot when it shows (OUT/n1.png), two strokes
# dragged with the pen (filled through the stencil, OUT/n2.png), T (the text tool), a click on the page, "Hello
# RTX" typed, a shot (OUT/n3.png), Escape and Ctrl+W.  Kept in OUT: Notes's lines,
# the compositor's lines of the run (session.log), the kernel's nvrtx lines; a summary is printed (OUT/summary.txt).
# zedBSD stays up with the test kernel (rtx-p004.sh's restore step puts the machine back).
#   usage: plan/tools/rtx-video/rtx-notes-check.sh OUT VMUNIX AAT_INPUT [NOTES]   (from the top of the tree; NOTES: a
#          Notes to run instead of the machine's /bin/notes)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
OUT=${1:?usage: rtx-notes-check.sh OUT VMUNIX AAT_INPUT [NOTES]}
VMUNIX=${2:?usage: rtx-notes-check.sh OUT VMUNIX AAT_INPUT [NOTES]}
AAT_INPUT=${3:?usage: rtx-notes-check.sh OUT VMUNIX AAT_INPUT [NOTES]}
NOTES=${4:-}
export GPU_PERF_MACHINE=${GPU_PERF_MACHINE:-rtx}
MEASURE="sh plan/tools/gpu-perf/measure.sh"
top=$(pwd)
. plan/tools/gpu-perf/conf.sh
KEY=${KEY:-$top/plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
mkdir -p "$OUT" "$top/build/tmp"
ZSSHOPT="$SSHOPT -o ControlMaster=auto -o ControlPath=$top/build/tmp/ssh-%h -o ControlPersist=600"
SUMMARY=$OUT/summary.txt
: > "$SUMMARY"
AS_USER='su $(awk -F: '"'"'$3 == 1000 { print $1; exit }'"'"' /etc/passwd) -c'
ENVS="env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0"
SESSION=/run/user/1000/session.log

say() {
	echo "$(date +%H:%M:%S) $*" | tee -a "$SUMMARY"
}

# zs IP COMMAND: a command on zedBSD as root, at most ZS_TIMEOUT seconds.
zs() {
	zip=$1
	shift
	timeout "${ZS_TIMEOUT:-60}" ssh -i "$KEY" $ZSSHOPT "root@$zip" "$@"
}

# input COMMAND...: one aat-input command on zedBSD.
input() {
	zs "$IP" "/tmp/aat-input $*" < /dev/null >> "$OUT/input.log" 2>&1
}

# shot NAME: the desktop's picture into OUT/NAME.png.
shot() {
	ZS_TIMEOUT=30 zs "$IP" "env XDG_RUNTIME_DIR=/run/user/1000 keiland-shot /tmp/$1.png > /dev/null 2>&1; cat /tmp/$1.png" > "$OUT/$1.png" 2> /dev/null < /dev/null
}

# The kernel in place and zedBSD up.
$MEASURE deploy --kernel "$VMUNIX" > "$OUT/deploy.log" 2>&1 || { say "FAIL deploy (deploy.log)"; exit 1; }
IP=$($MEASURE zedbsd 2> "$OUT/zedbsd.log") || { say "FAIL zedbsd not up"; exit 1; }
say "zedbsd $IP kernel $(zs "$IP" "cksum < /boot/boot0/vmunix" < /dev/null | cut -d' ' -f1) (wanted $(cksum < "$VMUNIX" | cut -d' ' -f1)) | $(zs "$IP" "uname -v" < /dev/null)"
say "machine's notes: $(zs "$IP" "ls -l /bin/notes 2>&1; cksum < /bin/notes 2>&1" < /dev/null | tr '\n' ' ')"
timeout 120 scp -i "$KEY" $ZSSHOPT "$AAT_INPUT" "root@$IP:/tmp/aat-input" > /dev/null 2>&1 || { say "FAIL copy aat-input"; exit 1; }
program=/bin/notes
if [ -n "$NOTES" ]; then
	timeout 120 scp -i "$KEY" $ZSSHOPT "$NOTES" "root@$IP:/tmp/notes" > /dev/null 2>&1 || { say "FAIL copy notes"; exit 1; }
	program=/tmp/notes
fi
zs "$IP" "chmod 755 /tmp/aat-input /tmp/notes 2> /dev/null; /tmp/aat-input stop > /dev/null 2>&1; /tmp/aat-input start --width 1920 --height 1080" > "$OUT/input-start.log" 2>&1 < /dev/null
say "aat-input: $(tr '\n' ' ' < "$OUT/input-start.log")"

# Notes as the session's user, for 40 s at most.
counts=$(zs "$IP" "echo \$(wc -l < $SESSION) \$(dmesg | wc -l)" < /dev/null)
zs "$IP" "$AS_USER \"$ENVS $program --timeout-s=40\" > /tmp/notes.log 2>&1 < /dev/null &" < /dev/null
sleep 8
shot n1
input drag 760 520 1160 700 40
sleep 1
input drag 760 700 1160 520 40
sleep 2
shot n2
input key t
input click left 960 800
sleep 1
input "type 'Hello RTX'"
sleep 2
shot n3
input key esc
sleep 1
input key ctrl+w
sleep 3

# The lines of the run.
zs "$IP" "cat /tmp/notes.log" > "$OUT/notes.log" 2>&1 < /dev/null
set -- $counts
zs "$IP" "tail -n +$(( ${1:-0} + 1 )) $SESSION" > "$OUT/session.log" 2>&1 < /dev/null
zs "$IP" "dmesg | tail -n +$(( ${2:-0} + 1 )) | grep -E 'nvrtx'" > "$OUT/kernel.log" 2>&1 < /dev/null
zs "$IP" "/tmp/aat-input stop" > /dev/null 2>&1 < /dev/null

# The summary.
say "NOTES: $(grep -E 'NOTES (START|EXIT|DISCONNECTED)|NOTES .* none|notes: ' "$OUT/notes.log" | cut -c1-120 | tr '\n' ' ')"
say "NOTES TEXT: $(grep -c 'NOTES TEXT' "$OUT/notes.log") lines | $(grep -E 'NOTES TEXT box (open|input)' "$OUT/notes.log" | head -2 | cut -c1-120 | tr '\n' ' ')"
say "failures: notes $(grep -ciE 'fail|error' "$OUT/notes.log"), compositor KWL FAILED $(grep -c 'KWL FAILED' "$OUT/session.log"), VULKAN_ERROR/VULKAN fail $(grep -cE 'VULKAN_ERROR|VULKAN .*fail' "$OUT/session.log" "$OUT/notes.log" | awk -F: '{ s += $2 } END { print s }'), kernel $(grep -ciE 'fail|lost|fault|refused' "$OUT/kernel.log")"
say "shots: n1 $(wc -c < "$OUT/n1.png") bytes, n2 $(wc -c < "$OUT/n2.png") bytes, n3 $(wc -c < "$OUT/n3.png") bytes"
say "done: $OUT"

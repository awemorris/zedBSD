#!/bin/sh
# WS307 p015: the screen lock's time without input on the AAT image (plan/ws307/tests/config-amd64-p015.mk), on the
# Venus guest: the guest is started (its own GUEST_RUNTIME), SSH and the session's KWL READY are waited for, aat's
# input server is started, plan/ws307/tests/p015-lock.py runs its four items (the lock and the sleep default never and
# 16 minutes without a lock or the screen going out, the Settings slider to 1 minute and back to Never and no lock,
# 1 minute locks and the password unlocks, a 2 minutes' sleep puts the screen out), and the guest is stopped.  The judgement is the session's lines and the screenshots over SSH, not the console.
#   plan/ws307/tests/p015-lock.sh IMAGE [OUTDIR]     (default OUTDIR build/ws307-p015-run)
# VENUS_RENDERER names the renderer's install (plan/ws035/tests/zdesktop-guest.sh).
# The last lines: "p015-lock: ITEM pass|fail ..." for each item and "p015-lock: PASS" or "p015-lock: FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
[ $# -ge 1 ] || { echo "usage: p015-lock.sh IMAGE [OUTDIR]" >&2; exit 2; }
image=$(realpath -- "$1")
out=${2:-build/ws307-p015-run}
mkdir -p "$out"
out=$(realpath -- "$out")
summary=$out/summary.txt
: > "$summary"
failed=0
GUEST_RUNTIME=$out/run
AAT_STATE=$out/aat-state
export GUEST_RUNTIME AAT_STATE

note() {
	echo "p015-lock: $*" | tee -a "$summary"
}

# The guest, SSH and the session.
timeout 180 plan/ws035/tests/zdesktop-guest.sh start "$image" > "$out/guest-start.txt" 2>&1
status=$?
note "guest-start status=$status"
[ $status -eq 0 ] || failed=1
if [ $failed -eq 0 ]; then
	timeout 300 python3 plan/tools/guest/guest.py wait > "$out/guest-wait.txt" 2>&1
	status=$?
	note "ssh status=$status"
	if [ $status -ne 0 ]; then
		# The screen the guest stopped at (QMP screendump), for the analysis.
		timeout 30 python3 plan/tools/guest/guest.py screenshot "$out/no-ssh.png" > /dev/null 2>&1
		failed=1
	fi
fi
if [ $failed -eq 0 ]; then
	timeout 240 python3 plan/tools/aat/aat --qemu wait-log 'KWL READY socket=.* role=normal' --timeout 180 \
	    > "$out/ready.txt" 2>&1
	status=$?
	note "session status=$status $(tail -1 "$out/ready.txt")"
	[ $status -eq 0 ] || failed=1
fi
if [ $failed -eq 0 ]; then
	timeout 120 python3 plan/tools/aat/aat --qemu start > "$out/aat-start.txt" 2>&1
	status=$?
	note "aat-start status=$status $(tail -1 "$out/aat-start.txt")"
	[ $status -eq 0 ] || failed=1
fi

# The items (about 25 minutes).
if [ $failed -eq 0 ]; then
	timeout 2400 python3 plan/ws307/tests/p015-lock.py --outdir "$out" -- --qemu > "$out/items.txt" 2>&1
	note "items status=$?"
	if [ -s "$out/verdicts.tsv" ]; then
		# The fields are separated by single TABs and may be empty (no pictures), so awk splits them, not read.
		awk -F '\t' '{ printf "%s %s %s (%s s; %s)\n", $1, $2, $5, $6, $4 }' "$out/verdicts.tsv" | while read -r line; do
			note "$line"
		done
		[ "$(awk -F '\t' '$2 != "pass"' "$out/verdicts.tsv" | wc -l)" -eq 0 ] || failed=1
	else
		note "no verdicts"
		failed=1
	fi
	[ "$(wc -l < "$out/verdicts.tsv" 2>/dev/null || echo 0)" -eq 4 ] || { note "not four items"; failed=1; }
	timeout 60 python3 plan/tools/aat/aat --qemu stop > /dev/null 2>&1
fi

# The guest stops.
timeout 120 plan/ws035/tests/zdesktop-guest.sh stop > "$out/guest-stop.txt" 2>&1
if [ $failed -eq 0 ]; then
	note PASS
else
	note FAIL
fi
exit $failed

#!/bin/sh
# ws099-p001: checks the compositor's criteria (plan/ws099/ws.md, C1..C10) on the Venus guest of the criteria image
# (plan/ws099/tests/build-criteria-image.sh), each test on a guest started afresh.  The thresholds are the variables
# below (the criteria are still the user's draft); the tests read them from the environment.
#
#   plan/ws099/tests/criteria.sh [IMAGE] [OUTDIR] [CRITERIA...]
#     IMAGE     default build/ws099-criteria.img
#     OUTDIR    default build/ws099-criteria (results.txt: one line per test; the tests' outputs and pictures)
#     CRITERIA  default all: C1 C2 C3 C4 C5 C6 C7 C8 C9 C10 (C9 runs the regression list C9_TESTS)
# C6 is the machine's (plan/ws075/tests/hdmi/measure-apps.sh on the 5330): it is listed as not run here.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."

# The thresholds (the criteria's numbers; QEMU's where the criterion's is the machine's).
export C5_WINDOWS=${C5_WINDOWS:-10}
export C5_ROUNDS=${C5_ROUNDS:-3}
export C5_FIRST_FRAME_MS=${C5_FIRST_FRAME_MS:-100}
export C5_GAP_MS=${C5_GAP_MS:-150}
export C7_MIN_CONTRAST=${C7_MIN_CONTRAST:-4.5}
export C10_SECONDS=${C10_SECONDS:-3600}
export C10_MAX_ERRORS=${C10_MAX_ERRORS:-0}
C1_CYCLES=${C1_CYCLES:-2}
# The zdesktop regressions of C9: NAME for plan/ws035/tests/zdesktop-NAME.sh, or a path (plan/ws099/tests/...).
C9_TESTS=${C9_TESTS:-"p052 p053 p076 p126 p128 p134 p137 p138 plan/ws099/tests/cursor-owner.sh"}

image=${1:-build/ws099-criteria.img}
out=${2:-build/ws099-criteria}
[ $# -ge 2 ] && shift 2 || shift $#
criteria=${*:-"C1 C2 C3 C4 C5 C6 C7 C8 C9 C10"}
mkdir -p "$out"
results=$out/results.txt
GUEST_RUNTIME=$(pwd)/build/ws035-sq-run
export GUEST_RUNTIME

# Starts the guest afresh from the image at a size (1280x800 or 1920x1280), and waits for its boot: SIZE NAME.
# BUG-147 (ws099-p026): a start that failed (the copy of the image or the emulator's start not done within the time,
# so no session.json) was not noticed, and the next test ran with no guest ("no guest is running", the first time of
# q609's cursor-owner).  The start's own output goes to OUTDIR/NAME.start.log, SSH must answer after the boot's wait,
# and a failed start is tried once more; returns 1 when the second fails too.
start_guest() {
	attempt=1
	: > "$out/$2.start.log"
	while :; do
		sh plan/ws035/tests/zdesktop-guest.sh stop >/dev/null 2>&1
		if [ "$1" = 1920x1280 ]; then
			VENUS_SIZE=1920x1280 timeout 300 sh plan/ws035/tests/zdesktop-guest.sh start "$image" >> "$out/$2.start.log" 2>&1
		else
			env -u VENUS_SIZE timeout 300 sh plan/ws035/tests/zdesktop-guest.sh start "$image" >> "$out/$2.start.log" 2>&1
		fi
		started=$?
		if [ $started -eq 0 ]; then
			sleep 40
			timeout 200 python3 plan/tools/guest/guest.py wait --timeout 180 >> "$out/$2.start.log" 2>&1 && return 0
		fi
		echo "criteria: guest start $attempt failed (start status $started)" >> "$out/$2.start.log"
		[ $attempt -ge 2 ] && return 1
		attempt=$((attempt + 1))
	done
}

# Runs one test on a fresh guest: CRITERION NAME SIZE COMMAND...; records its verdict, time and RESULT line.
# A guest that does not start fails the test as INFRA without running it.
run() {
	criterion=$1 name=$2 size=$3
	shift 3
	if ! start_guest "$size" "$name"; then
		keep_failure "$name"
		echo "$criterion $name FAIL seconds=0 INFRA: the guest did not start ($out/$name.start.log)" | tee -a "$results"
		return
	fi
	began=$(date +%s)
	if [ "$size" = 1920x1280 ]; then
		VENUS_SIZE=1920x1280 timeout 5400 "$@" > "$out/$name.log" 2>&1
	else
		env -u VENUS_SIZE timeout 5400 "$@" > "$out/$name.log" 2>&1
	fi
	code=$?
	verdict=FAIL
	[ $code -eq 0 ] && verdict=PASS
	[ $code -ne 0 ] && keep_failure "$name"
	detail=$(grep -E 'RESULT|: PASS|: FAIL|status=' "$out/$name.log" | tail -2 | tr '\n' ' ')
	echo "$criterion $name $verdict seconds=$(($(date +%s) - began)) $detail" | tee -a "$results"
}

# Keeps what tells a failed test's cause from an infrastructure failure (ws099-p024, BUG-147), before the next test starts
# the guest afresh: whether QEMU still runs and its own stderr (the emulator's log, not the guest's console), and the
# session's and the test compositor's logs when SSH answers.
keep_failure() {
	alive=no
	[ -f "$GUEST_RUNTIME/session.json" ] && kill -0 "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["pid"])' "$GUEST_RUNTIME/session.json" 2>/dev/null)" 2>/dev/null && alive=yes
	echo "qemu alive=$alive" > "$out/$1.failure.txt"
	cp "$GUEST_RUNTIME/qemu.log" "$out/$1.qemu.log" 2>/dev/null
	timeout 30 python3 plan/tools/guest/guest.py run 'cat /run/user/1000/session.log 2>/dev/null' > "$out/$1.session.log" 2>&1
	timeout 30 python3 plan/tools/guest/guest.py run 'cat /tmp/zdesktop.log 2>/dev/null' > "$out/$1.zdesktop.log" 2>&1
}

: > "$results"
for criterion in $criteria; do
	case $criterion in
	C1)
		# Login and Log Out (black pictures fail the run), then the boot and Shut Down (ws099-p004; starts its own guest).
		# Named c1-p126 so that C9's p126 does not write over its log and its failure files (T1-477).
		run C1 c1-p126 1280x800 sh plan/ws035/tests/zdesktop-p126.sh "$out/c1" "$C1_CYCLES" --no-black
		began=$(date +%s)
		sh plan/ws099/tests/c1-boot-shutdown.sh "$image" "$out/c1-boot" > "$out/c1-boot-shutdown.log" 2>&1
		code=$?
		verdict=FAIL
		[ $code -eq 0 ] && verdict=PASS
		echo "C1 c1-boot-shutdown $verdict seconds=$(($(date +%s) - began)) $(grep -E 'RESULT' "$out/c1-boot-shutdown.log" | tail -1)" | tee -a "$results"
		;;
	C2) run C2 c2-geometry 1920x1280 sh plan/ws099/tests/c2-geometry.sh "$out/c2" ;;
	C3)
		# Esc (p138), and the bottom edge's swipe back from fullscreen (ws099-p015).
		run C3 p138 1280x800 sh plan/ws035/tests/zdesktop-p138.sh "$out/c3"
		run C3 c3-swipe-back 1280x800 sh plan/ws099/tests/c3-swipe-back.sh "$out/c3-swipe-back"
		;;
	C4) run C4 p137 1280x800 sh plan/ws035/tests/zdesktop-p137.sh "$out/c4" ;;
	C5) run C5 c5-transitions 1280x800 sh plan/ws099/tests/c5-transitions.sh "$out/c5" ;;
	C6) echo "C6 measure-apps NOT-RUN the machine's measure (plan/ws075/tests/hdmi/measure-apps.sh on the 5330)" | tee -a "$results" ;;
	C7) run C7 c7-contrast 1280x800 sh plan/ws099/tests/c7-contrast.sh "$out/c7" ;;
	C8) run C8 p134 1280x800 sh plan/ws035/tests/zdesktop-p134.sh "$out/c8" ;;
	C9)
		for test in $C9_TESTS; do
			size=1280x800
			[ "$test" = p128 ] && size=1920x1280
			case $test in
			*/*) name=$(basename "$test" .sh); run C9 "$name" "$size" sh "$test" "$out/c9-$name" ;;
			*) run C9 "$test" "$size" sh "plan/ws035/tests/zdesktop-$test.sh" "$out/c9-$test" ;;
			esac
		done
		;;
	C10) run C10 c10-soak 1280x800 sh plan/ws099/tests/c10-soak.sh "$out/c10" ;;
	*) echo "unknown criterion $criterion" ;;
	esac
done
sh plan/ws035/tests/zdesktop-guest.sh stop >/dev/null 2>&1
echo "criteria done: $results"

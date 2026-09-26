#!/bin/sh
# ws068-p010: the EGL pbuffer on the Venus guest (the zdesktop image, built by
# plan/ws035/tests/build-zdesktop-image.sh, run by plan/ws035/tests/zdesktop-guest.sh start).
# egltest --platform=pbuffer --scene=draw draws scene.c's shapes into a pbuffer nothing shows and
# reads them back (EGLTEST CHECK); 600 frames must end (the frame's stream and descriptors are
# freed at each swap), and a pbuffer larger than the screen works the same.
#
#   plan/ws068/tests/egl-p010.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws068-p010}
mkdir -p "$out"
guest() { timeout 300 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
status=0

# Fails the run unless a log has a line matching a pattern.
expect_log() {
	found=$(guest "grep -cE '$2' $1" | tail -1)
	if [ "${found:-0}" -gt 0 ] 2>/dev/null; then
		echo "log: $2 ok"
	else
		echo "log: $2 MISSING"
		status=1
	fi
}

# 1. 600 frames into a 320x200 pbuffer (no compositor, nothing on the screen).
guest '/bin/egltest --platform=pbuffer --size=320x200 --scene=draw --frames=600 --delay-ms=0 --token=p > /tmp/egl-p.log 2>&1 </dev/null; echo rc=$?' > "$out/pbuffer-rc.txt"
guest 'cat /tmp/egl-p.log' > "$out/pbuffer.txt"
grep -vE "PIXEL.*ok$" "$out/pbuffer.txt"
expect_log /tmp/egl-p.log 'EGLTEST START run=p platform=pbuffer .* size=320x200'
expect_log /tmp/egl-p.log 'EGLTEST CHECK run=p failures=0 glerror=0x0'
expect_log /tmp/egl-p.log 'EGLTEST DONE run=p frames=600 glerror=0x0 failures=0'

# 2. A pbuffer larger than the screen.
guest '/bin/egltest --platform=pbuffer --size=2048x1536 --scene=draw --frames=5 --delay-ms=0 --token=big > /tmp/egl-big.log 2>&1 </dev/null; echo rc=$?' > "$out/big-rc.txt"
guest 'cat /tmp/egl-big.log' > "$out/big.txt"
expect_log /tmp/egl-big.log 'EGLTEST CHECK run=big failures=0 glerror=0x0'
expect_log /tmp/egl-big.log 'EGLTEST DONE run=big frames=5 glerror=0x0 failures=0'

[ $status -eq 0 ] && echo "egl-p010: PASS" || echo "egl-p010: FAIL"
exit $status

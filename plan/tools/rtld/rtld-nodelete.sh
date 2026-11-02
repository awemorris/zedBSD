#!/bin/sh
# Runs rtld-nodelete (rtld-nodelete.c: a NODELETE object stays mapped after its last dlclose(), by DF_1_NODELETE
# and by RTLD_NODELETE) in the running SSH guest.  Builds it with build-nodelete.sh against BUILD's libc.so, sends
# it in one ustar archive, unpacks it with pax in /root/rtld-nodelete and runs it there with LD_LIBRARY_PATH.  The
# guest runs BUILD's image (config-amd64-rtld.mk), so its /lib/ld.so is the loader under test.
# PASS: the last line rtld-nodelete: PASS.  The program printed RTLD-NODELETE: PASS and exited with 0; the plain
# library was finalized at its dlclose(), before the step's line; keep, flag and dep were finalized only after the
# program's last line, their atexit() functions ran then too, and dep after keep.  The output is in
# OUT/rtld-nodelete.txt.
#   plan/tools/guest/test-image.sh plan/tools/rtld/config-amd64-rtld.mk BUILD
#   GUEST_RUNTIME=... python3 plan/tools/guest/guest.py start BUILD/hdd-image.img
#   GUEST_RUNTIME=... plan/tools/rtld/rtld-nodelete.sh [BUILD]     (default build/amd64; OUT is build/rtld-nodelete)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
build=${1:-build/amd64}
out=build/rtld-nodelete
guest() { timeout 300 python3 plan/tools/guest/guest.py "$@" 2>&1 </dev/null; }

# The program and its libraries.
sh plan/tools/rtld/build-nodelete.sh "$build" "$out/files" || { echo "rtld-nodelete: FAIL (build)"; exit 1; }

# Into the guest, in one archive.
guest run 'rm -rf /root/rtld-nodelete /root/rtld-nodelete.tar && mkdir -p /root/rtld-nodelete' >/dev/null
guest put "$out/rtld-nodelete.tar" /root/rtld-nodelete.tar >/dev/null
guest run 'cd /root/rtld-nodelete && pax -r -f /root/rtld-nodelete.tar && ls | wc -l' | tail -1 |
    sed 's/^/rtld-nodelete: files in the guest: /'

# The run.
guest run 'cd /root/rtld-nodelete && chmod +x rtld-nodelete &&
    LD_LIBRARY_PATH=/root/rtld-nodelete /root/rtld-nodelete/rtld-nodelete; echo exit=$?' > "$out/rtld-nodelete.txt"
grep -E '^RTLD-NODELETE|^exit=' "$out/rtld-nodelete.txt"

# The order of the lines: what ran at each dlclose() and what only at exit.
verdict=$(awk '
	/^RTLD-NODELETE fini plain$/ { plain_fini = NR }
	/^RTLD-NODELETE step=plain ok$/ { plain_ok = NR }
	/^RTLD-NODELETE: PASS$/ { pass = NR }
	/^RTLD-NODELETE fini keep$/ { keep_fini = NR }
	/^RTLD-NODELETE fini flag$/ { flag_fini = NR }
	/^RTLD-NODELETE fini dep$/ { dep_fini = NR }
	/^RTLD-NODELETE atexit keep$/ { keep_exit = NR }
	/^RTLD-NODELETE atexit flag$/ { flag_exit = NR }
	/^exit=0$/ { exited = NR }
	END {
		if (!pass) { print "no PASS line"; exit }
		if (!exited) { print "the program did not exit with 0"; exit }
		if (!plain_fini || !plain_ok || plain_fini > plain_ok) { print "plain was not finalized at its dlclose"; exit }
		if (!keep_exit || keep_exit < pass || !flag_exit || flag_exit < pass) { print "an atexit function did not run at exit"; exit }
		if (!keep_fini || keep_fini < pass || !flag_fini || flag_fini < pass) { print "keep or flag was not finalized at exit only"; exit }
		if (!dep_fini || dep_fini < keep_fini) { print "dep was not finalized at exit, after keep"; exit }
		print "ok"
	}' "$out/rtld-nodelete.txt")
if [ "$verdict" = ok ]; then
	echo "rtld-nodelete: PASS"
	exit 0
fi
echo "rtld-nodelete: FAIL ($verdict)"
exit 1

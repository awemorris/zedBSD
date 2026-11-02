#!/bin/sh
# Runs rtld-finalize (rtld-finalize.c: dlclose() runs an unloaded library's atexit() functions and C++ static
# destructors before it unmaps it; a NODELETE library's run at exit) in the running SSH guest.  Builds it with
# build-finalize.sh against BUILD's libc.so, sends it in one ustar archive, unpacks it with pax in
# /root/rtld-finalize and runs it there with LD_LIBRARY_PATH.  The guest runs BUILD's image (config-amd64-rtld.mk),
# so its /lib/ld.so and /lib/libc.so are the ones under test.
# PASS: the last line rtld-finalize: PASS.  The program printed RTLD-FINALIZE: PASS (the plain library's handlers ran
# in each dlclose(), newest first, then its destructor) and exited with 0; after that line exactly these ran, in this
# order: keep's handlers newest first, its C++ static object's last, the program's own atexit() function, then keep's
# destructor; nothing of plain ran at exit.  The output is in OUT/rtld-finalize.txt.
#   plan/tools/guest/test-image.sh plan/tools/rtld/config-amd64-rtld.mk BUILD
#   GUEST_RUNTIME=... python3 plan/tools/guest/guest.py start BUILD/hdd-image.img
#   GUEST_RUNTIME=... plan/tools/rtld/rtld-finalize.sh [BUILD]     (default build/amd64; OUT is build/rtld-finalize)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
build=${1:-build/amd64}
out=build/rtld-finalize
guest() { timeout 300 python3 plan/tools/guest/guest.py "$@" 2>&1 </dev/null; }

# The program and its libraries.
sh plan/tools/rtld/build-finalize.sh "$build" "$out/files" || { echo "rtld-finalize: FAIL (build)"; exit 1; }

# Into the guest, in one archive.
guest run 'rm -rf /root/rtld-finalize /root/rtld-finalize.tar && mkdir -p /root/rtld-finalize' >/dev/null
guest put "$out/rtld-finalize.tar" /root/rtld-finalize.tar >/dev/null
guest run 'cd /root/rtld-finalize && pax -r -f /root/rtld-finalize.tar && ls | wc -l' | tail -1 |
    sed 's/^/rtld-finalize: files in the guest: /'

# The run.
guest run 'cd /root/rtld-finalize && chmod +x rtld-finalize &&
    LD_LIBRARY_PATH=/root/rtld-finalize /root/rtld-finalize/rtld-finalize; echo exit=$?' > "$out/rtld-finalize.txt"
grep -E '^RTLD-FINALIZE|^exit=' "$out/rtld-finalize.txt"

# What ran after the program's last line, in order.
expected='keep cxa-null|keep atexit-tail|keep cxa-handle|keep atexit|keep c++-static|atexit main|keep fini|'
verdict=$(awk -v expected="$expected" '
	/^RTLD-FINALIZE: PASS$/ { pass = NR; next }
	pass && /^RTLD-FINALIZE run / { sub(/^RTLD-FINALIZE run /, ""); after = after $0 "|"; next }
	/^RTLD-FINALIZE run / { early = 1 }
	/^exit=0$/ { exited = NR }
	END {
		if (!pass) { print "no PASS line"; exit }
		if (early) { print "a handler ran before the end outside a step"; exit }
		if (!exited) { print "the program did not exit with 0"; exit }
		if (after != expected) { print "at exit: " after; exit }
		print "ok"
	}' "$out/rtld-finalize.txt")
if [ "$verdict" = ok ]; then
	echo "rtld-finalize: PASS"
	exit 0
fi
echo "rtld-finalize: FAIL ($verdict)"
exit 1

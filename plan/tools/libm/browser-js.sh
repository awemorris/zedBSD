#!/bin/sh
# ws076: runs browser's JavaScript tests (plan/ws074/tests/js) and
# WS076's libm lines (plan/tools/libm/js) in the guest with the new libm and
# compares them with Chromium's output (NAME.expected) through
# plan/ws074/tests/run-js-tests.py and plan/tools/libm/js-reference.py.
#
#   plan/tools/libm/browser-js.sh
#
# BUILD (default build/ws076-browser) is the build directory of the image
# (plan/tools/libm/config-amd64-browser-libm.mk), RUN the emulator's work
# directory (default build/ws076-browser-run).  The outputs go to
# build/ws076-browser-js/NAME.out.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
. plan/tools/guest/jobs.sh  # ZEDBSD_JOBS, the parallel jobs (default 16)
build=${BUILD:-build/ws076-browser}
out=build/ws076-browser-js
mkdir -p "$out"

# The image, with the tests in /root/js.
extra=""
for test in plan/ws074/tests/js/*.js plan/tools/libm/js/*.js; do
	extra="$extra --file /root/js/$(basename "$test")=$(pwd)/$test"
done
# The firmware the configuration selects is fetched after its license is accepted once (ws233-p001); the question
# is answered the way CI answers it (2026-10-14, Q1 for the user).
yes | make CONFIGMK=plan/tools/libm/config-amd64-browser-libm.mk BUILD="$build" firmware-licenses
make -j"$ZEDBSD_JOBS" BUILD_JOBS="$ZEDBSD_JOBS" CONFIGMK=plan/tools/libm/config-amd64-browser-libm.mk \
	BUILD="$build" "ROOTFS_EXTRA_FILES=$extra" disk

# Runs every test, marking where each output and its standard error begin.
command='for f in /root/js/*.js; do n=${f##*/}; echo "@@@ ${n%.js}"; browser --js $f 2>/tmp/js.err; echo "@@@ stderr"; cat /tmp/js.err; done; echo "@@@ end"'
RUN=${RUN:-build/ws076-browser-run} TIMEOUT=${TIMEOUT:-600} \
	sh plan/tools/guest/amd64-serial.sh "$build/hdd-image.img" "$command" > "$out/serial.txt"

# Splits the transcript into NAME.out, turning "Uncaught X: message" on
# the standard error into "Uncaught X" as run-js-tests.py does for the host.
python3 - "$out" <<'EOF'
import sys
out = sys.argv[1]
name = None
part = None
files = {}
for line in open(out + "/serial.txt", errors="replace").read().splitlines():
	line = line.rstrip("\r")
	if line.startswith("@@@ "):
		word = line[4:]
		if word == "end":
			break
		if word == "stderr":
			part = "stderr"
			continue
		name = word
		part = "stdout"
		files[name] = []
		continue
	if name is None:
		continue
	if part == "stderr" and line.startswith("Uncaught "):
		line = "Uncaught " + line[len("Uncaught "):].split(":")[0]
	files[name].append(line)
for name, lines in files.items():
	with open("%s/%s.out" % (out, name), "w") as stream:
		stream.write("\n".join(lines) + ("\n" if lines else ""))
print("browser-js: %d outputs" % len(files))
EOF

# The browser's own tests, then WS076's libm lines, both against Chromium.
status=0
python3 plan/ws074/tests/run-js-tests.py --outputs "$out" || status=1
python3 plan/tools/libm/js-reference.py --outputs "$out" || status=1
exit $status

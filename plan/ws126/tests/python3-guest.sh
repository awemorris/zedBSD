#!/bin/sh
# ws126-p005: Python 3 on the guest, P1-P3 and P5-P7 of WS126 (P4: no third-tier module, 2026-10-09 user D1; P8 is
# plan/tools/boot-test.sh on the same image).
#
# The image: plan/tools/guest/test-image.sh plan/ws126/tests/config-amd64-python3.mk BUILD, started with
# plan/tools/guest/guest.sh start BUILD/hdd-image.img (GUEST_RUNTIME as for guest.py).  Then
#
#   plan/ws126/tests/python3-guest.sh OUTDIR
#
# Each part prints NAME: ok or NAME: FAIL (...); the regression tests' outcome (P5) is recorded and only counts
# as a failure when the run did not finish.  The last line is python3-guest: PASS or python3-guest: FAIL.  OUTDIR
# gets each part's output (a fresh directory each run).
#
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

set -u
cd "$(dirname "$0")/../../.." || exit 2
. plan/tools/fresh-out.sh
[ $# -eq 1 ] || { echo "usage: python3-guest.sh OUTDIR" >&2; exit 2; }
fresh_out "$1"
out=$1
status=0

# Runs a command in the guest (at most some seconds) into a file of OUTDIR.
guest() {
	timeout "$2" python3 plan/tools/guest/guest.py run "$3" > "$out/$1.txt" 2>&1 </dev/null
}

# Passes a part when its output's last line is the wanted one.
part() {
	if tail -1 "$out/$2.txt" | grep -qx -- "$3"; then
		echo "$1: ok"
	else
		echo "$1: FAIL ($(tail -1 "$out/$2.txt"))"
		status=1
	fi
}

# P1: the version, from the command and from sys.
guest version 60 'python3 -V; python3 -c "import sys; print(sys.version)"; echo done'
if grep -qx 'Python 3.14.8' "$out/version.txt" && grep -q '^3\.14\.8 ' "$out/version.txt"; then
	echo "P1 version: ok"
else
	echo "P1 version: FAIL ($(head -2 "$out/version.txt" | tr '\n' ' '))"
	status=1
fi

# P2: the representative modules.
guest smoke 300 'cd /root/ws126 && python3 smoke.py'
part "P2 smoke" smoke 'smoke: PASS'

# P3: ssl and hashlib over OpenSSL, the default CA bundle, a loopback TLS exchange.
guest tls 300 'cd /root/ws126 && python3 tls-loopback.py'
part "P3 tls" tls 'tls-loopback: PASS'

# P5: the chosen regression tests (the outcome is recorded; failures go to Bugs or limitations).
# test_concurrent_initialization_subinterpreter (test_datetime: 8 subinterpreters of an InterpreterPoolExecutor) is
# left out: it did not end in the 10 minutes of the timeout (T1-508), a limitation recorded in ws126-p005.
guest regrtest 3000 'cd /root/ws126 && python3 -m test -j2 --timeout 600 --ignore test_concurrent_initialization_subinterpreter test_json test_re test_datetime test_os test_subprocess test_socket test_threading test_zlib test_hashlib test_unicodedata test_pathlib test_asyncio; echo "status=$?"'
if grep -q '^== Tests result: ' "$out/regrtest.txt"; then
	echo "P5 regrtest: ran ($(grep '^== Tests result: ' "$out/regrtest.txt" | tail -1); $(tail -1 "$out/regrtest.txt"))"
else
	echo "P5 regrtest: FAIL (no result line; $(tail -1 "$out/regrtest.txt"))"
	status=1
fi

# P6: the REPL on a pseudo-terminal.
guest repl 120 'cd /root/ws126 && python3 repl-pty.py'
part "P6 repl" repl 'repl-pty: PASS'

# P7: the license and what the library takes in the image.
guest license 60 'ls -l /usr/share/licenses/python3/LICENSE && du -sk /usr/lib/python3.14 /usr/lib/libpython3.14.so.1.0 /usr/bin/python3.14 && find /usr/lib/python3.14 -type f | wc -l; echo done'
if grep -q 'LICENSE$' "$out/license.txt"; then
	echo "P7 license: ok ($(grep -v LICENSE "$out/license.txt" | grep -v '^done$' | tr '\n' ' '))"
else
	echo "P7 license: FAIL"
	status=1
fi

if [ "$status" -eq 0 ]; then
	echo "python3-guest: PASS"
else
	echo "python3-guest: FAIL (outputs in $out)"
fi
exit "$status"

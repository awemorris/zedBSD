#!/bin/sh
# The host tests of the browser's net/tls.c, which calls libssl-compat and libcrypto-compat directly.  The browser
# and host-loader are built with plan/ws074/tests/host-build.sh (it compiles the two libraries into libbrowser.so
# and the host-loader from their sources).  Then:
#   - libbrowser.so needs no libssl or libcrypto (readelf), exports no SSL_, X509_, ERR_ or TLS_client_method name
#     and imports none (nm), and holds no name of a TLS library to open (strings);
#   - the WS074 runners run-http-tests.py and run-loader-tests.py pass, with the counts of the WS335 p036 baseline
#     (http 15, loader 11), and the dynamic loader's log shows no libssl or libcrypto started.
#   sh plan/ws335/tests/run-nettls.sh
# The servers bind 127.0.0.1 on ports the kernel chose just before.  The browser build goes to
# build/ws335-nettls-host (kept between runs, host-build.sh rebuilds what changed); the rest under
# build/tmp/ws335-nettls (a fresh run directory).  The last line is run-nettls: PASS or run-nettls: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
PATH=/usr/bin:/bin:$PATH
export PATH
python=/usr/bin/python3
. plan/tools/fresh-out.sh
out=build/tmp/ws335-nettls
fresh_out "$out"
host=build/ws335-nettls-host
status=0

# 1. The browser and host-loader for the host.
if ! BROWSER_HOST_BUILD=$host timeout 3000 sh plan/ws074/tests/host-build.sh plain > "$out/host-build.log" 2>&1; then
	tail -20 "$out/host-build.log"
	echo "run-nettls: FAIL (host-build.sh; $out/host-build.log)"
	exit 1
fi
for program in browser host-loader; do
	if [ ! -x "$host/plain/$program" ]; then
		echo "run-nettls: FAIL (no $host/plain/$program)"
		exit 1
	fi
done

# 2. The library names: nothing of OpenSSL's or of the compat shared libraries is needed, imported, exported or opened.
library=$host/plain/libbrowser.so
needed=$(readelf -d "$library" | grep 'NEEDED' | grep -c 'Shared library: \[lib\(ssl\|crypto\)')
imported=$(nm -D --undefined-only "$library" | grep -c -E ' (SSL_|X509_|ERR_|TLS_client_method)')
exported=$(nm -D --defined-only "$library" | grep -c -E ' (SSL_|X509_|ERR_|TLS_client_method)')
named=$(strings "$library" | grep -c -E 'libssl\.so|libcrypto\.so|libssl-compat\.so|libcrypto-compat\.so')
echo "nettls: libbrowser.so: $needed libssl/libcrypto NEEDED, $imported imported and $exported exported TLS names, $named library names in strings"
if [ "$needed" -ne 0 ] || [ "$imported" -ne 0 ] || [ "$exported" -ne 0 ] || [ "$named" -ne 0 ]; then
	echo "nettls: libbrowser.so depends on or names a TLS shared library"
	status=1
fi

# 3. The wrappers: each runs the program with the dynamic loader's log of its libraries in its own file.
mkdir -p "$out/ld"
for program in browser host-loader; do
	{
		echo '#!/bin/sh'
		echo "LD_DEBUG=libs LD_DEBUG_OUTPUT=$top/$out/ld/$program exec $top/$host/plain/$program \"\$@\""
	} > "$out/$program"
	chmod 755 "$out/$program"
done

free_port() {
	"$python" -I -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])'
}

# 4. The runners.
timeout 1200 "$python" plan/ws074/tests/run-http-tests.py --program "$top/$out/browser" \
    --port "$(free_port)" --tls-port "$(free_port)" --tls-wrong-port "$(free_port)" > "$out/http.txt" 2>&1
echo "nettls: run-http-tests $(tail -1 "$out/http.txt")"
timeout 1200 "$python" plan/ws074/tests/run-loader-tests.py --program "$top/$out/host-loader" \
    --port "$(free_port)" --tls-port "$(free_port)" --tls-wrong-port "$(free_port)" > "$out/loader.txt" 2>&1
echo "nettls: run-loader-tests $(tail -1 "$out/loader.txt")"
grep '^FAIL' "$out/http.txt" "$out/loader.txt"
for runner in http loader; do
	total=$(tail -1 "$out/$runner.txt" | sed -n 's/^[a-z-]* \([0-9]*\)\/\([0-9]*\)$/\2/p')
	passed=$(tail -1 "$out/$runner.txt" | sed -n 's/^[a-z-]* \([0-9]*\)\/\([0-9]*\)$/\1/p')
	if [ -z "$total" ] || [ "$passed" != "$total" ]; then
		echo "nettls: $runner tests did not all pass"
		status=1
	fi
	eval "count_$runner=\${total:-0}"
done
if [ "$count_http" -ne 15 ]; then
	echo "nettls: the http count is $count_http, not 15"
	status=1
fi
if [ "$count_loader" -ne 11 ]; then
	echo "nettls: the loader count is $count_loader, not 11"
	status=1
fi

# 5. Which libraries were started (the dynamic loader names each it initializes): never a libssl or libcrypto.
started=$(cat "$out"/ld/* 2> /dev/null | grep -c -E 'calling init: .*/lib(ssl|crypto)')
logged=$(cat "$out"/ld/* 2> /dev/null | grep -c 'calling init:')
echo "nettls: loader logs: $logged libraries started, $started of them libssl or libcrypto"
if [ "$logged" -eq 0 ] || [ "$started" -ne 0 ]; then
	echo "nettls: the libraries started are not the ones meant"
	status=1
fi

if [ $status -eq 0 ]; then
	echo "run-nettls: PASS (http $count_http cases, loader $count_loader cases, libssl-compat and libcrypto-compat linked)"
else
	echo "run-nettls: FAIL"
fi
exit $status

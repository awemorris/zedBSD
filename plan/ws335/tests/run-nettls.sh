#!/bin/sh
# The host tests of the browser's net/tls.c with libssl-compat in place of OpenSSL.  net/tls.c opens libssl-compat.so
# (with its libcrypto-compat.so) when it is there, else OpenSSL's libssl.so or libssl.so.3; the compat libraries are
# built for the host as shared libraries (the host's cc, their exports.map) into one directory under their own names
# only (no link named like OpenSSL's: the compat runs show that net/tls.c opens the compat names itself), and the
# browser and host-loader (plan/ws074/tests/host-build.sh, unchanged) are run
# through a wrapper that sets LD_LIBRARY_PATH to that directory (only the browser gets it: the test server is Python
# with the host's OpenSSL).  The WS074 runners run-http-tests.py and run-loader-tests.py go once with the host's
# OpenSSL and once with compat; both must pass, with the same counts, and the dynamic loader's log must show the
# compat libraries loaded in the compat runs and not in the OpenSSL runs.
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

# 2. The compat libraries as host shared libraries, under the names net/tls.c opens first.
lib=$out/lib
mkdir -p "$lib/include" "$lib/obj"
ln -sfn "$top/include/libc/compat" "$lib/include/compat"
build_library() {
	name=$1
	shift
	objects=
	for file in "$@"; do
		object=$lib/obj/$name-$(basename "$file" .c).o
		if ! timeout 300 cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fPIC -I"$lib/include" -I. \
		    -c "$file" -o "$object" >> "$lib/build.log" 2>&1; then
			return 1
		fi
		objects="$objects $object"
	done
	cc -shared -Wl,-soname,$name.so -Wl,-z,defs -Wl,-z,nodelete -Wl,--version-script=userland/base/$name/exports.map \
	    $objects -L"$lib" $libraries -lpthread -o "$lib/$name.so" >> "$lib/build.log" 2>&1
}
# The sources of each library from its sources.mk (the zedBSD lists: the common files and the POSIX one).
crypto_sources=$(grep -v '^#' userland/base/libcrypto-compat/sources.mk | grep -o 'userland/base/libcrypto-compat/[a-z0-9-]*\.c' | grep -v -- '-win32\.c')
ssl_sources=$(grep -v '^#' userland/base/libssl-compat/sources.mk | grep -o 'userland/base/libssl-compat/[a-z0-9-]*\.c')
: > "$lib/build.log"
libraries=
build_library libcrypto-compat $crypto_sources || { tail -20 "$lib/build.log"; echo "run-nettls: FAIL (libcrypto-compat.so)"; exit 1; }
libraries=-l:libcrypto-compat.so
build_library libssl-compat $ssl_sources || { tail -20 "$lib/build.log"; echo "run-nettls: FAIL (libssl-compat.so)"; exit 1; }
if [ -s "$lib/build.log" ]; then
	head -20 "$lib/build.log"
	echo "run-nettls: FAIL (warnings in the library build; $lib/build.log)"
	exit 1
fi
echo "nettls: host libcrypto-compat.so and libssl-compat.so built ($(echo $crypto_sources | wc -w) and $(echo $ssl_sources | wc -w) sources), no warning"

# 3. The wrappers: each runs the program with the dynamic loader's log of its libraries in its own files.
for flavour in openssl compat; do
	mkdir -p "$out/$flavour/ld"
	for program in browser host-loader; do
		{
			echo '#!/bin/sh'
			[ "$flavour" = compat ] && echo "LD_LIBRARY_PATH=$top/$lib"
			[ "$flavour" = compat ] && echo "export LD_LIBRARY_PATH"
			echo "LD_DEBUG=libs LD_DEBUG_OUTPUT=$top/$out/$flavour/ld/$program exec $top/$host/plain/$program \"\$@\""
		} > "$out/$flavour/$program"
		chmod 755 "$out/$flavour/$program"
	done
done

free_port() {
	"$python" -I -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])'
}

# 4. The runners, with the host's OpenSSL and with compat.
for flavour in openssl compat; do
	timeout 1200 "$python" plan/ws074/tests/run-http-tests.py --program "$top/$out/$flavour/browser" \
	    --port "$(free_port)" --tls-port "$(free_port)" --tls-wrong-port "$(free_port)" > "$out/$flavour/http.txt" 2>&1
	echo "nettls: $flavour: run-http-tests $(tail -1 "$out/$flavour/http.txt")"
	timeout 1200 "$python" plan/ws074/tests/run-loader-tests.py --program "$top/$out/$flavour/host-loader" \
	    --port "$(free_port)" --tls-port "$(free_port)" --tls-wrong-port "$(free_port)" > "$out/$flavour/loader.txt" 2>&1
	echo "nettls: $flavour: run-loader-tests $(tail -1 "$out/$flavour/loader.txt")"
	grep '^FAIL' "$out/$flavour/http.txt" "$out/$flavour/loader.txt"
	for runner in http loader; do
		total=$(tail -1 "$out/$flavour/$runner.txt" | sed -n 's/^[a-z-]* \([0-9]*\)\/\([0-9]*\)$/\2/p')
		passed=$(tail -1 "$out/$flavour/$runner.txt" | sed -n 's/^[a-z-]* \([0-9]*\)\/\([0-9]*\)$/\1/p')
		if [ -z "$total" ] || [ "$passed" != "$total" ]; then
			echo "nettls: $flavour: $runner tests did not all pass"
			status=1
		fi
		eval "count_${flavour}_$runner=\$total"
	done
done
for runner in http loader; do
	eval "a=\${count_openssl_$runner:-}; b=\${count_compat_$runner:-}"
	if [ "$a" != "$b" ]; then
		echo "nettls: the $runner counts differ (OpenSSL $a, compat $b)"
		status=1
	fi
done

# 5. Which libraries were loaded (the dynamic loader names each by the path it opened): in the compat runs libssl
# from the compat directory and never another, in the OpenSSL runs never one from that directory.
compat_loads=$(cat "$out"/compat/ld/* 2> /dev/null | grep -c "calling init: $top/$lib/libssl-compat\.so")
compat_system=$(cat "$out"/compat/ld/* 2> /dev/null | grep 'calling init: .*/libssl[.-]' | grep -vc "calling init: $top/$lib/")
openssl_loads=$(cat "$out"/openssl/ld/* 2> /dev/null | grep -c 'calling init: .*/libssl\.so')
openssl_compat=$(cat "$out"/openssl/ld/* 2> /dev/null | grep -c "$top/$lib/")
echo "nettls: loader logs: compat runs init libssl from $lib $compat_loads times and another libssl $compat_system times; OpenSSL runs init libssl $openssl_loads times, from $lib $openssl_compat times"
if [ "$compat_loads" -eq 0 ] || [ "$compat_system" -ne 0 ] || [ "$openssl_loads" -eq 0 ] || [ "$openssl_compat" -ne 0 ]; then
	echo "nettls: the libraries loaded are not the ones meant"
	status=1
fi

if [ $status -eq 0 ]; then
	echo "run-nettls: PASS (http $count_compat_http cases, loader $count_compat_loader cases, each with OpenSSL and compat)"
else
	echo "run-nettls: FAIL"
fi
exit $status

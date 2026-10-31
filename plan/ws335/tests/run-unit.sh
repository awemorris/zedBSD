#!/bin/sh
# The host unit tests of libcrypto-compat and libssl-compat: each test plan/ws335/tests/unit/test-<name>.c is built
# with every .c of both libraries except the Windows ones (modules not implemented yet are skeletons that answer
# ENOSYS), with ASan and UBSan, and run.  Each test's last line is "test-<name>: PASS (<n> checks)" or
# "test-<name>: FAIL (...)".  The test vectors are read from WS335_VECTORS (default build/ws335-vectors, made by
# fetch.sh); a test that needs a missing file says FAIL, not PASS.
#   sh plan/ws335/tests/run-unit.sh [--win32] <name>...   one or more tests (ct-<name> runs unit/ct-<name>.c too)
#   sh plan/ws335/tests/run-unit.sh [--win32] all         every test present
# Without --win32 (or with --posix) the tests are built with the host's cc as above and run on the host.
# With --win32 they are built for x86_64 Windows the way a Windows program uses the libraries: every .c except
# the POSIX ones is compiled once with llvm-mingw (LLVM_MINGW, default
# $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64; -std=c89, warnings as errors, the Windows build's
# -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE, UBSan in trap mode) into the static libraries
# libcrypto-compat.a and libssl-compat.a, each test is linked with them and -lbcrypt -lcrypt32 -lws2_32 into one
# .exe, and run under Wine (a prefix of its own, build/tmp/ws335-wine, made on first use and marked ready only
# when wineboot succeeded; winedbg is disabled so a trap ends the test; WS335_VECTORS is given as its Z: path).
# Each .exe is built and run in a directory of its own ($out/<test>.run/, its current directory too), so a file
# a test puts beside its .exe (cert.pem) is seen by that test only.  The prefix is kept between runs: a test that
# adds certificates to its stores removes them before it ends and first removes any left by an earlier run.
# A test may test _WIN32 for its Windows-only checks.  Threads in a Windows test are CreateThread or
# _beginthreadex (no pthread library is linked).  The aarch64 Windows build is only compiled and linked
# (check-skeleton.sh); there is no arm64 Wine.
# Last line: run-unit: PASS or run-unit: FAIL.  Output under build/tmp/ws335-unit (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
. plan/tools/fresh-out.sh
out=build/tmp/ws335-unit
fresh_out "$out"
mkdir -p "$out/include"
ln -sfn "$(pwd)/include/libc/compat" "$out/include/compat"
WS335_VECTORS=${WS335_VECTORS:-$(pwd)/build/ws335-vectors}
case "$WS335_VECTORS" in
/*) ;;
*) WS335_VECTORS=$(pwd)/$WS335_VECTORS ;;
esac
export WS335_VECTORS

system=posix
case "${1:-}" in
--posix) shift ;;
--win32) system=win32; shift ;;
esac

names="$*"
if [ "$names" = "all" ]; then
	names=""
	for file in plan/ws335/tests/unit/test-*.c plan/ws335/tests/unit/ct-*.c; do
		[ -f "$file" ] || continue
		name=$(basename "$file" .c)
		case "$name" in
		test-*) names="$names ${name#test-}" ;;
		*) names="$names $name" ;;
		esac
	done
fi
if [ -z "$names" ]; then
	echo "usage: sh plan/ws335/tests/run-unit.sh [--win32] <name>... | all"
	exit 2
fi

modules=""
for file in userland/base/libcrypto-compat/*.c userland/base/libssl-compat/*.c; do
	case "$file" in
	*-win32.c) ;;
	*) modules="$modules $file" ;;
	esac
done

status=0
if [ "$system" = win32 ]; then
	LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
	wcc=$LLVM_MINGW/bin/x86_64-w64-mingw32-clang
	wflags="-std=c89 -O1 -g -Wall -Wextra -Werror -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE
		-fsanitize=undefined -fsanitize-trap=undefined -I$out/include -I. -Iplan/ws335/tests/unit"
	wine_prefix=$top/build/tmp/ws335-wine
	if [ ! -x "$wcc" ] || ! command -v wine > /dev/null 2>&1; then
		echo "run-unit: FAIL (--win32 needs $wcc and wine)"
		exit 1
	fi
	# The two static libraries, once for every test.
	mkdir -p "$out/lib"
	for file in userland/base/libcrypto-compat/*.c userland/base/libssl-compat/*.c; do
		case "$file" in
		*-posix.c) continue ;;
		userland/base/libcrypto-compat/*) object=$out/lib/crypto-$(basename "$file" .c).o ;;
		*) object=$out/lib/ssl-$(basename "$file" .c).o ;;
		esac
		if ! $wcc $wflags -c "$file" -o "$object" > "$out/lib/build.log" 2>&1 || [ -s "$out/lib/build.log" ]; then
			echo "$file: FAIL (Windows build)"
			head -20 "$out/lib/build.log"
			echo "run-unit: FAIL"
			exit 1
		fi
	done
	"$LLVM_MINGW/bin/llvm-ar" rcs "$out/lib/libcrypto-compat.a" "$out"/lib/crypto-*.o
	"$LLVM_MINGW/bin/llvm-ar" rcs "$out/lib/libssl-compat.a" "$out"/lib/ssl-*.o
	if [ ! -f "$wine_prefix/.ws335-ready" ]; then
		mkdir -p "$wine_prefix"
		if WINEPREFIX=$wine_prefix WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=' timeout 600 wineboot -i \
		    > "$out/wineboot.log" 2>&1; then
			: > "$wine_prefix/.ws335-ready"
		else
			echo "run-unit: FAIL (wineboot; log $out/wineboot.log)"
			exit 1
		fi
	fi
fi
for name in $names; do
	case "$name" in
	ct-*) source=plan/ws335/tests/unit/$name.c; name=${name#ct-}; label=ct-$name ;;
	*) source=plan/ws335/tests/unit/test-$name.c; label=test-$name ;;
	esac
	if [ ! -f "$source" ]; then
		echo "$label: FAIL (no $source)"
		status=1
		continue
	fi
	if [ "$system" = win32 ]; then
		mkdir -p "$out/$label.run"
		program=$out/$label.run/$label.exe
		if ! $wcc $wflags "$source" -L"$out/lib" -lssl-compat -lcrypto-compat -lbcrypt -lcrypt32 -lws2_32 \
		    -o "$program" > "$out/$label.build.log" 2>&1 || [ -s "$out/$label.build.log" ]; then
			echo "$label (win32): FAIL (build)"
			head -20 "$out/$label.build.log"
			status=1
			continue
		fi
		(cd "$out/$label.run" && WINEPREFIX=$wine_prefix WINEDEBUG=-all WINEDLLOVERRIDES=winedbg.exe=d \
		    WS335_VECTORS="Z:$WS335_VECTORS" timeout 600 wine "$top/$program") > "$out/$label.log" 2>&1
		code=$?
		last=$(tr -d '\r' < "$out/$label.log" | tail -1)
		case "$code:$last" in
		"0:$label: PASS"*) echo "$last (win32)" ;;
		*)
			echo "$label (win32): FAIL (exit $code; log $out/$label.log)"
			tr -d '\r' < "$out/$label.log" | grep -v '^ok ' | tail -20
			status=1 ;;
		esac
		continue
	fi
	if ! cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
	    -fno-sanitize-recover=all -fno-omit-frame-pointer -I"$out/include" -I. -Iplan/ws335/tests/unit \
	    "$source" $modules -lpthread -o "$out/$label" > "$out/$label.build.log" 2>&1; then
		echo "$label: FAIL (build)"
		head -20 "$out/$label.build.log"
		status=1
		continue
	fi
	if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	    timeout 600 "$out/$label" > "$out/$label.log" 2>&1; then
		tail -1 "$out/$label.log"
	else
		echo "$label: FAIL (exit; log $out/$label.log)"
		grep -v '^ok ' "$out/$label.log" | head -20
		status=1
	fi
done

if [ "$status" = 0 ]; then echo "run-unit: PASS"; else echo "run-unit: FAIL"; fi
exit "$status"

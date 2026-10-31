#!/bin/sh
# The shape checks of libcrypto-compat and libssl-compat:
#  1. every internal header (cryc*.h, sslc*.h) and every public header (include/libc/compat/crypto/*.h,
#     include/libc/compat/ssl/*.h) compiles alone, included twice, with cc and clang,
#     -std=c89 -Wall -Wextra -Werror -Wpedantic;
#  2. every .c of both libraries except *-win32.c compiles with cc and clang (same flags);
#  3. every .c except *-win32.c compiles against the zedBSD C library headers with the shared clang for
#     x86_64-unknown-zedbsd and aarch64-unknown-zedbsd (syntax only), when build/llvm exists;
#     every .c except *-posix.c compiles with llvm-mingw for x86_64 and aarch64 Windows (same flags), when
#     LLVM_MINGW (default ~/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64) exists;
#  4. layering: no libcrypto-compat file names an sslc header or an sslc_ symbol; no file outside *-win32.c
#     and *-posix.c tests _WIN32 or includes an operating-system header other than the C library's;
#  5. with BASE=<commit>, the headers are unchanged since that commit (an implementation Phase must not change
#     them; a header fault goes back to Q1).
#   sh plan/ws335/tests/check-skeleton.sh
#   BASE=<sha> sh plan/ws335/tests/check-skeleton.sh
# Last line: check-skeleton: PASS or check-skeleton: FAIL.  Output under build/tmp/ws335-skeleton (a fresh run).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws335-skeleton
fresh_out "$out"
mkdir -p "$out/include"
ln -sfn "$(pwd)/include/libc/compat" "$out/include/compat"
flags="-std=c89 -Wall -Wextra -Werror -Wpedantic -D_DEFAULT_SOURCE -I$out/include -I."
crypto=userland/base/libcrypto-compat
ssl=userland/base/libssl-compat
headers="$crypto/cryc*.h $ssl/sslc*.h include/libc/compat/crypto/*.h include/libc/compat/ssl/*.h"
status=0

for compiler in cc clang; do
	if ! command -v "$compiler" > /dev/null 2>&1; then
		echo "$compiler: skipped (not installed)"
		continue
	fi
	for header in $headers; do
		case "$header" in
		include/libc/*) name="<${header#include/libc/}>" ;;
		*) name="\"$header\"" ;;
		esac
		printf '#include %s\n#include %s\nint skeleton_header_check;\n' "$name" "$name" > "$out/one.c"
		if ! $compiler $flags -fsyntax-only "$out/one.c" > "$out/header.log" 2>&1; then
			echo "FAILED $compiler $header"
			head -10 "$out/header.log"
			status=1
		fi
	done
	for source in $crypto/*.c $ssl/*.c; do
		case "$source" in
		*-win32.c) continue ;;
		esac
		if ! $compiler $flags -c "$source" -o "$out/$compiler-$(basename "$source" .c).o" > "$out/source.log" 2>&1; then
			echo "FAILED $compiler $source"
			head -10 "$out/source.log"
			status=1
		fi
	done
done

zclang=build/llvm/bin/clang
if [ -x "$zclang" ]; then
	for target in x86_64-unknown-zedbsd aarch64-unknown-zedbsd; do
		for source in $crypto/*.c $ssl/*.c; do
			case "$source" in
			*-win32.c) continue ;;
			esac
			if ! "$zclang" --target=$target -nostdinc -I. -Iinclude -isystem include/libc -std=c89 -Wall -Wextra \
			    -Werror -Wpedantic -fsyntax-only "$source" > "$out/zedbsd.log" 2>&1; then
				echo "FAILED $target $source"
				head -10 "$out/zedbsd.log"
				status=1
			fi
		done
	done
else
	echo "zedBSD clang: skipped (no $zclang)"
fi

mingw=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
if [ -x "$mingw/bin/x86_64-w64-mingw32-clang" ]; then
	for target in x86_64 aarch64; do
		for source in $crypto/*.c $ssl/*.c; do
			case "$source" in
			*-posix.c) continue ;;
			esac
			if ! "$mingw/bin/$target-w64-mingw32-clang" $flags -D_WIN32_WINNT=0x0A00 -c "$source" \
			    -o "$out/$target-$(basename "$source" .c).o" > "$out/mingw.log" 2>&1; then
				echo "FAILED $target-w64-mingw32 $source"
				head -10 "$out/mingw.log"
				status=1
			fi
		done
	done
else
	echo "llvm-mingw: skipped (not at $mingw)"
fi

if grep -n -E 'sslc[-_]|SSLC_|libssl-compat/' $crypto/*.h $crypto/*.c | grep -v -E '^[^:]*:[0-9]+: \*|^[^:]*:[0-9]+:/\*'; then
	echo "FAILED libcrypto-compat names libssl-compat (above)"
	status=1
fi
for file in $crypto/*.c $crypto/*.h $ssl/*.c $ssl/*.h; do
	case "$file" in
	*-win32.c|*-posix.c) continue ;;
	esac
	if grep -n -E '^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef|elif).*_WIN32|#include <(windows|winsock2|bcrypt|wincrypt|unistd|pthread|fcntl|sys/[a-z]+)\.h>' "$file"; then
		echo "FAILED $file tests the platform or includes an OS header (above)"
		status=1
	fi
done

if [ -n "${BASE:-}" ]; then
	if ! git diff --quiet "$BASE" -- $headers; then
		echo "FAILED headers changed since $BASE:"
		git diff --stat "$BASE" -- $headers
		status=1
	fi
fi

if [ "$status" = 0 ]; then echo "check-skeleton: PASS"; else echo "check-skeleton: FAIL"; fi
exit "$status"

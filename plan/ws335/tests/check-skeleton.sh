#!/bin/sh
# The shape checks of libcrypto-compat and libssl-compat:
#  1. every internal header (cryc*.h, sslc*.h) and every public header (include/libc/compat/crypto/*.h,
#     include/libc/compat/ssl/*.h) compiles alone, included twice, with cc and clang,
#     -std=c89 -Wall -Wextra -Werror -Wpedantic;
#  2. every .c of both libraries except *-win32.c compiles with cc and clang (same flags);
#  3. every .c except *-win32.c compiles against the zedBSD C library headers with the shared clang for
#     x86_64-unknown-zedbsd and aarch64-unknown-zedbsd (syntax only), when build/llvm exists;
#     every .c except *-posix.c compiles with llvm-mingw for x86_64 and aarch64 Windows (same flags, and the
#     Windows build's -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE), when LLVM_MINGW (default
#     ~/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64) exists;
#  4. layering: no libcrypto-compat file names an sslc header or an sslc_ symbol; no file outside *-win32.c
#     and *-posix.c tests _WIN32 or includes an operating-system header other than the C library's;
#  5. with BASE=<commit>, the headers and the test tools are unchanged since that commit, none added or removed
#     (an implementation Phase must not change them; a fault goes back to Q1);
#  6. Windows, per arch: the objects make the two static libraries libcrypto-compat.a and libssl-compat.a, and a
#     program links every object of both (--whole-archive, then -lbcrypt -lcrypt32 -lws2_32) with no message;
#     every .c except *-posix.c also compiles for x86_64 Windows as a Windows program's build does (-std=gnu11 -O2,
#     without the version and UNICODE macros), warnings as errors;
#  7. the namespace: every external name an object defines (nm, for the cc objects and each Windows arch's) is a
#     public name or starts with cryc_ (libcrypto-compat: cryc_, ERR_, X509_, EVP_, RAND_, OPENSSL_, CRYPTO_,
#     d2i_X509) or sslc_ (libssl-compat: sslc_, SSL_, TLS_); the Windows static libraries share the namespace of
#     the executable they are linked into.  Names that are not C identifiers or start with __ (the compiler's) are
#     not checked.
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
crypto_names='^(cryc_|ERR_|X509_|EVP_|RAND_|OPENSSL_|CRYPTO_|d2i_X509$)'
ssl_names='^(sslc_|SSL_|TLS_)'

# The library a source belongs to: crypto or ssl.
library_of() {
	case "$1" in
	$crypto/*) echo crypto ;;
	*) echo ssl ;;
	esac
}

# <label> <nm> <directory> <prefix>: step 7 for the objects <directory>/<prefix>crypto-*.o and ...ssl-*.o.
# nm must succeed on objects that exist, and must show the library's own names (cryc_ and ERR_, sslc_ and SSL_),
# so that a missing tool or an empty list cannot pass.
check_names() {
	for library in crypto ssl; do
		if [ "$library" = crypto ]; then
			allowed=$crypto_names
			wanted='^cryc_ ^ERR_'
		else
			allowed=$ssl_names
			wanted='^sslc_ ^SSL_'
		fi
		objects=$(ls "$3"/"$4"$library-*.o 2> /dev/null)
		if [ -z "$objects" ]; then
			echo "FAILED $1: no lib$library-compat objects to check"
			status=1
			continue
		fi
		if ! "$2" -g --defined-only $objects > "$out/names.txt" 2> "$out/names.log"; then
			echo "FAILED $1: $2 failed on lib$library-compat"
			head -5 "$out/names.log"
			status=1
			continue
		fi
		awk 'NF >= 3 { print $3 } NF == 2 { print $2 }' "$out/names.txt" |
		    grep -E '^[A-Za-z_][A-Za-z0-9_]*$' | grep -v '^__' | sort -u > "$out/names.list"
		for pattern in $wanted; do
			if ! grep -q -E "$pattern" "$out/names.list"; then
				echo "FAILED $1: lib$library-compat shows no name matching $pattern (nm output not understood)"
				status=1
			fi
		done
		bad=$(grep -v -E "$allowed" "$out/names.list")
		if [ -n "$bad" ]; then
			echo "FAILED $1: lib$library-compat defines names outside its namespace:"
			echo "$bad" | head -20
			status=1
		fi
	done
}

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
		if ! $compiler $flags -c "$source" -o "$out/$compiler-$(library_of "$source")-$(basename "$source" .c).o" \
		    > "$out/source.log" 2>&1; then
			echo "FAILED $compiler $source"
			head -10 "$out/source.log"
			status=1
		fi
	done
done
if command -v cc > /dev/null 2>&1; then
	check_names "cc" nm "$out" cc-
fi

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
		wcc=$mingw/bin/$target-w64-mingw32-clang
		dir=$out/$target
		mkdir -p "$dir"
		for source in $crypto/*.c $ssl/*.c; do
			case "$source" in
			*-posix.c) continue ;;
			esac
			if ! "$wcc" $flags -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -c "$source" \
			    -o "$dir/$(library_of "$source")-$(basename "$source" .c).o" > "$out/mingw.log" 2>&1; then
				echo "FAILED $target-w64-mingw32 $source"
				head -10 "$out/mingw.log"
				status=1
			fi
		done
		"$mingw/bin/llvm-ar" rcs "$dir/libcrypto-compat.a" "$dir"/crypto-*.o
		"$mingw/bin/llvm-ar" rcs "$dir/libssl-compat.a" "$dir"/ssl-*.o
		printf 'int main(void)\n{\n\treturn 0;\n}\n' > "$dir/main.c"
		if ! "$wcc" "$dir/main.c" -L"$dir" -Wl,--whole-archive -lssl-compat -lcrypto-compat -Wl,--no-whole-archive \
		    -lbcrypt -lcrypt32 -lws2_32 -o "$dir/link.exe" > "$dir/link.log" 2>&1 || [ -s "$dir/link.log" ]; then
			echo "FAILED $target-w64-mingw32 static link of both libraries"
			head -20 "$dir/link.log"
			status=1
		fi
		check_names "$target-w64-mingw32" "$mingw/bin/llvm-nm" "$dir" ""
		if [ "$target" = x86_64 ]; then
			for source in $crypto/*.c $ssl/*.c; do
				case "$source" in
				*-posix.c) continue ;;
				esac
				if ! "$wcc" -std=gnu11 -O2 -Wall -Wextra -Werror -I"$out/include" -I. -c "$source" \
				    -o "$dir/gnu11.o" > "$out/mingw.log" 2>&1; then
					echo "FAILED $target-w64-mingw32 -std=gnu11 -O2 $source"
					head -10 "$out/mingw.log"
					status=1
				fi
			done
		fi
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
	frozen="userland/base/libcrypto-compat/*.h userland/base/libssl-compat/*.h include/libc/compat/crypto
include/libc/compat/ssl plan/ws335/tests/check-skeleton.sh plan/ws335/tests/run-unit.sh plan/ws335/tests/run-ct.sh
plan/ws335/tests/fetch.sh plan/ws335/tests/vectors.py plan/ws335/tests/unit/check.h
plan/ws335/tests/unit/vecfile.h plan/ws335/tests/unit/derwriter.h plan/ws335/tests/unit/ct.h"
	# Quoted pathspecs: git expands them, so a deleted or added file is seen as well as a changed one.
	set -f
	if ! git diff --quiet "$BASE" -- $frozen; then
		echo "FAILED headers or test tools changed since $BASE:"
		git diff --stat "$BASE" -- $frozen
		status=1
	fi
	set +f
fi

if [ "$status" = 0 ]; then echo "check-skeleton: PASS"; else echo "check-skeleton: FAIL"; fi
exit "$status"

#!/bin/sh
# The export check of libcrypto-compat.so and libssl-compat.so, each named by its file name:
#   libcrypto-compat.so  the names it defines are exactly the names the public headers declare
#                        (include/libc/compat/crypto/*.h) plus the library's own cryc_ names, and none of
#                        libssl-compat's (SSL_, TLS_, sslc_).
#   libssl-compat.so     the names it defines are exactly the names include/libc/compat/ssl/ssl.h declares (no
#                        sslc_ and no cryc_ name), and every cryc_ name it calls is defined by libcrypto-compat.so
#                        when that library is given too.
# Neither library carries a symbol version definition (tools/build/check-dynamic-elf.py takes none in a shared
# library, so the maps have no version node), and every name is a GLOBAL one.
#   sh plan/ws335/tests/check-exports.sh build/p039/dynamic/libcrypto-compat.so
#   sh plan/ws335/tests/check-exports.sh build/p033/dynamic/libcrypto-compat.so build/p033/dynamic/libssl-compat.so
# Needs llvm-nm and llvm-readelf (build/llvm/bin, or on PATH).
# Last line: check-exports: PASS or check-exports: FAIL.  Output under build/tmp/ws335-exports (a fresh run).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh

usage() {
	echo "usage: sh plan/ws335/tests/check-exports.sh <libcrypto-compat.so> [<libssl-compat.so>]"
	echo "check-exports: FAIL"
	exit 1
}
[ $# -ge 1 ] && [ $# -le 2 ] || usage
crypto_library=
ssl_library=
for library in "$@"; do
	[ -f "$library" ] || usage
	case "$(basename -- "$library")" in
	libcrypto-compat.so) crypto_library=$library ;;
	libssl-compat.so) ssl_library=$library ;;
	*) usage ;;
	esac
done
nm=build/llvm/bin/llvm-nm
readelf=build/llvm/bin/llvm-readelf
[ -x "$nm" ] || nm=llvm-nm
[ -x "$readelf" ] || readelf=llvm-readelf
out=build/tmp/ws335-exports
fresh_out "$out"
status=0

# The functions some public headers declare: comments and preprocessor lines (with their continuation lines) are
# dropped, the rest is cut at each semicolon, and the name before the first parenthesis of a statement is the
# function.
public_names() {
	for header in "$@"; do
		awk '
		BEGIN { RS = "\001" }
		{
			text = $0
			while ((start = index(text, "/*")) > 0) {
				rest = substr(text, start + 2)
				stop = index(rest, "*/")
				if (stop == 0) {
					text = substr(text, 1, start - 1)
					break
				}
				text = substr(text, 1, start - 1) " " substr(rest, stop + 2)
			}
			count = split(text, lines, "\n")
			joined = ""
			directive = 0
			for (i = 1; i <= count; i++) {
				if (directive || lines[i] ~ /^[ \t]*#/) {
					directive = lines[i] ~ /\\[ \t]*$/
					continue
				}
				joined = joined " " lines[i]
			}
			count = split(joined, statements, ";")
			for (i = 1; i <= count; i++) {
				statement = statements[i]
				if (statement ~ /typedef/) {
					continue
				}
				if (match(statement, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
					name = substr(statement, RSTART, RLENGTH)
					sub(/[ \t]*\($/, "", name)
					print name
				}
			}
		}' "$header"
	done | sort -u
}

# The checks every library gets: the names it defines into $dir/defined.txt; no version definition, no versioned
# name, every defined dynamic symbol GLOBAL.
common_checks() {
	library=$1
	dir=$2
	if ! "$nm" -D --defined-only "$library" > "$dir/nm.txt" 2> "$dir/nm.log"; then
		echo "FAILED $nm could not read $library"
		head -5 "$dir/nm.log"
		status=1
		return 1
	fi
	awk 'NF >= 3 { print $3 }' "$dir/nm.txt" | sed 's/@.*$//' | sort -u > "$dir/defined.txt"
	if "$readelf" -S "$library" | grep -q '\.gnu\.version_d'; then
		echo "FAILED $library has symbol version definitions"
		status=1
	fi
	if grep -q '@' "$dir/nm.txt"; then
		echo "FAILED $library has versioned names"
		status=1
	fi
	if ! "$readelf" --dyn-syms "$library" > "$dir/dynsyms.txt" 2> /dev/null; then
		echo "FAILED $readelf could not read the dynamic symbols of $library"
		status=1
	else
		hidden=$(awk '$7 != "UND" && $7 != "Ndx" && $5 != "GLOBAL" && NF >= 8 { print $8 }' "$dir/dynsyms.txt")
		if [ -n "$hidden" ]; then
			echo "FAILED defined dynamic symbols of $library that are not GLOBAL:"
			echo "$hidden"
			status=1
		fi
	fi
	return 0
}

# The public list and the defined list must agree exactly ($1 the public list, $2 the defined names that are not the
# library's own, $3 the library).
compare_public() {
	if [ ! -s "$1" ]; then
		echo "FAILED the public list of $3 is empty"
		status=1
	fi
	missing=$(comm -23 "$1" "$2")
	if [ -n "$missing" ]; then
		echo "FAILED public names $3 does not define:"
		echo "$missing"
		status=1
	fi
	extra=$(comm -13 "$1" "$2")
	if [ -n "$extra" ]; then
		echo "FAILED names $3 defines that no public header declares and that are not its own:"
		echo "$extra"
		status=1
	fi
}

# libcrypto-compat.so: the public names and the cryc_ names, nothing of libssl-compat.
if [ -n "$crypto_library" ]; then
	dir=$out/crypto
	mkdir -p "$dir"
	public_names include/libc/compat/crypto/*.h > "$dir/public.txt"
	if common_checks "$crypto_library" "$dir"; then
		grep '^cryc_' "$dir/defined.txt" > "$dir/private.txt"
		grep -v '^cryc_' "$dir/defined.txt" > "$dir/other.txt"
		if [ ! -s "$dir/private.txt" ]; then
			echo "FAILED $crypto_library defines no cryc_ name"
			status=1
		fi
		compare_public "$dir/public.txt" "$dir/other.txt" "$crypto_library"
		if grep -E '^(SSL_|TLS_|sslc_)' "$dir/defined.txt" > "$dir/ssl.txt"; then
			echo "FAILED $crypto_library defines names of libssl-compat:"
			cat "$dir/ssl.txt"
			status=1
		fi
		echo "check-exports: libcrypto-compat.so public $(wc -l < "$dir/public.txt" | tr -d ' '), cryc_ $(wc -l < "$dir/private.txt" | tr -d ' '), defined $(wc -l < "$dir/defined.txt" | tr -d ' ')"
	fi
fi

# libssl-compat.so: exactly the names of ssl.h; the cryc_ names it calls are libcrypto-compat's.
if [ -n "$ssl_library" ]; then
	dir=$out/ssl
	mkdir -p "$dir"
	public_names include/libc/compat/ssl/ssl.h > "$dir/public.txt"
	if common_checks "$ssl_library" "$dir"; then
		compare_public "$dir/public.txt" "$dir/defined.txt" "$ssl_library"
		"$nm" -D --undefined-only "$ssl_library" | awk '{ print $NF }' | sed 's/@.*$//' | sort -u > "$dir/undefined.txt"
		grep '^cryc_' "$dir/undefined.txt" > "$dir/calls.txt"
		if [ ! -s "$dir/calls.txt" ]; then
			echo "FAILED $ssl_library calls no cryc_ name (it is built on libcrypto-compat)"
			status=1
		fi
		if [ -n "$crypto_library" ] && [ -s "$out/crypto/defined.txt" ]; then
			unresolved=$(grep -E '^(cryc_|[A-Z])' "$dir/undefined.txt" | comm -23 - "$out/crypto/defined.txt")
			if [ -n "$unresolved" ]; then
				echo "FAILED names $ssl_library takes from libcrypto-compat that it does not define:"
				echo "$unresolved"
				status=1
			fi
		fi
		echo "check-exports: libssl-compat.so public $(wc -l < "$dir/public.txt" | tr -d ' '), defined $(wc -l < "$dir/defined.txt" | tr -d ' '), cryc_ calls $(wc -l < "$dir/calls.txt" | tr -d ' ')"
	fi
fi

if [ $status -eq 0 ]; then
	echo "check-exports: PASS"
else
	echo "check-exports: FAIL"
fi
exit $status

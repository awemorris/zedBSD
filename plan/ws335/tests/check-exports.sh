#!/bin/sh
# The export check of libcrypto-compat.so: the names the shared library defines are exactly the names the public
# headers declare (include/libc/compat/crypto/*.h) plus the library's own cryc_ names, and the library carries no
# symbol version definition (tools/build/check-dynamic-elf.py takes none in a shared library, so the map has no
# version node).
#   sh plan/ws335/tests/check-exports.sh build/p039/dynamic/libcrypto-compat.so
# Needs llvm-nm and llvm-readelf (build/llvm/bin, or on PATH).
# Last line: check-exports: PASS or check-exports: FAIL.  Output under build/tmp/ws335-exports (a fresh run).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh

if [ $# -ne 1 ] || [ ! -f "$1" ]; then
	echo "usage: sh plan/ws335/tests/check-exports.sh <libcrypto-compat.so>"
	echo "check-exports: FAIL"
	exit 1
fi
library=$1
nm=build/llvm/bin/llvm-nm
readelf=build/llvm/bin/llvm-readelf
[ -x "$nm" ] || nm=llvm-nm
[ -x "$readelf" ] || readelf=llvm-readelf
out=build/tmp/ws335-exports
fresh_out "$out"
status=0

# The functions the public headers declare: comments and preprocessor lines are dropped, the rest is cut at each
# semicolon, and the name before the first parenthesis of a statement is the function.
for header in include/libc/compat/crypto/*.h; do
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
		for (i = 1; i <= count; i++) {
			if (lines[i] ~ /^[ \t]*#/) {
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
done | sort -u > "$out/public.txt"

if ! "$nm" -D --defined-only "$library" > "$out/nm.txt" 2> "$out/nm.log"; then
	echo "FAILED $nm could not read $library"
	head -5 "$out/nm.log"
	echo "check-exports: FAIL"
	exit 1
fi
awk 'NF >= 3 { print $3 }' "$out/nm.txt" | sed 's/@.*$//' | sort -u > "$out/defined.txt"
grep '^cryc_' "$out/defined.txt" > "$out/private.txt"
grep -v '^cryc_' "$out/defined.txt" > "$out/other.txt"

# The lists must not be empty, so that a missing tool or an unreadable header cannot pass.
for list in public private; do
	if [ ! -s "$out/$list.txt" ]; then
		echo "FAILED the $list list is empty"
		status=1
	fi
done

# 1. Every public name is defined, and nothing but public names and cryc_ names is.
missing=$(comm -23 "$out/public.txt" "$out/other.txt")
if [ -n "$missing" ]; then
	echo "FAILED public names the library does not define:"
	echo "$missing"
	status=1
fi
extra=$(comm -13 "$out/public.txt" "$out/other.txt")
if [ -n "$extra" ]; then
	echo "FAILED names the library defines that no public header declares and that are not cryc_ names:"
	echo "$extra"
	status=1
fi

# 2. No SSL names (they belong to libssl-compat) and no symbol version definition.
if grep -E '^(SSL_|TLS_|sslc_)' "$out/defined.txt" > "$out/ssl.txt"; then
	echo "FAILED the library defines names of libssl-compat:"
	cat "$out/ssl.txt"
	status=1
fi
if "$readelf" -S "$library" | grep -q '\.gnu\.version_d'; then
	echo "FAILED the library has symbol version definitions"
	status=1
fi
if grep -q '@' "$out/nm.txt"; then
	echo "FAILED the library has versioned names"
	status=1
fi

# 3. Everything is exported as a global function or object with default visibility (no weak or hidden entry).
if ! "$readelf" --dyn-syms "$library" > "$out/dynsyms.txt" 2> /dev/null; then
	echo "FAILED $readelf could not read the dynamic symbols"
	status=1
else
	hidden=$(awk '$7 != "UND" && $7 != "Ndx" && $5 != "GLOBAL" && NF >= 8 { print $8 }' "$out/dynsyms.txt")
	if [ -n "$hidden" ]; then
		echo "FAILED defined dynamic symbols that are not GLOBAL:"
		echo "$hidden"
		status=1
	fi
fi

echo "check-exports: public $(wc -l < "$out/public.txt" | tr -d ' '), cryc_ $(wc -l < "$out/private.txt" | tr -d ' '), defined $(wc -l < "$out/defined.txt" | tr -d ' ')"
if [ $status -eq 0 ]; then
	echo "check-exports: PASS"
else
	echo "check-exports: FAIL"
fi
exit $status

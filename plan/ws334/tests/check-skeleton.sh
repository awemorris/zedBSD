#!/bin/sh
# The shape checks of WS334's modules (the operating-system layers of libbrowser and libmedia, libbrowser's
# storage, libkeiland's Windows files, the apps' tools):
#  1. every module header compiles alone (included twice, so its guard works) with cc, clang and the
#     x86_64 and aarch64 llvm-mingw compilers;
#  2. every module .c compiles with warning 0 with the compilers of its system: the *-posix.c files with
#     cc and clang, the *-win32.c files with both llvm-mingw compilers, the others with all four;
#  3. no module file outside the *-win32.c files and os-win32.h names <windows.h> or tests _WIN32, and no
#     *-posix.c file tests _WIN32 (the system is chosen by the build, not by #ifdef);
#  4. the Windows build skeleton (make browser-win64 and browser-winarm64) compiles the layers;
#  5. with BASE=<commit>, the headers and the test tools are unchanged since that commit (an
#     implementation Phase must not change them; a header fault goes back to Q1).
#   sh plan/ws334/tests/check-skeleton.sh
#   BASE=<sha> sh plan/ws334/tests/check-skeleton.sh
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64).
# Last line: check-skeleton: PASS or check-skeleton: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
. plan/ws334/tests/modules.sh
out=build/tmp/ws334-skeleton
fresh_out "$out"
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
host_flags="-std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE $ws334_includes"
win_flags="-std=gnu11 -Wall -Wextra -Werror -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE $ws334_includes"
status=0

# compile <compiler> <flags> <source> <object>: reports a failure or a warning.
compile() {
	if ! $1 $2 -c "$3" -o "$4" > "$out/compile.log" 2>&1; then
		echo "FAILED $1 $3"
		head -10 "$out/compile.log"
		status=1
	elif [ -s "$out/compile.log" ]; then
		echo "WARNED $1 $3"
		head -10 "$out/compile.log"
		status=1
	fi
}

compilers_host="cc clang"
compilers_win="$LLVM_MINGW/bin/x86_64-w64-mingw32-clang $LLVM_MINGW/bin/aarch64-w64-mingw32-clang"
for compiler in $compilers_host $compilers_win; do
	if ! command -v "$compiler" > /dev/null 2>&1; then
		echo "FAILED $compiler is not installed"
		status=1
	fi
done

n=0
for header in $ws334_headers; do
	printf '#include "%s"\n#include "%s"\nint ws334_header_only;\n' "$header" "$header" > "$out/one.c"
	for compiler in $compilers_host; do
		compile "$compiler" "$host_flags" "$out/one.c" "$out/one.o"
	done
	for compiler in $compilers_win; do
		compile "$compiler" "$win_flags" "$out/one.c" "$out/one.o"
	done
	n=$((n + 1))
done
echo "headers: $n"

n=0
for source in $ws334_sources; do
	object=$out/$(echo "$source" | tr / _).o
	case "$source" in
	*-posix.c)
		for compiler in $compilers_host; do compile "$compiler" "$host_flags" "$source" "$object"; done ;;
	*-win32.c)
		for compiler in $compilers_win; do compile "$compiler" "$win_flags" "$source" "$object"; done ;;
	*)
		for compiler in $compilers_host; do compile "$compiler" "$host_flags" "$source" "$object"; done
		for compiler in $compilers_win; do compile "$compiler" "$win_flags" "$source" "$object"; done ;;
	esac
	n=$((n + 1))
done
echo "sources: $n"

for file in $ws334_headers $ws334_sources; do
	case "$file" in
	*-win32.c|*/os-win32.h) continue ;;
	esac
	if grep -n '_WIN32\|<windows.h>\|<winsock2.h>' "$file"; then
		echo "FAILED $file tests the system (above)"
		status=1
	fi
done

for name in win64 winarm64; do
	if ! timeout 600 make --no-print-directory browser-$name LLVM_MINGW="$LLVM_MINGW" \
	    BROWSER_WIN32_BUILD="$out/browser-$name" > "$out/make-$name.log" 2>&1; then
		echo "FAILED make browser-$name (log $out/make-$name.log)"
		tail -10 "$out/make-$name.log"
		status=1
	elif grep -n 'warning:' "$out/make-$name.log"; then
		echo "FAILED make browser-$name warned (above)"
		status=1
	fi
done

if [ -n "${BASE:-}" ]; then
	if ! git diff --quiet "$BASE" -- $ws334_headers $ws334_tools; then
		echo "FAILED headers or test tools changed since $BASE:"
		git diff --stat "$BASE" -- $ws334_headers $ws334_tools
		status=1
	fi
fi

if [ "$status" = 0 ]; then echo "check-skeleton: PASS"; else echo "check-skeleton: FAIL"; fi
exit "$status"

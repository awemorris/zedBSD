#!/bin/sh
# WS032 p003: host test for the generated cross-build entry points.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# Checks what an upstream build system actually does: compile and link in one
# step, compile and link separately, build a shared library and an archive, and
# configure a CMake project.  Every produced ELF is put through the repository's
# own dynamic-ELF contract check.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
arch=${ZEDBSD_ARCH:-amd64}
platform=${ZEDBSD_PLATFORM_DIR:-$arch}
cross="$root/build/$platform/packages/toolchain"
check="$root/tools/build/check-dynamic-elf.py"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

test -x "$cross/bin/zedbsd-clang" || {
	echo "run-cross-host-test: missing $cross/bin/zedbsd-clang" >&2
	echo "run 'make packages-cross-toolchain' first" >&2
	exit 1
}

checks=0
failures=0
ok() { checks=$((checks + 1)); }
bad() {
	checks=$((checks + 1))
	failures=$((failures + 1))
	echo "FAIL: $*" >&2
}
run() {
	description=$1
	shift
	if "$@" >"$work/out" 2>&1; then ok; else bad "$description: $(cat "$work/out")"; fi
}

CC="$cross/bin/zedbsd-clang"
AR="$cross/bin/zedbsd-ar"

cat > "$work/hello.c" <<'C'
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
	(void)argc;
	printf("hello from %s\n", argv[0] ? argv[0] : "?");
	return strlen("ok") == 2 ? 0 : 1;
}
C
cat > "$work/lib.c" <<'C'
#include <string.h>
#include <stdlib.h>
char *zed_dup(const char *s) {
	size_t n = strlen(s) + 1;
	char *p = malloc(n);
	if (p != NULL) memcpy(p, s, n);
	return p;
}
C
cat > "$work/use.c" <<'C'
#include <stdio.h>
char *zed_dup(const char *);
int main(void) {
	char *p = zed_dup("copied");
	printf("%s\n", p ? p : "(null)");
	return p ? 0 : 1;
}
C

# An upstream configure script compiles and links in one command, with no
# knowledge of start files, loaders or where the shared libc lives.
run "cc hello.c -o hello" "$CC" "$work/hello.c" -o "$work/hello"
run "hello is a conforming application" \
	python3 "$check" --machine "$arch" --role application --needed libc.so "$work/hello"

run "cc -c then link" sh -c \
	"'$CC' -c '$work/hello.c' -o '$work/hello.o' && '$CC' '$work/hello.o' -o '$work/hello2'"
run "separately linked hello is conforming" \
	python3 "$check" --machine "$arch" --role application --needed libc.so "$work/hello2"

run "cc -shared" "$CC" -shared -Wl,-soname,libzedtest.so "$work/lib.c" \
	-o "$work/libzedtest.so"
run "the shared library is conforming and records its libc dependency" \
	python3 "$check" --machine "$arch" --role shared-library \
	--soname libzedtest.so --needed libc.so "$work/libzedtest.so"

run "link against the shared library" sh -c \
	"'$CC' '$work/use.c' -L'$work' -l:libzedtest.so -o '$work/use'"
run "the consumer records both dependencies" \
	python3 "$check" --machine "$arch" --role application \
	--needed libzedtest.so --needed libc.so "$work/use"

# The archive lives in its own directory so that -l finds it rather than the
# shared object built above.
mkdir -p "$work/archive"
run "archive and link against it" sh -c \
	"'$CC' -c '$work/lib.c' -o '$work/lib.o' && '$AR' rcs '$work/archive/libzedtest.a' '$work/lib.o' && '$CC' '$work/use.c' -L'$work/archive' -lzedtest -o '$work/use-static'"
run "the archive consumer needs only libc" \
	python3 "$check" --machine "$arch" --role application --needed libc.so "$work/use-static"

# A compile-only invocation must not acquire start files or libraries.
if "$CC" -c "$work/hello.c" -o "$work/only.o" >"$work/out" 2>&1 &&
   test "$(od -An -tx1 -N4 "$work/only.o" | tr -d ' \n')" = 7f454c46; then
	ok
else
	bad "compile-only did not produce a relocatable object"
fi

# CMake, as the C++ runtime and clang will use it.
if command -v cmake >/dev/null 2>&1; then
	mkdir -p "$work/cmake-project"
	cat > "$work/cmake-project/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(zedcross C)
add_library(zedtest SHARED lib.c)
add_executable(zeduse use.c)
target_link_libraries(zeduse PRIVATE zedtest)
CMAKE
	cp "$work/lib.c" "$work/use.c" "$work/cmake-project/"
	run "cmake configure with the generated toolchain file" \
		cmake -S "$work/cmake-project" -B "$work/cmake-build" \
		-DCMAKE_TOOLCHAIN_FILE="$cross/zedbsd.cmake" \
		-DCMAKE_BUILD_TYPE=Release
	run "cmake build" cmake --build "$work/cmake-build"
	if test -f "$work/cmake-build/libzedtest.so"; then
		run "the cmake shared library is conforming" \
			python3 "$check" --machine "$arch" --role shared-library \
			--soname libzedtest.so --needed libc.so "$work/cmake-build/libzedtest.so"
	else
		bad "cmake did not produce libzedtest.so"
	fi
	if test -f "$work/cmake-build/zeduse"; then
		run "the cmake executable is conforming" \
			python3 "$check" --machine "$arch" --role application \
			--needed libzedtest.so --needed libc.so "$work/cmake-build/zeduse"
	else
		bad "cmake did not produce zeduse"
	fi
else
	echo "run-cross-host-test: cmake is absent, skipping the CMake cases" >&2
fi

echo "cross toolchain host test: $checks checks, $failures failures"
test "$failures" = 0

# Sourced by run-unit.sh: the include paths of the unit tests of the Windows build's modules (the operating-system
# layers of libbrowser and libmedia, libbrowser's storage, libkeiland's Windows files, the apps' tools), and which
# sources each unit test links on each system.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

lb=userland/desktop/libbrowser
lm=userland/desktop/libmedia
lk=userland/desktop/libkeiland
ab=apps/browser-win32

bw_includes="-I. -Iuserland/desktop/include -I$lb -I$lm -I$ab/tools -Iplan/tools/browser-win32/unit"

# libbrowser's threads over the program's table (base/thread.c, base/once.c) and the tests' table of the
# POSIX threads (winpthreads on Windows), which the tests that make a queue or a lock give the engine first.
bw_threads="$lb/base/thread.c $lb/base/once.c userland/tests/browser-threads/threads-posix.c"

# The ISO C helpers of libbrowser's base that the modules use (buffers, vectors, UTF-8 and UTF-16).
bw_base="$lb/base/arena.c $lb/base/buffer.c $lb/base/vector.c $lb/base/utf.c $lb/base/hash.c
	$lb/base/unicode.c $lb/base/unicode-case-table.c"

# bw_test_sources <name> <posix|win32>: the module sources test-<name>.c links on that system
# ("none" when the test does not run there).
bw_test_sources() {
	case "$1:$2" in
	os-win32:posix) echo none ;;
	os-win32:win32) echo "$lb/base/os-win32.c $bw_base" ;;
	os-error:*) echo "$lb/base/os-error.c" ;;
	os-system:posix) echo "$lb/base/os-system-posix.c $bw_base" ;;
	os-system:win32) echo "$lb/base/os-system-win32.c $lb/base/os-win32.c $bw_base" ;;
	os-file:posix) echo "$lb/base/os-file-posix.c $bw_threads $bw_base" ;;
	os-file:win32) echo "$lb/base/os-file-win32.c $lb/base/os-win32.c $bw_threads $bw_base" ;;
	os-net:posix) echo "$lb/base/os-net-posix.c $bw_base" ;;
	os-net:win32) echo "$lb/base/os-net-win32.c $lb/base/os-win32.c $bw_base" ;;
	os-time:posix) echo "$lb/base/os-time-posix.c $bw_threads $bw_base" ;;
	os-time:win32) echo "$lb/base/os-time-win32.c $lb/base/os-zones.c $lb/base/os-win32.c $bw_base" ;;
	os-zones:*) echo "$lb/base/os-zones.c $bw_base" ;;
	records:*) echo "$lb/storage/records.c $bw_base" ;;
	files:posix) echo "$lb/storage/files.c $lb/storage/records.c $lb/base/os-file-posix.c $bw_threads $bw_base" ;;
	files:win32) echo "$lb/storage/files.c $lb/storage/records.c $lb/base/os-file-win32.c $lb/base/os-win32.c $bw_threads $bw_base" ;;
	queue:posix) echo "$lb/storage/queue.c $lb/storage/records.c $lb/base/os-system-posix.c $bw_threads $bw_base" ;;
	queue:win32) echo "$lb/storage/queue.c $lb/storage/records.c $lb/base/os-system-win32.c $lb/base/os-win32.c $bw_threads $bw_base" ;;
	tools:*) echo "$bw_base" ;;
	keys-table:*) echo "$lk/win32/keys-table.c" ;;
	frame:*) echo "$lk/win32/frame.c" ;;
	wide:*) echo "$lk/win32/wide.c" ;;
	input:*) echo "$lk/win32/input.c" ;;
	cf-html:*) echo "$lk/win32/cf-html.c" ;;
	platform-win32:posix) echo none ;;
	platform-win32:win32) echo "$lk/platform/platform-win32.c" ;;
	ico:*) echo "$ab/tools/ico.c" ;;
	media-os:posix) echo "$lm/media-os-posix.c" ;;
	media-os:win32) echo "$lm/media-os-win32.c" ;;
	*) echo unknown ;;
	esac
}

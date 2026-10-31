# Sourced by WS334's test tools: the modules' headers, sources and test tools, the include paths, and
# which sources each unit test links on each system.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

lb=userland/desktop/libbrowser
lm=userland/desktop/libmedia

ws334_includes="-I. -Iuserland/desktop/include -I$lb -I$lm -Iplan/ws334/tests/unit"

# The headers that are the modules' contracts (fixed once the design is frozen).
ws334_headers="$lb/base/os.h $lb/base/os-win32.h $lb/base/os-system.h $lb/base/os-file.h $lb/base/os-net.h
	$lb/base/os-time.h $lb/base/os-zones.h
	userland/desktop/include/browser/storage.h $lb/storage/records.h $lb/storage/files.h $lb/storage/queue.h
	$lm/media-os.h"

# The modules' sources.
ws334_sources="$lb/base/os-win32.c
	$lb/base/os-system-posix.c $lb/base/os-system-win32.c
	$lb/base/os-file-posix.c $lb/base/os-file-win32.c
	$lb/base/os-net-posix.c $lb/base/os-net-win32.c
	$lb/base/os-time-posix.c $lb/base/os-time-win32.c
	$lb/base/os-zones.c
	$lb/storage/records.c $lb/storage/files.c $lb/storage/queue.c
	$lm/media-os-posix.c $lm/media-os-win32.c"

# The test tools an implementation Phase must not change.
ws334_tools="plan/ws334/tests/modules.sh plan/ws334/tests/run-unit.sh plan/ws334/tests/check-skeleton.sh
	plan/ws334/tests/unit/check.h"

# The ISO C helpers of libbrowser's base that the modules use (buffers, vectors, UTF-8 and UTF-16).
ws334_base="$lb/base/arena.c $lb/base/buffer.c $lb/base/vector.c $lb/base/utf.c $lb/base/hash.c
	$lb/base/unicode.c $lb/base/unicode-case-table.c"

# ws334_test_sources <name> <posix|win32>: the module sources test-<name>.c links on that system
# ("none" when the test does not run there).
ws334_test_sources() {
	case "$1:$2" in
	os-win32:posix) echo none ;;
	os-win32:win32) echo "$lb/base/os-win32.c $ws334_base" ;;
	os-system:posix) echo "$lb/base/os-system-posix.c $ws334_base" ;;
	os-system:win32) echo "$lb/base/os-system-win32.c $lb/base/os-win32.c $ws334_base" ;;
	os-file:posix) echo "$lb/base/os-file-posix.c $ws334_base" ;;
	os-file:win32) echo "$lb/base/os-file-win32.c $lb/base/os-win32.c $ws334_base" ;;
	os-net:posix) echo "$lb/base/os-net-posix.c $ws334_base" ;;
	os-net:win32) echo "$lb/base/os-net-win32.c $lb/base/os-win32.c $ws334_base" ;;
	os-time:posix) echo "$lb/base/os-time-posix.c $ws334_base" ;;
	os-time:win32) echo "$lb/base/os-time-win32.c $lb/base/os-zones.c $lb/base/os-win32.c $ws334_base" ;;
	os-zones:*) echo "$lb/base/os-zones.c $ws334_base" ;;
	records:*) echo "$lb/storage/records.c $ws334_base" ;;
	files:posix) echo "$lb/storage/files.c $lb/storage/records.c $lb/base/os-file-posix.c $ws334_base" ;;
	files:win32) echo "$lb/storage/files.c $lb/storage/records.c $lb/base/os-file-win32.c $lb/base/os-win32.c $ws334_base" ;;
	queue:posix) echo "$lb/storage/queue.c $lb/storage/records.c $lb/base/os-system-posix.c $ws334_base" ;;
	queue:win32) echo "$lb/storage/queue.c $lb/storage/records.c $lb/base/os-system-win32.c $lb/base/os-win32.c $ws334_base" ;;
	tools:*) echo "$ws334_base" ;;
	media-os:posix) echo "$lm/media-os-posix.c" ;;
	media-os:win32) echo "$lm/media-os-win32.c" ;;
	*) echo unknown ;;
	esac
}

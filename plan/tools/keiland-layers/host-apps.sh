#!/bin/sh
# Host tests of moved sources whose own tests belong to other workstreams (which this one does not change): each is
# built here as its workstream builds it, with libkeiland's thread layer (platform/thread-posix.c) and the public
# headers added, under ASan and UBSan.  Run from the worktree's root.
#   sh plan/tools/keiland-layers/host-apps.sh [LINUX_BUILD] TEST...     TEST: ime, picker
#     ime     the input method's engines with the user dictionary's writer thread (ime/ja-user.c), the driver
#             plan/ws095/tests/host-engine.c
#     picker  libkeiland's media picker's model (ui/picker-model.c) against the Linux libkeiland (LINUX_BUILD, default
#             build/ws327-linux: make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=DIR DIR/lib/libkeiland.so),
#             plan/ws197/tests/picker-model-host-test.sh
# libkeiland's sound streams (audio/audio.c) are tested by plan/ws191/tests/host-audio-stream.sh, which links the
# thread layer itself.
# Prints one line per test and "ws327-host-apps: PASS" or "FAIL".  Output in build/tmp/ws327-host-apps (a new
# directory each run).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u

. plan/tools/fresh-out.sh

linux=build/ws327-linux
case ${1:-} in
build/*) linux=$1; shift ;;
esac
[ $# -gt 0 ] || { echo "usage: host-apps.sh [LINUX_BUILD] ime|picker..."; exit 2; }
fresh_out build/tmp/ws327-host-apps
out=$fresh_dir
cc=${CC:-cc}
san='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
U=userland/desktop
thread=$U/libkeiland/platform/thread-posix.c
failed=0

# Runs one step, its output in a log of its own; a failure is told with the log's path.
step() {
	name=$1
	shift
	if "$@" > "$out/$name.log" 2>&1; then
		return 0
	fi
	echo "$name: FAIL ($out/$name.log)"
	failed=1
	return 1
}

for test in "$@"; do
	case $test in
	ime)
		D=$U/ime
		# The thread layer is gnu17 (stdatomic); the engines are c11 with declarations first, as their test builds them.
		step ime-build sh -c "$cc -std=gnu17 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror $san -I$U/include -c $thread -o $out/thread-posix.o &&
			$cc -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -Wdeclaration-after-statement $san -I$D -I$U/include \
			plan/ws095/tests/host-engine.c $D/output.c $D/engine-direct.c $D/ja-kana.c $D/ja-romaji.c $D/ja-dict.c \
			$D/ja-user.c $D/ja-inflect.c $D/ja-segment.c $D/ja-engine.c $D/ja-keys.c $D/ja-predict.c $out/thread-posix.o \
			-pthread -o $out/host-engine" &&
		step ime-run timeout 120 "$out/host-engine" plan/ws095/tests/ja-test.dict &&
		echo "ime: PASS ($(tail -n 1 "$out/ime-run.log"))"
		;;
	picker)
		if [ ! -f "$linux/lib/libkeiland.so" ]; then
			echo "picker: NOT-DONE (no $linux/lib/libkeiland.so)"
			failed=1
			continue
		fi
		mkdir -p "$out/picker-run"
		step picker-build $cc -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g $san -I. -I$U/include -I"$linux/include" \
			-o "$out/picker-model-host-test" plan/ws197/tests/picker-model-host-test.c $U/libkeiland/ui/picker-model.c \
			-L"$linux/lib" -Wl,-rpath-link,"$linux/lib" -lkeiland -lpthread &&
		step picker-run env LD_LIBRARY_PATH="$linux/lib" timeout 60 "$out/picker-model-host-test" "$PWD/$out/picker-run" &&
		echo "picker: PASS ($(tail -n 1 "$out/picker-run.log"))"
		;;
	*)
		echo "unknown test $test"
		failed=1
		;;
	esac
done
[ $failed = 0 ] && echo "ws327-host-apps: PASS" || echo "ws327-host-apps: FAIL"
[ $failed = 0 ]

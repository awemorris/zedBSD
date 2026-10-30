#!/bin/sh
# ws095: builds and runs the host tests of the input method's engines (host-engine.c)
# with the engines' sources, on Linux, under ASan and UBSan.  REmacs's SKK-JISYO.X is
# read too when it is at hand (userland/base/emacs/dict since 2026-10-04, else the shared build/sources/remacs, read only).
#   sh plan/ws095/tests/host-engine.sh [OUTPUT]   (default build/ws095/host-engine)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -e
cd "$(dirname "$0")/../../.."
out=${1:-build/ws095/host-engine}
mkdir -p "$(dirname "$out")"
D=userland/desktop/ime
san='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
# The user dictionary's writer (ja-user.c) runs on libkeiland's threads: their POSIX layer, gnu17 for its atomics.
${CC:-clang} -std=gnu17 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror $san -Iuserland/desktop/include \
	-c userland/desktop/libkeiland/platform/thread-posix.c -o "$out-thread-posix.o"
${CC:-clang} -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -Wdeclaration-after-statement \
	$san -I$D -Iuserland/desktop/include \
	plan/ws095/tests/host-engine.c $D/output.c $D/engine-direct.c $D/ja-kana.c $D/ja-romaji.c \
	$D/ja-dict.c $D/ja-user.c $D/ja-inflect.c $D/ja-segment.c $D/ja-engine.c $D/ja-keys.c $D/ja-predict.c \
	"$out-thread-posix.o" -pthread -o "$out"
x=
for candidate in userland/base/emacs/dict/SKK-JISYO.X build/sources/remacs/dict/SKK-JISYO.X; do
	if [ -f "$candidate" ]; then x=$candidate; break; fi
done
"$out" plan/ws095/tests/ja-test.dict $x

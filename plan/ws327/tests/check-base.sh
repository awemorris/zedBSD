#!/bin/sh
# What an implementation phase may change, against the point its branch left main (git merge-base HEAD main),
# committed or not:
#   1. the frozen files are as they were there: the public headers, libkeiland's exports and POSIX layers and
#      Makefiles, the design and the tests' tools (p017 alone may change the two layers' headers' comments);
#   2. every changed or new file is one the phase's phase.md names (its §2);
#   3. the changes have no whitespace errors.
# Run from the worktree's root.
#   sh plan/ws327/tests/check-base.sh PHASE     (PHASE: p008 ... p015, p017)
# Prints what is out of place and "ws327-check-base: PASS" or "FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
phase=${1:?usage: check-base.sh PHASE}
D=userland/desktop
K=$D/libkeiland
case $phase in
p008) allowed="$D/mailer/ plan/tools/mail/run-host-mail-backend.sh plan/tools/mail/host-mail-backend.c plan/tools/mail/host-mail-sync.c" ;;
p009) allowed="$D/music/ $D/videoplayer/ $D/media-app/avcodec.c plan/tools/music/host-music-play.sh plan/tools/media/run-host-codec.sh" ;;
p010) allowed="$D/photos/thumbs.c $D/photos/media-worker.c" ;;
p011) allowed="$D/settings/settings.h $D/settings/look.c $D/settings/storage-scan.h $D/settings/storage-scan.c $D/settings/storage-trash.h $D/settings/storage-trash.c" ;;
p012) allowed="$D/ime/ja-user.c $K/ui/picker.h $K/ui/picker-model.c $K/audio/audio.c" ;;
p013) allowed="$K/platform/thread-win32.c" ;;
p014) allowed="$K/platform/socket-win32.c" ;;
p015) allowed="docs/howto/keiland-programming.md docs/howto/keiland-examples/08-worker.c docs/howto/keiland-examples/08-language.c docs/howto/keiland-examples/08-network.c" ;;
p017) allowed="*" ;;
*) echo "ws327-check-base: unknown phase $phase"; exit 2 ;;
esac
own="plan/ws327/phase${phase#p}/"
frozen="$D/include/keiland/ $K/exports.map $K/exports.py $K/platform/thread-posix.c $K/platform/socket-posix.c $K/Makefile $K/Makefile.linux $K/Makefile.freebsd plan/ws327/design.md plan/ws327/tests/"
fork=$(git merge-base HEAD main) || { echo "ws327-check-base: no merge base with main"; exit 2; }
failed=0

# 1. The frozen files (the layers' headers' comments are p017's to mend).
for file in $(git diff --name-only "$fork" -- $frozen; git ls-files --others --exclude-standard -- $frozen); do
	case $phase:$file in
	p017:$D/include/keiland/keiland_thread.h|p017:$D/include/keiland/keiland_socket.h) continue ;;
	esac
	echo "frozen file changed: $file (stop and report it instead of changing it)"
	failed=1
done

# 2. Every changed or new file is the phase's: a named file, or anything under a named directory.
if [ "$allowed" != "*" ]; then
	for file in $(git diff --name-only "$fork"; git ls-files --others --exclude-standard | grep -v '^build/'); do
		ok=0
		for path in $allowed $own; do
			case $path in
			*/) case $file in "$path"*) ok=1 ;; esac ;;
			*) [ "$file" = "$path" ] && ok=1 ;;
			esac
		done
		[ $ok = 1 ] || { echo "not this phase's file: $file"; failed=1; }
	done
fi

# 3. Whitespace.
if ! git diff --check "$fork" > /dev/null 2>&1; then
	git diff --check "$fork"
	echo "whitespace errors (above)"
	failed=1
fi
[ $failed = 0 ] && echo "ws327-check-base: PASS" || echo "ws327-check-base: FAIL"
[ $failed = 0 ]

#!/bin/sh
# Builds zedBSD's ssh programs for the host (userland/base/ssh): ssh, ssh-keygen, sshd and sftp-server, with ASan and
# UBSan, against the host's OpenSSL 3.5 through the shims in plan/ws336/tests/host-crypto (libcrypto-compat has
# OpenSSL 3.5's interface).  Each program is its main file, the program files of its own (ssh-client-*.c for ssh,
# sshd-*.c and the console login's password check for sshd) and every library module.
#   sh plan/ws336/tests/build-host.sh <out-dir> [program...]     default: every program
# SSH_SOURCE_DIR=<dir> takes the sources from another directory (a changed copy under build/tmp).
# Prints "build-host: <program> ok" or "build-host: <program> FAIL" for each; last line: build-host: PASS or FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
if [ $# -lt 1 ]; then
	echo "usage: sh plan/ws336/tests/build-host.sh <out-dir> [program...]"
	echo "build-host: FAIL"
	exit 2
fi
out=$1
shift
programs="$*"
[ -n "$programs" ] || programs="ssh ssh-keygen sshd sftp-server"
mkdir -p "$out"
source_dir=${SSH_SOURCE_DIR:-userland/base/ssh}

modules=""
for file in "$source_dir"/ssh-*.c; do
	case "$file" in
	*-main.c|*/ssh-client-*.c) ;;
	*) modules="$modules $file" ;;
	esac
done

flags="-std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=undefined
 -fno-omit-frame-pointer -I. -I$source_dir -Iplan/ws336/tests/host-crypto"
status=0
for program in $programs; do
	case "$program" in
	ssh) sources="$source_dir/ssh-main.c $(ls "$source_dir"/ssh-client-*.c)"; libraries="-lcrypto" ;;
	ssh-keygen) sources="$source_dir/ssh-keygen-main.c"; libraries="-lcrypto" ;;
	sshd) sources="$(ls "$source_dir"/sshd-*.c) userland/base/login/verify.c"; libraries="-lcrypto -lcrypt"
		flags_extra="-Iuserland/base/login" ;;
	sftp-server) sources="$source_dir/sftp-server-main.c"; libraries="-lcrypto" ;;
	*) echo "build-host: $program FAIL (unknown program)"; status=1; continue ;;
	esac
	[ "$program" = sshd ] || flags_extra=""
	# shellcheck disable=SC2086
	if timeout 300 cc $flags $flags_extra $sources $modules $libraries -o "$out/$program" > "$out/$program.build.log" 2>&1; then
		echo "build-host: $program ok"
	else
		echo "build-host: $program FAIL (log $out/$program.build.log)"
		head -20 "$out/$program.build.log"
		status=1
	fi
done
if [ "$status" = 0 ]; then echo "build-host: PASS"; else echo "build-host: FAIL"; fi
exit "$status"

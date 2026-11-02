#!/bin/sh
# The host checks of zedBSD's ssh programs against libcrypto-compat (userland/base/libcrypto-compat) in the place of
# the host's OpenSSL: builds libcrypto-compat for the host from its sources.mk (ASan and UBSan, as the programs are)
# into a static library, points SSH_CRYPTO_INCLUDE (a directory whose compat/ is include/libc/compat) and
# SSH_CRYPTO_LIB at it, and runs crypto-contract.sh, run-unit.sh all, sshd-pair-host.sh and interop-host.sh; and
# checks the package's files (userland/base/ssh/files) with the programs built on it: ssh-keygen -A makes the host
# key under a fresh root, sshd -t takes sshd_config (its HostKey moved under that root), ssh reads ssh_config up to
# the connection, sh -n reads sshd-start.
#   sh plan/ws336/tests/compat-host.sh [check...]     checks: contract unit files pair interop (default: all)
# CRYPTO_COMPAT_SOURCE_DIR=<dir> takes libcrypto-compat's files from another directory (a changed copy under
# build/tmp) in the place of userland/base/libcrypto-compat.
# Prints the last line of each check; last line: compat-host: PASS or compat-host: FAIL.  Output under
# build/tmp/ssh-compat-host (a fresh run directory); the checks keep their own output directories.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
fresh_out build/tmp/ssh-compat-host
out=$(cd build/tmp/ssh-compat-host && pwd -P)
checks="$*"
[ -n "$checks" ] || checks="contract unit files pair interop"
compat_dir=${CRYPTO_COMPAT_SOURCE_DIR:-userland/base/libcrypto-compat}
status=0

# The headers as a program on zedBSD finds them (<compat/crypto/...>), without the rest of include/libc.
mkdir -p "$out/include" "$out/obj"
ln -s "$(pwd -P)/include/libc/compat" "$out/include/compat"

# The library's files, as its Makefile takes them.
sources=$(printf 'include userland/base/libcrypto-compat/sources.mk\nall:\n\t@echo $(LIBCRYPTO_COMPAT_SOURCES) $(LIBCRYPTO_COMPAT_POSIX_SOURCES)\n' |
    make -s -f - | sed "s#userland/base/libcrypto-compat/#$compat_dir/#g")
if [ -z "$sources" ]; then
	echo "compat-host: no sources from userland/base/libcrypto-compat/sources.mk"
	echo "compat-host: FAIL"
	exit 1
fi
count=0
for source in $sources; do
	object="$out/obj/$(basename "$source" .c).o"
	if ! timeout 300 cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
	    -fno-sanitize-recover=undefined -fno-omit-frame-pointer -I"$out/include" -I. -c "$source" -o "$object" \
	    >> "$out/build.log" 2>&1; then
		echo "compat-host: $source does not build (log $out/build.log)"
		echo "compat-host: FAIL"
		exit 1
	fi
	count=$((count + 1))
done
ar rcs "$out/libcrypto-compat-host.a" "$out"/obj/*.o
echo "libcrypto-compat for the host: $count files"
SSH_CRYPTO_INCLUDE=$out/include
SSH_CRYPTO_LIB="$out/libcrypto-compat-host.a -lpthread"
export SSH_CRYPTO_INCLUDE SSH_CRYPTO_LIB

# The package's files, with the programs on libcrypto-compat (build-host.sh); prints FAIL lines and "files: PASS"
# or "files: FAIL".
files_check() {
	work=$1
	failed=0
	mkdir -p "$work/root/etc/ssh"
	if ! sh plan/ws336/tests/build-host.sh "$work/bin" ssh ssh-keygen sshd > "$work/build.log" 2>&1; then
		echo "FAIL build-host: $(tail -1 "$work/build.log")"
		echo "files: FAIL"
		return 1
	fi
	"$work/bin/ssh-keygen" -q -A -f "$work/root" || { echo "FAIL ssh-keygen -A"; failed=1; }
	[ -f "$work/root/etc/ssh/ssh_host_ed25519_key" ] || { echo "FAIL no ed25519 host key under the root"; failed=1; }
	sed "s#^HostKey /etc/ssh/#HostKey $work/root/etc/ssh/#" userland/base/ssh/files/sshd_config > "$work/sshd_config"
	grep -q "^HostKey $work/root/etc/ssh/ssh_host_ed25519_key\$" "$work/sshd_config" || { echo "FAIL sshd_config names no ed25519 HostKey"; failed=1; }
	[ "$(grep -c '^HostKey ' "$work/sshd_config")" = 1 ] || { echo "FAIL sshd_config names another HostKey"; failed=1; }
	"$work/bin/sshd" -t -f "$work/sshd_config" || { echo "FAIL sshd -t refuses sshd_config"; failed=1; }
	# A closed port: ssh reads the configuration and then fails to connect, which it says as "Connection refused".
	port=$(/usr/bin/python3 -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
	HOME=$work timeout 30 "$work/bin/ssh" -F userland/base/ssh/files/ssh_config -o BatchMode=yes -p "$port" 127.0.0.1 true \
	    > "$work/ssh.out" 2>&1
	grep -q 'Connection refused' "$work/ssh.out" || { echo "FAIL ssh with ssh_config: $(cat "$work/ssh.out")"; failed=1; }
	sh -n userland/base/ssh/files/sshd-start || { echo "FAIL sh -n sshd-start"; failed=1; }
	[ -x userland/base/ssh/files/sshd-start ] || { echo "FAIL sshd-start is not executable"; failed=1; }
	if [ "$failed" = 0 ]; then echo "files: PASS"; else echo "files: FAIL"; fi
	return "$failed"
}

for check in $checks; do
	case "$check" in
	contract) script="plan/ws336/tests/crypto-contract.sh"; arguments="" ;;
	files)
		files_check "$out/files" > "$out/files.log" 2>&1
		last=$(tail -1 "$out/files.log")
		echo "$last"
		[ "$last" = "files: PASS" ] || { grep FAIL "$out/files.log" | head -10; status=1; }
		continue ;;
	unit) script="plan/ws336/tests/run-unit.sh"; arguments="all" ;;
	pair) script="plan/ws336/tests/sshd-pair-host.sh"; arguments="" ;;
	interop) script="plan/ws336/tests/interop-host.sh"; arguments="" ;;
	*) echo "compat-host: unknown check $check"; status=1; continue ;;
	esac
	# shellcheck disable=SC2086
	timeout 3600 sh "$script" $arguments > "$out/$check.log" 2>&1
	last=$(tail -1 "$out/$check.log")
	echo "$check: $last"
	case "$last" in
	*": PASS") ;;
	*) grep 'FAIL' "$out/$check.log" | head -10; status=1 ;;
	esac
done

sync
if [ "$status" = 0 ]; then echo "compat-host: PASS"; else echo "compat-host: FAIL"; fi
exit "$status"

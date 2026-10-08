#!/bin/sh
# ws173-p003: builds the AAT image (config-amd64-aat.mk) with plan/tools/guest/test-image.sh.  Root's authorized key
# and the SSH host keys come from the guest harness (plan/tools/guest/guest.py extra-files, made once under
# plan/tmp/guest), so aat reaches root with the harness's key; the harness's /etc/net.conf is left out, because it
# addresses QEMU's USB network adapter and would replace the machine's own network settings.
#   plan/tools/aat/build-image.sh BUILD [ARGUMENT...]     (arguments as test-image.sh takes them)
# AAT_CONFIG names another configuration that includes config-amd64-aat.mk (default config-amd64-aat.mk; q911:
# config-amd64-aat-bugs.mk, the UI bugs' sweep).
# The image is BUILD/hdd-image.img (written to the USB stick for the 5330, or booted by guest.sh for the self-test).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
[ $# -ge 1 ] || { echo "usage: build-image.sh BUILD [ARGUMENT...]" >&2; exit 2; }
build=$1
shift
files=$(python3 plan/tools/guest/guest.py extra-files | sed -n "s/^ZEDBSD_TEST_EXTRA_FILES='\(.*\)'$/\1/p")
[ -n "$files" ] || { echo "build-image: no guest harness files" >&2; exit 1; }
# Every --file and --mode pair but the network's.
keep=
pending=
for word in $files; do
	case $word in
	--file|--mode) pending=$word; continue ;;
	/etc/net.conf=*) pending=; continue ;;
	esac
	[ -n "$pending" ] && keep="$keep $pending $word"
	pending=
done
# shellcheck disable=SC2086
exec plan/tools/guest/test-image.sh --no-harness "${AAT_CONFIG:-plan/tools/aat/config-amd64-aat.mk}" "$build" $keep "$@"

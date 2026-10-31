#!/bin/sh
# Install-admin's editor requests, refusals and cancellation.  Boots IMAGE (the installer's image of
# plan/tools/installer/config-amd64-p009.mk, as for p009-install.sh) from USB with one empty 8 GiB NVMe disk and runs
# plan/tools/installer/p009-edit-cancel-steps.sh in it over SSH.  The summary is the steps' lines and
# "p009-edit-cancel: PASS|FAIL OUT".  Output goes to a new build/installer-test/p009e.*.
#   sh plan/tools/installer/p009-edit-cancel.sh IMAGE
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname "$0")/../../.."
image=${1:?usage: p009-edit-cancel.sh IMAGE}
mkdir -p build/installer-test
out=$(mktemp -d build/installer-test/p009e.XXXXXX)
export GUEST_RUNTIME="$(pwd)/$out/guest"
truncate -s 8G "$out/nvme.img"
cp --sparse=always "$image" "$out/boot.img"
g() { timeout "${G_TIMEOUT:-1800}" python3 plan/tools/guest/guest.py "$@"; }
status=0
g start "$out/boot.img" --disk usb --qemu-extra "-drive file=$out/nvme.img,if=none,id=p9n,format=raw -device nvme,serial=p009e,drive=p9n" > "$out/start.txt" 2>&1
if g wait --timeout 600 > "$out/wait.txt" 2>&1; then
	g put plan/tools/installer/p009-edit-cancel-steps.sh /tmp/p009e-steps.sh > /dev/null 2>&1 || true
	G_TIMEOUT=3600 g run sh /tmp/p009e-steps.sh > "$out/steps.txt" 2>&1 || true
else
	echo "p009-edit-cancel: the live system did not answer" > "$out/steps.txt"
fi
g stop > /dev/null 2>&1 || true
grep -E '^(DISK|STEP|OUT|REQ|ERR|PROBE|NW|P009E)' "$out/steps.txt" || true
grep -q '^P009E DONE' "$out/steps.txt" || status=1
grep -q '^STEP .* fail' "$out/steps.txt" && status=1
[ $status -eq 0 ] && echo "p009-edit-cancel: PASS $out" || echo "p009-edit-cancel: FAIL $out"
exit $status

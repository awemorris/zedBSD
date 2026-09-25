#!/bin/sh
# ws042: runs the sh cases that sh-diff.py --export wrote on the amd64 guest,
# 40 at a time, starting the guest again every 10 batches (BUG-029), and
# writes each batch's FAIL lines and PASS count to OUTPUT; the last line is
# ALL-BATCHES-FINISHED.  The guest image must be built already.
#   [IMAGE=...] sh plan/tools/sh/guest-batches.sh [OUTPUT]
#   (default OUTPUT build/ws042/guest-all.out; IMAGE the native guest image
#   build/ws053-full-hal-guest/hdd-image.img, booted from NVMe)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:-build/ws042/guest-all.out}
work=build/ws042/guest-batches
export GUEST_RUNTIME=${GUEST_RUNTIME:-build/ws042/guest-run}
image=${IMAGE:-build/ws053-full-hal-guest/hdd-image.img}

# The oils test data the cases read (REPO_ROOT on the guest is /root/oils).
rm -rf "$work"
# oils/bin is there, empty, as in the oils tree (a glob case looks for it).
mkdir -p "$work/oils/oils/spec" "$work/oils/oils/bin"
cp -r build/ws042/oils/spec/testdata build/ws042/oils/spec/bin "$work/oils/oils/spec/"
tar -C "$work/oils" -cf "$work/oils.tar" oils

# The cases, 40 to a batch.
ls build/ws042/guest-export/*/*.sh | sort > "$work/cases"
split -l 40 "$work/cases" "$work/batch."
: > "$out"
n=0
for batch in "$work"/batch.*; do
	if [ $((n % 10)) -eq 0 ]; then
		python3 plan/tools/guest/guest.py stop >/dev/null 2>&1
		python3 plan/tools/guest/guest.py start --disk nvme "$image" >/dev/null 2>&1
		python3 plan/tools/guest/guest.py wait >/dev/null 2>&1
		python3 plan/tools/guest/guest.py put plan/tools/sh/guest-diff.sh /tmp/guest-diff.sh
		python3 plan/tools/guest/guest.py put "$work/oils.tar" /root/oils.tar
		python3 plan/tools/guest/guest.py run 'cd /root && rm -rf oils && pax -r -f oils.tar && rm -f oils.tar'
	fi
	n=$((n + 1))
	rm -rf "$work/one" && mkdir -p "$work/one/export/00"
	while read -r code; do
		cp "$code" "${code%.sh}.exp" "$work/one/export/00/"
	done < "$batch"
	tar -C "$work/one" -cf "$work/one.tar" export
	python3 plan/tools/guest/guest.py put "$work/one.tar" /tmp/shb.tar
	python3 plan/tools/guest/guest.py run "cd /tmp && pax -r -f shb.tar && rm -f shb.tar && sh /tmp/guest-diff.sh /tmp/export /root/oils 2>&1; rm -rf /tmp/export" >> "$out" 2>&1
done
python3 plan/tools/guest/guest.py stop >/dev/null 2>&1
echo ALL-BATCHES-FINISHED >> "$out"

#!/bin/sh
# Runs in the installer's live system (as root, through plan/tools/installer/p009-edit-cancel.sh) with one empty NVMe disk
# (8 GiB): the editor's requests on an empty GPT (a partition with a new ESP, another, the second deleted), the ones
# install-admin refuses (a second ESP, a table the screen no longer shows), a whole-disk install cancelled while it
# unpacks (nothing left mounted, no zedbsd.cfg), and a user outside wheel who cannot run the helper.  Each step prints
# "STEP name ok|fail"; the last line is "P009E DONE".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
w=/tmp/p009e
mkdir -p $w /mnt/p009e
admin=/usr/libexec/install-admin
step() { sname=$1; shift; if "$@" > $w/step.txt 2>&1; then echo "STEP $sname ok"; else echo "STEP $sname fail"; cat $w/step.txt; fi; }
f() { printf '%d:%s' "${#1}" "$1"; }
probe() { $admin probe > $w/probe.txt 2> $w/probe.err; }
field() { awk -F'\t' -v c="$1" '$1 == "disk" && $5 == 512 && $6 == 16777216 { print $c; exit }' $w/probe.txt; }
disk_words() { echo "disk $(field 3) $(field 2) 512 16777216 $(field 12)"; }
session() {
	: > "$2"
	{
		cat "$1"
		n=0
		while ! grep -Eq '^(DONE|FAIL)' "$2" && [ $n -lt 3000 ]; do sleep 1; n=$((n + 1)); done
	} | $admin session > "$2" 2> "$2.err"
}
people="user $(f tester) fullname $(f 'P009 Tester') password $(f pw-tester-9) root-password $(f pw-root-9) autologin no hostname $(f p009host)"

probe
reg=$(field 3)
disk=$(field 2)
echo "DISK $disk $reg"
step gpt sh -c "echo 'ERASE $disk:$reg' | diskpart init $disk 5b6c7d8e-9f0a-4b1c-8d2e-3f4a5b6c7d8e"
probe
printf '%s\n' "edit create-with-esp $(disk_words) esp 2048 1048576 new 1050624 4194304" > $w/e1.req
session $w/e1.req $w/e1.out
step create-esp grep -q '^DONE' $w/e1.out
probe
old="$(disk_words)"
printf '%s\n' "edit create $(disk_words) new 5244928 2097152" > $w/e2.req
session $w/e2.req $w/e2.out
step create grep -q '^DONE' $w/e2.out
uuid=$(awk '$1 == "STATE" && $2 == "created" && $3 == "part" { print $6 }' $w/e2.out)
probe
step two-made test "$(awk -F'\t' -v r="$reg" '$1 == "part" && $2 == r' $w/probe.txt | wc -l | tr -d ' ')" = 3
printf '%s\n' "edit create $old new 7342080 1048576" > $w/stale.req
session $w/stale.req $w/stale.out
grep '^FAIL' $w/stale.out | sed 's/^/OUT stale /'
step stale-refused grep -q '^FAIL edit' $w/stale.out
printf '%s\n' "edit create-with-esp $(disk_words) esp 7342080 1048576 new 8390656 1048576" > $w/esp2.req
session $w/esp2.req $w/esp2.out
grep '^FAIL' $w/esp2.out | sed 's/^/OUT esp2 /'
step second-esp-refused grep -q '^FAIL edit' $w/esp2.out
printf '%s\n' "edit delete $(disk_words) part $reg:5244928:2097152:$uuid" > $w/del.req
session $w/del.req $w/del.out
cat $w/del.req | sed 's/^/REQ del /'
grep -E '^(STATE|FAIL|DONE)' $w/del.out | sed 's/^/OUT del /'
tail -3 $w/del.out.err | sed 's/^/ERR del /'
step delete grep -q '^DONE' $w/del.out
probe
step one-gone test "$(awk -F'\t' -v r="$reg" '$1 == "part" && $2 == r' $w/probe.txt | wc -l | tr -d ' ')" = 2
awk -F'\t' -v r="$reg" '$1 == "part" && $2 == r' $w/probe.txt | sed 's/^/PROBE /'

# A whole-disk install, cancelled once it unpacks.
printf '%s\n' "install $(disk_words) whole 536870912 1073741824 units 4 base desktop firmware lang-clang $people" > $w/cancel.req
: > $w/cancel.out
{
	cat $w/cancel.req
	n=0
	while ! grep -q '^STEP 6 ' $w/cancel.out && [ $n -lt 900 ]; do sleep 1; n=$((n + 1)); done
	sleep 2
	echo cancel
	n=0
	while ! grep -Eq '^(DONE|FAIL)' $w/cancel.out && [ $n -lt 900 ]; do sleep 1; n=$((n + 1)); done
} | $admin session > $w/cancel.out 2> $w/cancel.err
grep -E '^(STEP|UNIT|FAIL|STATE|DONE)' $w/cancel.out | sed 's/^/OUT cancel /'
step cancelled grep -qi '^FAIL unpack .*cancel' $w/cancel.out
step nothing-mounted sh -c "! mount | grep -q install-admin"
probe
esp=$(awk -F'\t' -v r="$reg" '$1 == "part" && $2 == r && toupper($6) == "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" { print $9; exit }' $w/probe.txt)
step esp-mount mount -t fat -r /dev/$esp /mnt/p009e
step no-cfg sh -c "! test -e /mnt/p009e/zedbsd.cfg"
step esp-umount umount /mnt/p009e

# A user outside wheel.
step useradd useradd -m nw
su nw -c "$admin probe" > $w/nw.txt 2>&1
echo "NW status=$? $(head -1 $w/nw.txt)"
step nw-refused sh -c "! grep -q '^end' $w/nw.txt"
echo "P009E DONE"

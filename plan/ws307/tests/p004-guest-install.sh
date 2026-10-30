#!/bin/sh
# Runs in the installer's live system (as root, through plan/ws307/tests/p004-gtk.sh) with one empty NVMe disk of
# 512-byte blocks (24 GiB).  Through install-admin, as the installer would ask it: the whole disk with the base, the
# desktop, the firmware, OpenSSH, OpenSSL, Clang (and pkgconf, its item's) and GTK4, the user tester with the
# automatic login.  The installed root is read back (the units' record, clang, pkg-config, the .pc files and headers
# GTK4 needs), the harness's SSH keys and net.conf are put on it, and last a file is written to it, renamed and synced
# with the root left mounted (the j3 rule), for the host to read through the loader's reader.  Each step prints
# "STEP name ok|fail"; the last line is "P004 DONE".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
w=/tmp/p004
mkdir -p $w /mnt/p004r
admin=/usr/libexec/install-admin
step() { sname=$1; shift; if "$@" > $w/step.txt 2>&1; then echo "STEP $sname ok"; else echo "STEP $sname fail"; cat $w/step.txt; fi; }
f() { printf '%d:%s' "${#1}" "$1"; }
probe() { $admin probe > $w/probe.txt 2> $w/probe.err; }
field() { awk -F'\t' -v b="$1" -v s="$2" -v c="$3" '$1 == "disk" && $5 == b && $6 == s { print $c; exit }' $w/probe.txt; }
part_device() { awk -F'\t' -v r="$1" -v t="$2" '$1 == "part" && $2 == r && toupper($6) == t { print $9; exit }' $w/probe.txt; }
disk_words() { echo "disk $(field "$1" "$2" 3) $(field "$1" "$2" 2) $1 $2 $(field "$1" "$2" 12)"; }
people="user $(f tester) fullname $(f 'P004 Tester') password $(f pw-tester-4) root-password $(f pw-root-4) autologin yes hostname $(f p004host)"
session() {
	: > "$2"
	{
		cat "$1"
		n=0
		while ! grep -Eq '^(DONE|FAIL)' "$2" && [ $n -lt 3000 ]; do sleep 1; n=$((n + 1)); done
	} | $admin session > "$2" 2> "$2.err"
	grep -q '^DONE' "$2"
}
ROOT=516E7CB6-6ECF-11D6-8FF8-00022D09712B
SECTORS=50331648

step probe probe
units="base desktop firmware network-openssh security-openssl lang-clang devel-pkgconf desktop-gtk4"
set -- $units
printf '%s\n' "install $(disk_words 512 $SECTORS) whole 536870912 1073741824 units $# $units $people" > $w/install.req
step install session $w/install.req $w/install.out
grep -E '^(STEP|FAIL|DONE)' $w/install.out | sed 's/^/OUT /'
probe

r=$(field 512 $SECTORS 3)
root=$(part_device "$r" $ROOT)
start=$(awk -F'\t' -v r="$r" -v t="$ROOT" '$1 == "part" && $2 == r && toupper($6) == t { print $4; exit }' $w/probe.txt)
echo "INSTALLED root=$root start=$start"
step mount-root mount -t ufs /dev/$root /mnt/p004r
echo "UNITS $(cut -f1 /mnt/p004r/var/db/installer/units | tr '\n' ' ')"
step record-pkgconf grep -q '^devel-pkgconf	' /mnt/p004r/var/db/installer/units
step record-libcxx grep -q '^devel-libcxx	' /mnt/p004r/var/db/installer/units
step clang test -x /mnt/p004r/usr/bin/clang
step pkg-config test -x /mnt/p004r/usr/bin/pkgconf -a -L /mnt/p004r/usr/bin/pkg-config
step gtk4-pc test -f /mnt/p004r/usr/lib/pkgconfig/gtk4.pc
step gtk4-header test -f /mnt/p004r/usr/include/gtk-4.0/gtk/gtk.h
step glibconfig test -f /mnt/p004r/usr/lib/glib-2.0/include/glibconfig.h
step gtk4-link test -L /mnt/p004r/usr/lib/libgtk-4.so
step libcxx-header test -f /mnt/p004r/usr/include/c++/v1/vector
ls /mnt/p004r/usr/lib/pkgconfig | tr '\n' ' ' | sed 's/^/PC /'
echo

# The harness's keys and network, for the installed system's boot that follows.
harness() {
	mkdir -p /mnt/p004r/home/tester/.ssh &&
	cp /root/.ssh/authorized_keys /mnt/p004r/home/tester/.ssh/authorized_keys &&
	chmod 700 /mnt/p004r/home/tester /mnt/p004r/home/tester/.ssh && chmod 600 /mnt/p004r/home/tester/.ssh/authorized_keys &&
	chown -R 1000:1000 /mnt/p004r/home/tester &&
	cp /etc/net.conf /mnt/p004r/etc/net.conf &&
	cp /etc/ssh/ssh_host_*_key /etc/ssh/ssh_host_*_key.pub /mnt/p004r/etc/ssh/ &&
	chmod 600 /mnt/p004r/etc/ssh/ssh_host_*_key
}
step harness harness

# The rule for a new kernel (spec 5.10): written under a new name, renamed, synced; the root stays mounted.
step sync-write dd if=/dev/urandom of=/mnt/p004r/boot/p004-sync.new bs=4096 count=700
step sync-rename mv /mnt/p004r/boot/p004-sync.new /mnt/p004r/boot/p004-sync.bin
step sync-sync sync
echo "SYNCROOT $root $start"
echo "SYNCSUM /boot/p004-sync.bin $(cksum < /mnt/p004r/boot/p004-sync.bin)"
echo "P004 DONE"

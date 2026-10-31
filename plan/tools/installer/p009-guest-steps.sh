#!/bin/sh
# Runs in the installer's live system (as root, through plan/tools/installer/p009-install.sh) with four empty disks: an NVMe
# disk of 512-byte blocks (8 GiB), a SATA disk (9 GiB), a USB disk (10 GiB) and an NVMe disk of 4096-byte blocks
# (8 GiB, 2097152 blocks).  Through install-admin alone (as the installer would ask it): the probe, as root and as the
# image's user; a refused install on the live disk; whole-disk installs on the NVMe disk (desktop, OpenSSH and clang,
# a swap partition), the SATA disk (the base and OpenSSH, a swap file) and the 4096 disk (the base and OpenSSH, a swap
# partition); on the USB disk an empty GPT, then the editor's requests (a partition with a new ESP, another one) and an
# install on the chosen partitions.  Each installed root and ESP is then read back (accounts, hostname, sshd, fstab,
# the record, the kernel, the loader and zedbsd.cfg; the home is made at the installed system's first boot by its
# homes service), the harness's SSH keys and net.conf are put on the NVMe and SATA
# roots (for the boots that follow), and last a file is written to the SATA root, renamed and synced with the root
# left mounted, for the host to read through the loader's reader.  Each step prints "STEP name ok|fail"; the last line
# is "P009 DONE".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
w=/tmp/p009
mkdir -p $w /mnt/p009r /mnt/p009e
admin=/usr/libexec/install-admin
step() { sname=$1; shift; if "$@" > $w/step.txt 2>&1; then echo "STEP $sname ok"; else echo "STEP $sname fail"; cat $w/step.txt; fi; }
f() { printf '%d:%s' "${#1}" "$1"; }
probe() { $admin probe > $w/probe.txt 2> $w/probe.err; }
# A disk's field ($3, the probe's column) by its sector size ($1) and its size in sectors ($2).
field() { awk -F'\t' -v b="$1" -v s="$2" -v c="$3" '$1 == "disk" && $5 == b && $6 == s { print $c; exit }' $w/probe.txt; }
# The device of a disk's partition of a type ($2), by the disk's registration ($1).
part_device() { awk -F'\t' -v r="$1" -v t="$2" '$1 == "part" && $2 == r && toupper($6) == t { print $9; exit }' $w/probe.txt; }
disk_words() { echo "disk $(field "$1" "$2" 3) $(field "$1" "$2" 2) $1 $2 $(field "$1" "$2" 12)"; }
people="user $(f tester) fullname $(f 'P009 Tester') password $(f pw-tester-9) root-password $(f pw-root-9) autologin no hostname $(f p009host)"
# One session with one request ($1, a file): the input stays open until the answer ends, then the session ends.
session() {
	: > "$2"
	{
		cat "$1"
		n=0
		while ! grep -Eq '^(DONE|FAIL)' "$2" && [ $n -lt 3000 ]; do sleep 1; n=$((n + 1)); done
	} | $admin session > "$2" 2> "$2.err"
	grep -q '^DONE' "$2"
}
ESP=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
ROOT=516E7CB6-6ECF-11D6-8FF8-00022D09712B

step probe probe
grep -E '^(firmware|memory|disk)' $w/probe.txt | sed 's/^/PROBE /'
step probe-as-user su user -c "$admin probe"
step probe-end grep -q '^end' $w/probe.txt

# The live disk (the USB stick the image started from) is refused.
live=$(awk -F'\t' '$1 == "disk" && $8 == 1 { print $3 " " $2 " " $5 " " $6 " " $12; exit }' $w/probe.txt)
echo "LIVE $live"
printf '%s\n' "install disk $live whole 536870912 0 units 1 base $people" > $w/live.req
session $w/live.req $w/live.out
step live-refused grep -q '^FAIL check .* Device or resource busy' $w/live.out

# A whole disk ($1 sector bytes, $2 sectors, $3 swap bytes, $4 a name, the rest the units).
install_whole() {
	wb=$1; ws=$2; wswap=$3; wname=$4; shift 4
	printf '%s\n' "install $(disk_words "$wb" "$ws") whole 536870912 $wswap units $# $* $people" > $w/$wname.req
	session $w/$wname.req $w/$wname.out
	wstatus=$?
	grep -E '^(STEP|STATE|FAIL|DONE)' $w/$wname.out | sed "s/^/OUT $wname /"
	return $wstatus
}
step install-nvme install_whole 512 16777216 1073741824 nvme base desktop firmware network-openssh lang-clang
probe
step install-sata install_whole 512 18874368 0 sata base network-openssh
probe
step install-4k install_whole 4096 2097152 1073741824 4k base network-openssh
probe

# The USB disk: an empty GPT, then the editor's two creations, then the install on them.
reg=$(field 512 20971520 3)
usb=$(field 512 20971520 2)
echo "USB $usb $reg"
step usb-gpt sh -c "echo 'ERASE $usb:$reg' | diskpart init $usb 4a5b6c7d-8e9f-4a0b-9c1d-2e3f4a5b6c7d"
probe
printf '%s\n' "edit create-with-esp $(disk_words 512 20971520) esp 2048 1048576 new 1050624 12582912" > $w/edit1.req
step usb-create-esp session $w/edit1.req $w/edit1.out
grep -E '^(STATE|FAIL)' $w/edit1.out | sed 's/^/OUT edit1 /'
esp_uuid=$(awk '$1 == "STATE" && $2 == "created" && $3 == "esp" { print $6 }' $w/edit1.out)
root_uuid=$(awk '$1 == "STATE" && $2 == "created" && $3 == "part" { print $6 }' $w/edit1.out)
probe
printf '%s\n' "edit create $(disk_words 512 20971520) new 13633536 2097152" > $w/edit2.req
step usb-create session $w/edit2.req $w/edit2.out
grep -E '^(STATE|FAIL)' $w/edit2.out | sed 's/^/OUT edit2 /'
swap_uuid=$(awk '$1 == "STATE" && $2 == "created" && $3 == "part" { print $6 }' $w/edit2.out)
probe
grep -E "^part	$reg	" $w/probe.txt | sed 's/^/PROBE /'
printf '%s\n' "install $(disk_words 512 20971520) chosen esp $reg:2048:1048576:$esp_uuid root $reg:1050624:12582912:$root_uuid swap $reg:13633536:2097152:$swap_uuid units 1 base $people" > $w/usb.req
session $w/usb.req $w/usb.out
step install-usb grep -q '^DONE' $w/usb.out
grep -E '^(STEP|STATE|FAIL|DONE)' $w/usb.out | sed 's/^/OUT usb /'
probe
grep -E '^(disk|part)' $w/probe.txt | sed 's/^/PROBE /'

# An installed system read back ($1 sector bytes, $2 sectors, $3 a name).
check_installed() {
	b=$1; s=$2; name=$3
	r=$(field "$b" "$s" 3)
	root=$(part_device "$r" $ROOT)
	esp=$(part_device "$r" $ESP)
	echo "INSTALLED $name root=$root esp=$esp"
	step "$name-mount-root" mount -t ufs /dev/$root /mnt/p009r
	step "$name-passwd" grep -q '^tester:x:1000:1000:P009 Tester:/home/tester:' /mnt/p009r/etc/passwd
	step "$name-no-user" sh -c "! grep -q '^user:' /mnt/p009r/etc/passwd"
	step "$name-shadow" sh -c "grep '^tester:' /mnt/p009r/etc/shadow | grep -q '^tester:\\\$'"
	step "$name-root-hash" sh -c "test \"\$(grep '^root:' /mnt/p009r/etc/shadow)\" != \"\$(grep '^root:' /etc/shadow)\""
	step "$name-group" grep -q 'tester' /mnt/p009r/etc/group
	step "$name-no-old-home" sh -c "! test -e /mnt/p009r/home/user"
	step "$name-hostname" grep -q 'p009host' /mnt/p009r/etc/rc.conf
	step "$name-kernel" cmp /mnt/p009r/boot/vmunix /opt/installer/boot/vmunix
	step "$name-logo" cmp /mnt/p009r/boot/logo.ppm /opt/installer/boot/logo.ppm
	step "$name-record" grep -q '^base	' /mnt/p009r/var/db/installer/units
	step "$name-files" test -s /mnt/p009r/var/db/installer/base.files
	if [ -f /mnt/p009r/etc/ssh/sshd_config ]; then
		step "$name-sshd" grep -q '^PermitRootLogin no' /mnt/p009r/etc/ssh/sshd_config
	fi
	echo "UNITS $name $(cut -f1 /mnt/p009r/var/db/installer/units | tr '\n' ' ')"
	echo "FSTAB $name $(grep swap /mnt/p009r/etc/fstab | tr '\n' ' ')"
	ls -l /mnt/p009r/swapfile 2>/dev/null | sed "s/^/SWAPFILE $name /"
	ls -l /mnt/p009r/usr/libexec/install-admin 2>/dev/null | sed "s/^/LEFT $name /"
	step "$name-mount-esp" mount -t fat -r /dev/$esp /mnt/p009e
	step "$name-loader" cmp /mnt/p009e/EFI/BOOT/BOOTX64.EFI /opt/installer/boot/BOOTX64.EFI
	sed "s/^/CFG $name /" /mnt/p009e/zedbsd.cfg
	step "$name-umount-esp" umount /mnt/p009e
}
# The harness's keys and network on an installed root still mounted at /mnt/p009r.
harness() {
	mkdir -p /mnt/p009r/home/tester/.ssh &&
	cp /root/.ssh/authorized_keys /mnt/p009r/home/tester/.ssh/authorized_keys &&
	chmod 700 /mnt/p009r/home/tester /mnt/p009r/home/tester/.ssh && chmod 600 /mnt/p009r/home/tester/.ssh/authorized_keys &&
	chown -R 1000:1000 /mnt/p009r/home/tester &&
	cp /etc/net.conf /mnt/p009r/etc/net.conf &&
	cp /etc/ssh/ssh_host_*_key /etc/ssh/ssh_host_*_key.pub /mnt/p009r/etc/ssh/ &&
	chmod 600 /mnt/p009r/etc/ssh/ssh_host_*_key
}
check_installed 512 16777216 nvme
step nvme-harness harness
step nvme-umount umount /mnt/p009r
check_installed 4096 2097152 4k
step 4k-umount umount /mnt/p009r
check_installed 512 20971520 usb
step usb-umount umount /mnt/p009r
check_installed 512 18874368 sata
step sata-harness harness
step sata-umount umount /mnt/p009r

# The rule for a new kernel (spec 5.10): written under a new name, renamed, synced; the root stays mounted.
r=$(field 512 18874368 3)
root=$(part_device "$r" $ROOT)
start=$(awk -F'\t' -v r="$r" -v t="$ROOT" '$1 == "part" && $2 == r && toupper($6) == t { print $4; exit }' $w/probe.txt)
echo "SYNCROOT $root $start"
step sync-mount mount -t ufs /dev/$root /mnt/p009r
step sync-write dd if=/dev/urandom of=/mnt/p009r/boot/p009-sync.new bs=4096 count=700
step sync-rename mv /mnt/p009r/boot/p009-sync.new /mnt/p009r/boot/p009-sync.bin
step sync-sync sync
echo "SYNCSUM /boot/p009-sync.bin $(cksum < /mnt/p009r/boot/p009-sync.bin)"
echo "P009 DONE"

#!/bin/sh
# Runs in the guest as root, through plan/ws307/tests/p007-install-entry.sh.  MODE is one of
#   install   in the installer's live system, with one empty NVMe disk of 8 GiB: a GPT with one partition on the NVMe
#             disk, as an earlier installation leaves it; boot entries made on it (OLD, labelled zedBSD, and OTHER,
#             labelled Other) and on the live disk's ESP (KEEP, labelled zedBSD); a whole-disk install through
#             install-admin, which must make the new entry (STATE boot-entry NEW) at the end of BootOrder for the new
#             ESP and prune OLD (its partition is gone) but keep OTHER (another label) and KEEP (its partition is on
#             the live disk); BootNext set to NEW and the harness's root key, net.conf and host keys put on the
#             installed root
#   booted NEW
#             in the installed system, booted alone on the same variable store: the firmware started it from NEW
#             (BootCurrent), BootNext is used up; efiboot prune keeps NEW.  (OVMF itself drops the entries of
#             devices QEMU no longer has, OTHER and KEEP among them, when it boots with QEMU's boot order.)
# Each check prints "STEP name ok|fail"; the entries are printed as "ENTRY name XXXX"; the last line is
# "P007I DONE MODE".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
mode=$1
w=/tmp/p007i
mkdir -p $w
step() { sname=$1; shift; if "$@" > $w/step.txt 2>&1; then echo "STEP $sname ok"; else echo "STEP $sname fail"; sed 's/^/  /' $w/step.txt; fi; }
listed() { efiboot list > $w/list.txt 2>&1; sed "s/^/LIST$1 /" $w/list.txt; }
order_has() { grep '^BootOrder:' $w/list.txt | grep -Eq "[ ,]$1(,|\$)"; }
order_last() { grep '^BootOrder:' $w/list.txt | grep -Eq "[ ,]$1\$"; }
ESP=c12a7328-f81f-11d2-ba4b-00a0c93ec93b

case $mode in
install)
	admin=/usr/libexec/install-admin
	f() { printf '%d:%s' "${#1}" "$1"; }
	$admin probe > $w/probe.txt 2> $w/probe.err
	grep -E '^disk' $w/probe.txt | sed 's/^/PROBE /'
	field() { awk -F'\t' -v b="$1" -v s="$2" -v c="$3" '$1 == "disk" && $5 == b && $6 == s { print $c; exit }' $w/probe.txt; }
	dev=$(field 512 16777216 2)
	reg=$(field 512 16777216 3)
	live=$(awk -F'\t' '$1 == "disk" && $8 == 1 { print $2; exit }' $w/probe.txt)
	echo "DISKS target=$dev:$reg live=$live"

	# The earlier installation's table: one ESP-typed partition of 64 MiB at 1 MiB.
	step old-gpt sh -c "echo 'ERASE $dev:$reg' | diskpart init $dev 0d7e0001-0000-4000-8000-000000000001 2048 131072 $ESP 0d7e0002-0000-4000-8000-000000000002 old"
	old=$(efiboot create -l zedBSD "$dev" 1 '\EFI\OLD\BOOTX64.EFI' 2> $w/create-old.err)
	other=$(efiboot create -l Other "$dev" 1 '\EFI\OTHER\BOOTX64.EFI' 2> $w/create-other.err)
	live_slot=$(diskpart --machine show "$live" | awk -F'\t' -v e=$ESP '$1 == "partition" && tolower($5) == e { print $2; exit }')
	keep=$(efiboot create -l zedBSD "$live" "$live_slot" '\EFI\BOOT\BOOTX64.EFI' 2> $w/create-keep.err)
	old=${old#Boot}; other=${other#Boot}; keep=${keep#Boot}
	echo "ENTRY old $old"
	echo "ENTRY other $other"
	echo "ENTRY keep $keep"
	cat $w/create-*.err | sed 's/^/CREATE /'
	listed 0
	step entries-made test -n "$old" -a -n "$other" -a -n "$keep"
	step old-listed grep -q "^Boot$old\\* zedBSD	HD(1,GPT,0d7e0002-0000-4000-8000-000000000002," $w/list.txt

	# The install, whole disk: the base and OpenSSH, a swap partition.  The new GPT replaces the old one.
	(
		$admin probe > $w/probe.txt 2> $w/probe.err
		people="user $(f tester) fullname $(f 'P007 Tester') password $(f pw-tester-7) root-password $(f pw-root-7) autologin no hostname $(f p007host)"
		disk="disk $(field 512 16777216 3) $(field 512 16777216 2) 512 16777216 $(field 512 16777216 12)"
		printf '%s\n' "install $disk whole 536870912 1073741824 units 2 base network-openssh $people"
	) > $w/install.req
	: > $w/install.out
	{
		cat $w/install.req
		n=0
		while ! grep -Eq '^(DONE|FAIL)' $w/install.out && [ $n -lt 3000 ]; do sleep 1; n=$((n + 1)); done
	} | $admin session > $w/install.out 2> $w/install.err
	grep -E '^(STATE|FAIL|DONE)' $w/install.out | sed 's/^/OUT /'
	sed 's/^/ERR /' $w/install.err
	step install grep -q '^DONE' $w/install.out
	new=$(awk '$1 == "STATE" && $2 == "boot-entry" { print $3; exit }' $w/install.out)
	echo "ENTRY new $new"
	step entry-reported sh -c "test -n '$new' && test '$new' != none"

	# The new entry: the system's name, the new ESP by its slot and PARTUUID, the loader, the end of the order.
	listed 1
	diskpart --machine show "$dev" | sed 's/^/TABLE /'
	esp=$(diskpart --machine show "$dev" | awk -F'\t' -v e=$ESP '$1 == "partition" && tolower($5) == e { print $2 " " $6; exit }')
	echo "ESP $esp"
	step new-entry grep -q "^Boot$new\\* zedBSD	HD(${esp% *},GPT,${esp#* }," $w/list.txt
	step new-loader sh -c "grep '^Boot$new' $w/list.txt | grep -qF 'File(\\EFI\\BOOT\\BOOTX64.EFI)'"
	step new-order-last order_last "$new"

	# The prune: OLD went with its partition; OTHER (another label) and KEEP (on the live disk) stay.
	step old-pruned sh -c "! grep -q '^Boot$old' $w/list.txt"
	step old-out-of-order sh -c "! { grep '^BootOrder:' $w/list.txt | grep -Eq '[ ,]$old(,|\$)'; }"
	step other-kept grep -q "^Boot$other\\* Other" $w/list.txt
	step other-in-order order_has "$other"
	step keep-kept grep -q "^Boot$keep\\* zedBSD" $w/list.txt
	step prune-again-none sh -c "test -z \"\$(efiboot prune)\""

	# The next boot takes the new entry once; the installed root gets the harness's way in.
	step next efiboot next "$new"
	mkdir -p /mnt/p007r
	root=$(diskpart --machine show "$dev" | awk -F'\t' '$1 == "partition" && toupper($5) == "516E7CB6-6ECF-11D6-8FF8-00022D09712B" { print $2; exit }')
	case $dev in *[0-9]) rootdev=${dev}p$root ;; *) rootdev=$dev$root ;; esac
	echo "ROOT $rootdev"
	step mount-root mount -t ufs /dev/$rootdev /mnt/p007r
	harness() {
		mkdir -p /mnt/p007r/root/.ssh &&
		cp /root/.ssh/authorized_keys /mnt/p007r/root/.ssh/authorized_keys &&
		chmod 700 /mnt/p007r/root/.ssh && chmod 600 /mnt/p007r/root/.ssh/authorized_keys &&
		sed 's/^PermitRootLogin no$/PermitRootLogin prohibit-password/' /mnt/p007r/etc/ssh/sshd_config > $w/sshd_config &&
		cp $w/sshd_config /mnt/p007r/etc/ssh/sshd_config &&
		grep -q '^PermitRootLogin prohibit-password' /mnt/p007r/etc/ssh/sshd_config &&
		cp /etc/net.conf /mnt/p007r/etc/net.conf &&
		cp /etc/ssh/ssh_host_*_key /etc/ssh/ssh_host_*_key.pub /mnt/p007r/etc/ssh/ &&
		chmod 600 /mnt/p007r/etc/ssh/ssh_host_*_key
	}
	step harness harness
	step umount-root umount /mnt/p007r
	sync
	;;
booted)
	new=$2
	listed 2
	step booted-from-new grep -q "^BootCurrent: $new\$" $w/list.txt
	step next-used-up sh -c "! grep -q '^BootNext:' $w/list.txt"
	efiboot prune > $w/pruned.txt 2>&1
	sed 's/^/PRUNED /' $w/pruned.txt
	step prune-keeps-new test ! -s $w/pruned.txt
	listed 3
	step new-stays grep -q "^Boot$new\\* zedBSD" $w/list.txt
	step new-in-order order_has "$new"
	sync
	;;
esac
echo "P007I DONE $mode"

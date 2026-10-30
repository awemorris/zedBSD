#!/bin/sh
# Runs in the guest (as root, through plan/ws307/tests/p007-efiboot.sh).  MODE is one of
#   create          lists the entries, makes a zedBSD entry for \EFI\BOOT\BOOTX64.EFI on the boot disk's ESP with
#                   BootNext set to it, reads it back, and makes it again (the same number must come back)
#   after NUMBER    after the restart: the firmware booted from entry NUMBER (BootCurrent), the entry is still
#                   there, BootNext was used up; then deletes the entry and reads that back
#   gone NUMBER     after another restart: the deleted entry stayed deleted
#   nouefi          the kernel has no runtime services (a BIOS boot, an older loader): efiboot answers that the
#                   firmware's variables are not supported
# Each check prints "STEP name ok|fail"; the last line is "P007 DONE MODE".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
mode=$1
entry=${2:-}
step() { name=$1; shift; if "$@" > /tmp/p007-step.txt 2>&1; then echo "STEP $name ok"; else echo "STEP $name fail"; sed 's/^/  /' /tmp/p007-step.txt; fi; }
efiboot list > /tmp/p007-list.txt 2>&1
listed=$?
sed 's/^/LIST /' /tmp/p007-list.txt
dmesg 2>/dev/null | grep 'uefi:' | sed 's/^/KLOG /'
case $mode in
create)
	step list test $listed = 0
	slot=$(diskpart --machine show nvme0n1 | awk -F'\t' '$1 == "partition" && tolower($5) == "c12a7328-f81f-11d2-ba4b-00a0c93ec93b" { print $2; exit }')
	echo "ESP $slot"
	made=$(efiboot create -n nvme0n1 "$slot" '\EFI\BOOT\BOOTX64.EFI' 2> /tmp/p007-create.txt)
	sed 's/^/CREATE /' /tmp/p007-create.txt
	number=${made#Boot}
	echo "ENTRY $number"
	efiboot list > /tmp/p007-list2.txt 2>&1
	sed 's/^/LIST2 /' /tmp/p007-list2.txt
	step entry-made test -n "$number"
	step entry-read-back grep -q "^Boot$number\\* zedBSD	HD($slot,GPT," /tmp/p007-list2.txt
	step entry-loader grep -qF 'File(\EFI\BOOT\BOOTX64.EFI)' /tmp/p007-list2.txt
	step order-last sh -c "grep '^BootOrder:' /tmp/p007-list2.txt | grep -Eq '[ ,]$number\$'"
	step next-set grep -q "^BootNext: $number\$" /tmp/p007-list2.txt
	step same-again test "$(efiboot create nvme0n1 "$slot" '\EFI\BOOT\BOOTX64.EFI')" = "Boot$number"
	sync
	;;
after)
	step booted-from-entry grep -q "^BootCurrent: $entry\$" /tmp/p007-list.txt
	step entry-kept grep -q "^Boot$entry\\* zedBSD" /tmp/p007-list.txt
	step next-used-up sh -c "! grep -q '^BootNext:' /tmp/p007-list.txt"
	step delete efiboot delete "$entry"
	efiboot list > /tmp/p007-list3.txt 2>&1
	sed 's/^/LIST3 /' /tmp/p007-list3.txt
	step deleted sh -c "! grep -q '^Boot$entry' /tmp/p007-list3.txt"
	step order-without sh -c "! grep '^BootOrder:' /tmp/p007-list3.txt | grep -q '$entry'"
	sync
	;;
gone)
	step list test $listed = 0
	step still-deleted sh -c "! grep -q '^Boot$entry' /tmp/p007-list.txt"
	;;
nouefi)
	step list-refused test $listed != 0
	step not-supported grep -qi 'not supported' /tmp/p007-list.txt
	;;
esac
echo "P007 DONE $mode"

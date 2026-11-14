#!/bin/sh
# Brings the running Raspberry Pi 5's kernel and root up to a new build without rewriting its disk:
# run from the top of the tree, after "make CONFIGMK=config/config.rpi5.mk vmunix rootfs".
#   PI5=ADDRESS sh plan/tools/pi5-update.sh plan ROOTFS OUTDIR
#       the board's cksums (OUTDIR/board.cksum) against ROOTFS: OUTDIR/overlay.txt gets the files the board
#       lacks or has different, OUTDIR/kept.txt the different ones left alone because they are the board's own
#       state (accounts, keys, network, settings, /home, /var), OUTDIR/links.txt the symbolic links to make.
#       No change on the board.
#   PI5=ADDRESS sh plan/tools/pi5-update.sh deploy ROOTFS OUTDIR VMUNIX
#       after plan: the board's copies of the overlay files to OUTDIR/backup, /boot/boot0/vmunix to
#       /boot/boot0/vmunix.old and VMUNIX in its place (read back), the overlay files staged in the board's /tmp
#       with scp and put in place by root (a copy beside, the tree's mode, a rename over), the links made, then the
#       cksums checked
#       (OUTDIR/after.cksum).  No reboot.
#   PI5=ADDRESS sh plan/tools/pi5-update.sh reboot
#   PI5=ADDRESS sh plan/tools/pi5-update.sh check VMUNIX
#       uname, the kernel's cksum against VMUNIX, the session processes and the end of dmesg.
# cmdline.txt, config.txt and the dtbs are not touched.  The board's user is user/user (sudo).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
step=${1:?usage: pi5-update.sh plan|deploy|reboot|check ...}
: "${PI5:?PI5=the board's address}"
PW=${PI5_PASSWORD:-user}
SO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o PubkeyAuthentication=no"

b() {
	timeout "${BT:-120}" sshpass -p "$PW" ssh $SO "user@$PI5" "$@"
}

sb() {
	b "echo $PW | sudo -S sh -c '$1' 2> /dev/null"
}

# The board's own state: never replaced, only listed.
own() {
	case $1 in
	./.stamp|./home/*|./var/*|./root/*|./boot/*|./tmp/*|./run/*) return 0 ;;
	./etc/passwd|./etc/master.passwd|./etc/group|./etc/shadow|./etc/*.db|./etc/passkey) return 0 ;;
	./etc/ssh/ssh_host_*|./etc/net.conf|./etc/rc.conf|./etc/hosts|./etc/resolv.conf|./etc/fstab) return 0 ;;
	./etc/zedbsd-root|./etc/thermal.conf|./etc/keiland/autologin|./etc/keiland/apps.conf) return 0 ;;
	esac
	return 1
}

board_sums() {
	BT=600 sb 'cd / && find . -xdev -type f -print | sort | xargs cksum'
}

board_up() {
	b 'uname -s' 2> /dev/null | grep -q zedBSD
}

wait_down_up() {
	n=0
	while [ $n -lt 24 ]; do
		sleep 5
		BT=10 b 'uname -s' > /dev/null 2>&1 || break
		n=$((n + 1))
	done
	n=0
	while [ $n -lt 48 ]; do
		sleep 5
		BT=15 board_up && return 0
		n=$((n + 1))
	done
	return 1
}

case $step in
plan)
	root=${2:?ROOTFS}
	out=${3:?OUTDIR}
	board_up || { echo "FAIL $PI5 is not zedBSD"; exit 1; }
	mkdir -p "$out"
	board_sums > "$out/board.cksum" || { echo "FAIL board cksums"; exit 1; }
	awk '{ s = $3; for (i = 4; i <= NF; i++) s = s " " $i; print s "\t" $1 " " $2 }' "$out/board.cksum" |
	    sort > "$out/board.map"
	: > "$out/overlay.txt"
	: > "$out/kept.txt"
	: > "$out/links.txt"
	same=0
	(cd "$root" && find . -type f | sort) > "$out/tree.txt"
	while read -r f; do
		sum=$(cksum < "$root/$f" | awk '{ print $1 " " $2 }')
		was=$(awk -F '\t' -v f="$f" '$1 == f { print $2 }' "$out/board.map")
		if [ "$was" = "$sum" ]; then
			same=$((same + 1))
			continue
		fi
		if own "$f"; then
			echo "$f${was:+ (board differs)}${was:- (board lacks)}" >> "$out/kept.txt"
			continue
		fi
		echo "$f" >> "$out/overlay.txt"
	done < "$out/tree.txt"
	(cd "$root" && find . -type l | sort) | while read -r f; do
		own "$f" && continue
		echo "$f $(readlink "$root/$f")" >> "$out/links.txt"
	done
	echo "tree files: $(wc -l < "$out/tree.txt"), same: $same, to replace or add: $(wc -l < "$out/overlay.txt")," \
	    "kept: $(wc -l < "$out/kept.txt"), links: $(wc -l < "$out/links.txt")"
	;;
deploy)
	root=${2:?ROOTFS}
	out=${3:?OUTDIR}
	kernel=${4:?VMUNIX}
	test -s "$out/overlay.txt" || { echo "FAIL run plan first"; exit 1; }
	board_up || { echo "FAIL $PI5 is not zedBSD"; exit 1; }
	stamp=$(date +%s)
	remote=/tmp/pi5-update.$stamp
	# Backups of what is replaced, on the host.
	mkdir -p "$out/backup"
	sed 's#^\./##' "$out/overlay.txt" > "$out/overlay.rel"
	{
		echo "cd / && mkdir -p $remote.bak || exit 1"
		echo 'while read -r f; do'
		echo "	if [ -f \"\$f\" ]; then mkdir -p \"$remote.bak/\$(dirname \"\$f\")\" && cp -p \"\$f\" \"$remote.bak/\$f\"; fi"
		echo "done < $remote.list"
		echo "chown -R user $remote.bak && chmod -R u+rwX $remote.bak"
	} > "$out/backup.sh"
	timeout 60 sshpass -p "$PW" scp $SO -q "$out/overlay.rel" "user@$PI5:$remote.list" || exit 1
	timeout 60 sshpass -p "$PW" scp $SO -q "$out/backup.sh" "user@$PI5:$remote.backup.sh" || exit 1
	BT=600 sb "sh $remote.backup.sh" || { echo "FAIL backup on the board"; exit 1; }
	timeout 600 sshpass -p "$PW" scp $SO -q -rp "user@$PI5:$remote.bak/." "$out/backup/" || { echo "FAIL backup"; exit 1; }
	echo "backed up: $(cd "$out/backup" && find . -type f | wc -l) files in $out/backup (board: $remote.bak)"
	# The kernel, the old one kept as vmunix.old.
	want=$(cksum < "$kernel" | cut -d' ' -f1-2)
	timeout 300 sshpass -p "$PW" scp $SO -q "$kernel" "user@$PI5:$remote.vmunix" || { echo "FAIL kernel copy"; exit 1; }
	sb "cd /boot/boot0 && cp vmunix vmunix.old && cp $remote.vmunix vmunix && sync" || { echo "FAIL kernel install"; exit 1; }
	got=$(b 'cksum < /boot/boot0/vmunix' | cut -d' ' -f1-2)
	[ "$got" = "$want" ] || { echo "FAIL kernel read back $got, want $want"; exit 1; }
	echo "kernel: /boot/boot0/vmunix $got (old: $(b 'cksum < /boot/boot0/vmunix.old' | cut -d' ' -f1-2))"
	# The userland files, staged owner-writable.  Root copies each next to its place with the tree's mode and
	# renames it over the old one, so the running programs keep the old file (libc.so among them) until the reboot.
	stage=$out/stage.$stamp
	mkdir -p "$stage"
	printf '#!/bin/sh\nset -e\n' > "$out/install.sh"
	while read -r f; do
		mkdir -p "$stage/$(dirname "$f")"
		cp -p "$root/$f" "$stage/$f"
		chmod u+rw "$stage/$f"
		printf 'mkdir -p /%s; cp %s/%s /%s.pi5new; chown root:wheel /%s.pi5new; chmod %s /%s.pi5new; mv -f /%s.pi5new /%s\n' \
		    "$(dirname "$f")" "$remote" "$f" "$f" "$f" "$(stat -c %a "$root/$f")" "$f" "$f" "$f" >> "$out/install.sh"
	done < "$out/overlay.rel"
	while read -r f t; do
		f=${f#./}
		printf 'mkdir -p /%s; ln -sfn %s /%s\n' "$(dirname "$f")" "$t" "$f" >> "$out/install.sh"
	done < "$out/links.txt"
	timeout 900 sshpass -p "$PW" scp $SO -q -rp "$stage" "user@$PI5:$remote" || { echo "FAIL stage copy"; exit 1; }
	timeout 60 sshpass -p "$PW" scp $SO -q "$out/install.sh" "user@$PI5:$remote.install.sh" || exit 1
	BT=600 sb "sh $remote.install.sh && sync" || { echo "FAIL install"; exit 1; }
	echo "installed $(wc -l < "$out/overlay.rel") files from $remote"
	board_sums > "$out/after.cksum"
	bad=0
	while read -r f; do
		sum=$(cksum < "$root/$f" | awk '{ print $1 " " $2 }')
		grep -q "^$sum \./$f\$" "$out/after.cksum" || { echo "differs after install: $f"; bad=$((bad + 1)); }
	done < "$out/overlay.rel"
	echo "after install: $bad of $(wc -l < "$out/overlay.rel") differ"
	;;
reboot)
	board_up || { echo "FAIL $PI5 is not zedBSD"; exit 1; }
	sb 'sync; (sleep 1; /sbin/reboot) > /dev/null 2>&1 &' > /dev/null 2>&1
	wait_down_up || { echo "FAIL the board did not come back"; exit 1; }
	echo "up: $(b 'uname -a')"
	;;
check)
	kernel=${2:?VMUNIX}
	board_up || { echo "FAIL $PI5 is not zedBSD"; exit 1; }
	echo "uname: $(b 'uname -a')"
	echo "kernel: board $(b 'cksum < /boot/boot0/vmunix' | cut -d' ' -f1-2), build $(cksum < "$kernel" | cut -d' ' -f1-2)"
	echo "date: $(b date)"
	b 'ps -ef'
	echo "--- dmesg (panic, fault, trap, abort, error)"
	sb 'dmesg' | grep -inE 'panic|fault|trap|abort|error|fail' | tail -40
	echo "--- dmesg tail"
	sb 'dmesg' | tail -20
	;;
*)
	echo "usage: pi5-update.sh plan|deploy|reboot|check ..."
	exit 1
	;;
esac
sync

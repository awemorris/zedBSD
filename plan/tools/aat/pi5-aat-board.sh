#!/bin/sh
# The Raspberry Pi 5 made ready for plan/tools/aat/pi5-record.py, and put back: run from the top of the tree.
#   PI5=ADDRESS sh plan/tools/aat/pi5-aat-board.sh check            what the board has (cksums), no change
#   PI5=ADDRESS KERNEL=VMUNIX sh plan/tools/aat/pi5-aat-board.sh deploy [FORCE_HOTPLUG]
#       keeps vmunix, cmdline.txt and config.txt once as NAME.aatold, puts KERNEL (a build of
#       plan/tools/aat/config-rpi5-aat.mk) as /boot/boot0/vmunix, with FORCE_HOTPLUG=1 adds hdmi_force_hotplug=1 (and
#       HDMI_MODE=N: hdmi_group=1 hdmi_mode=N) to config.txt's [all], trusts the guest harness's key for the board's
#       user, and reboots
#   PI5=ADDRESS sh plan/tools/aat/pi5-aat-board.sh files            after a boot: aat-input, mview, its model and the
#       sample video into /tmp/aat-rec on the board (BUILD=build/q1898 by default; SAMPLE the video)
#   PI5=ADDRESS sh plan/tools/aat/pi5-aat-board.sh reboot
#   PI5=ADDRESS sh plan/tools/aat/pi5-aat-board.sh restore          the kept files back, a boot, and the cksums
#       compared with the ones deploy saw (RESTORED OK).  The user's ~/.ssh/authorized_keys that deploy wrote stays:
#       it is named at the end, for whoever cleans the board.
# The board's user is user/user (sudo).  The cksums deploy saw are in build/aat/pi5-board.txt.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
step=${1:?usage: pi5-aat-board.sh check|deploy|files|reboot|restore}
: "${PI5:?PI5=the board's address}"
PW=${AAT_PASSWORD:-user}
BUILD=${BUILD:-build/q1898}
STATE=build/aat/pi5-board.txt
SO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o PubkeyAuthentication=no"
FILES="vmunix cmdline.txt config.txt"

b() {
	timeout "${BT:-60}" sshpass -p "$PW" ssh $SO "user@$PI5" "$@"
}

sb() {
	b "echo $PW | sudo -S sh -c '$1' 2> /dev/null"
}

put() {
	timeout 300 sshpass -p "$PW" scp $SO -q -r "$1" "user@$PI5:$2"
}

sums() {
	for f in $FILES; do
		printf '%s=%s ' "$f" "$(b "cksum < /boot/boot0/$f" | cut -d' ' -f1)"
	done
	echo
}

wait_down_up() {
	n=0
	while [ $n -lt 24 ]; do
		sleep 5
		b 'uname -s' > /dev/null 2>&1 || break
		n=$((n + 1))
	done
	n=0
	while [ $n -lt 48 ]; do
		sleep 5
		if b 'uname -s' 2> /dev/null | grep -q zedBSD; then
			return 0
		fi
		n=$((n + 1))
	done
	return 1
}

reboot_board() {
	sb 'sync; (sleep 1; /sbin/reboot) > /dev/null 2>&1 &' > /dev/null 2>&1
	wait_down_up || { echo "FAIL the board did not come back"; exit 1; }
	echo "up $(b 'uname -v')"
}

case $(b 'uname -s' 2>&1) in
zedBSD*) ;;
*) echo "FAIL $PI5 is not zedBSD"; exit 1 ;;
esac

case $step in
check)
	echo "board $(sums)"
	b 'ls /boot/boot0/ | grep aatold'
	;;
deploy)
	: "${KERNEL:?KERNEL=the kernel}"
	mkdir -p "$(dirname "$STATE")"
	if b 'ls /boot/boot0/ | grep -q aatold'; then
		echo "kept copies are there already (an earlier deploy): $(sums)"
	else
		echo "original $(sums)" | tee "$STATE"
		sb 'cd /boot/boot0 && cp vmunix vmunix.aatold && cp cmdline.txt cmdline.txt.aatold && cp config.txt config.txt.aatold && sync' ||
		    { echo "FAIL the copies"; exit 1; }
	fi
	want=$(cksum < "$KERNEL" | cut -d' ' -f1)
	put "$KERNEL" /tmp/vmunix.aat || { echo "FAIL copy"; exit 1; }
	sb "test \"\$(cksum < /tmp/vmunix.aat | cut -d\" \" -f1)\" = $want && cp /tmp/vmunix.aat /boot/boot0/vmunix && sync" ||
	    { echo "FAIL install"; exit 1; }
	if [ "${2:-0}" = 1 ]; then
		extra="hdmi_force_hotplug=1"
		[ -n "${HDMI_MODE:-}" ] && extra="$extra
hdmi_group=1
hdmi_mode=$HDMI_MODE"
		printf '%s\n' "$extra" > "$BUILD/hdmi.txt"
		put "$BUILD/hdmi.txt" /tmp/hdmi.txt
		sb 'cat /boot/boot0/config.txt.aatold /tmp/hdmi.txt > /boot/boot0/config.txt && sync'
		echo "config.txt: $(b 'tail -4 /boot/boot0/config.txt' | tr '\n' ' ')"
	fi
	b "mkdir -p .ssh && chmod 700 .ssh && cat > .ssh/authorized_keys && chmod 600 .ssh/authorized_keys" < plan/tmp/guest/id_ed25519.pub
	echo "deployed $(sums)"
	reboot_board
	;;
files)
	b 'mkdir -p /tmp/aat-rec'
	put "$BUILD/bin/aat-input" /tmp/aat-rec/aat-input &&
	    put "$BUILD/bin/mview" /tmp/aat-rec/mview &&
	    put userland/tests/mview/models/qs40 /tmp/aat-rec/ &&
	    put "${SAMPLE:-$BUILD/rec/sample.mp4}" /tmp/aat-rec/sample.mp4 || { echo "FAIL files"; exit 1; }
	b 'chmod 755 /tmp/aat-rec /tmp/aat-rec/aat-input /tmp/aat-rec/mview; chmod -R a+rX /tmp/aat-rec; ls -la /tmp/aat-rec'
	;;
reboot)
	reboot_board
	;;
restore)
	sb 'cd /boot/boot0 && mv vmunix.aatold vmunix && mv cmdline.txt.aatold cmdline.txt && mv config.txt.aatold config.txt && sync' ||
	    { echo "FAIL restore"; exit 1; }
	reboot_board
	now=$(sums)
	was=$(sed -n 's/^original //p' "$STATE")
	echo "now $now"
	echo "was $was"
	if [ "$now" = "$was" ]; then echo "RESTORED OK"; else echo "FAIL restore mismatch"; fi
	b 'ls /boot/boot0/ | grep aatold; ls -la ~/.ssh'
	;;
esac
sync

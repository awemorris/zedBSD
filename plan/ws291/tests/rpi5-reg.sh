#!/bin/sh
# ws291-p027: one tree's libvulkan (with its kernel, compositor and libkeiland) on the Raspberry Pi 5, against the
# board's own programs, run from the top of the tree on the board booted from the measurement image:
#   [PI5=10.0.30.7] CAND=BUILD sh plan/ws291/tests/rpi5-reg.sh OUT
# CAND is the tree's build of plan/ws291/tests/config-rpi5-shot.mk (vmunix, bin/mview, bin/wayland, bin/aat-input and
# the dynamic libraries).  The board's files this changes (the kernel, cmdline.txt, /lib/libvulkan.so,
# /lib/libkeiland.so, /bin/wayland, /bin/aat-input, /etc/keiland/session, /root/p021/bin and /root/p021/lib) are kept
# on the board first (NAME.q1860old, /root/q1860save) with their checksums (OUT/before.txt).  Then, with CAND's kernel:
#   mview-a  the screen's mview (rpi5-mview.sh) with the programs the board had in /root/p021 (the base);
#   mview-b  the same with CAND's programs and libraries;
#   off-a    the offscreen mview (rpi5-offscreen.sh) with CAND's viewer and the board's /lib;
#   off-b    the same after CAND's libvulkan.so and libkeiland.so are put in /lib;
#   desk     the graphical login's desktop frames (rpi5-desk.sh, two boots) with CAND's compositor and /lib.
# At the end every kept file is put back, the board rebooted and the checksums compared with OUT/before.txt.
# OUT/summary.txt holds the verdict lines: the offscreen checksums against the expected ones, the frame rates, the
# kernel's GPU fault lines (the boot self-test's V9 lines left out) and libvulkan's failure lines.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rpi5-reg.sh OUT}
: "${CAND:?CAND=the tree's build}"
PI5=${PI5:-10.0.30.7}
export PI5
KEY=plan/tmp/guest/id_ed25519
SO="-i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o BatchMode=yes"
EXPECT="c1=0c6e08c7 c21=dd5f82af c62=90ab3e0f c74=224da3e7"
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

b() {
	timeout "${BT:-60}" ssh $SO "root@$PI5" "$@"
}

wait_up() {
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

FILES="/boot/boot0/vmunix /boot/boot0/cmdline.txt /lib/libvulkan.so /lib/libkeiland.so /bin/wayland /bin/aat-input /etc/keiland/session"

say "start $(date +%H:%M:%S) cand $(cksum < "$CAND/vmunix" | cut -d' ' -f1) libvulkan $(cksum < "$CAND/dynamic/libvulkan.so" | cut -d' ' -f1)"
case $(b 'uname -s' 2>&1) in
zedBSD*) ;;
*) say "FAIL $PI5 is not zedBSD"; exit 1 ;;
esac

# The board's state, kept on the board and listed.
b "for f in $FILES; do test -e \$f.q1860old || cp -p \$f \$f.q1860old; done; test -d /root/q1860save || { mkdir -p /root/q1860save && cp -Rp /root/p021/bin /root/p021/lib /root/q1860save/; }; sync" ||
    { say "FAIL the board's files were not kept"; exit 1; }
b "cksum $FILES /root/p021/bin/* /root/p021/lib/*" > "$out/before.txt" 2>&1
b "cat /boot/boot0/cmdline.txt" > "$out/before-cmdline.txt" 2>&1
say "before $(awk '{print $1, $3}' "$out/before.txt" | tr '\n' ' ')"

# The base: the board's own programs in /root/p021, read back for rpi5-mview.sh's PROGRAMS.
mkdir -p "$out/base/bin" "$out/base/dynamic"
timeout 300 scp $SO -q "root@$PI5:/root/q1860save/bin/wayland" "root@$PI5:/root/q1860save/bin/mview" "$out/base/bin/" &&
    timeout 300 scp $SO -q "root@$PI5:/root/q1860save/lib/*.so" "$out/base/dynamic/" ||
    { say "FAIL the base programs were not read back"; exit 1; }
say "base libvulkan $(cksum < "$out/base/dynamic/libvulkan.so" | cut -d' ' -f1) wayland $(cksum < "$out/base/bin/wayland" | cut -d' ' -f1)"

timeout 1500 env KERNEL="$CAND/vmunix" PROGRAMS="$out/base" COPIES='1 21 62 74' SPIN=12 MVIEW_ARGS=--model=/root/p021/qs40 \
    sh plan/ws291/tests/rpi5-mview.sh "$out/mview-a" > /dev/null 2>&1
timeout 1200 env KERNEL="$CAND/vmunix" PROGRAMS="$CAND" COPIES='1 21 62 74' SPIN=12 MVIEW_ARGS=--model=/root/p021/qs40 \
    sh plan/ws291/tests/rpi5-mview.sh "$out/mview-b" > /dev/null 2>&1
timeout 1800 env MVIEW="$CAND/bin/mview" TFU=1 COPIES='1 21 62 74' FRAMES=301 \
    sh plan/ws291/tests/rpi5-offscreen.sh "$out/off-a" > /dev/null 2>&1

# CAND's libvulkan and libkeiland in /lib (the originals are kept above).
for name in libvulkan.so libkeiland.so; do
	timeout 120 scp $SO -q "$CAND/dynamic/$name" "root@$PI5:/lib/$name.new" &&
	    b "test \"\$(cksum < /lib/$name.new | cut -d' ' -f1)\" = $(cksum < "$CAND/dynamic/$name" | cut -d' ' -f1) && chmod 644 /lib/$name.new && mv /lib/$name.new /lib/$name && sync" ||
	    say "FAIL /lib/$name was not put in"
done
say "lib $(b 'cksum /lib/libvulkan.so /lib/libkeiland.so' 2>&1 | awk '{print $1, $3}' | tr '\n' ' ')"
timeout 1800 env MVIEW="$CAND/bin/mview" TFU=1 COPIES='1 21 62 74' FRAMES=301 \
    sh plan/ws291/tests/rpi5-offscreen.sh "$out/off-b" > /dev/null 2>&1
timeout 2400 env CONSOLE=0 RUNS=2 KERNEL="$CAND/vmunix" AAT="$CAND/bin/aat-input" BINS="wayland=$CAND/bin/wayland" \
    sh plan/ws291/tests/rpi5-desk.sh "$out/desk" > /dev/null 2>&1

# The board's files back, the board rebooted on them and compared.
b "for f in $FILES; do cp -p \$f.q1860old \$f.back && mv \$f.back \$f; done; rm -rf /root/p021/bin /root/p021/lib && cp -Rp /root/q1860save/bin /root/q1860save/lib /root/p021/; sync" ||
    say "FAIL the board's files were not put back"
b 'sync; (sleep 1; /sbin/reboot) > /dev/null 2>&1 &'
sleep 20
wait_up || say "FAIL the board did not come back after the last reboot"
# SSH may drop once just after it first answers (the services still starting): up to three reads.
n=0
while [ $n -lt 3 ]; do
	sleep 15
	b "cksum $FILES /root/p021/bin/* /root/p021/lib/*" > "$out/after.txt" 2>&1
	b "cat /boot/boot0/cmdline.txt" > "$out/after-cmdline.txt" 2>&1
	[ -s "$out/after.txt" ] && break
	n=$((n + 1))
done
if cmp -s "$out/before.txt" "$out/after.txt" && cmp -s "$out/before-cmdline.txt" "$out/after-cmdline.txt"; then
	say "back same as before: $(awk '{print $1, $3}' "$out/after.txt" | tr '\n' ' ') cmdline $(cat "$out/after-cmdline.txt")"
	b "for f in $FILES; do rm -f \$f.q1860old; done; rm -f /bin/wayland.p021old /lib/libvulkan.so.p021old /lib/libkeiland.so.p021old; rm -rf /root/q1860save; sync"
else
	say "FAIL back differs from before (the kept copies stay on the board)"
	diff "$out/before.txt" "$out/after.txt" | tee -a "$summary"
fi
say "board now $(b 'uname -v; dmesg | grep -c "held; root starts it"' 2>&1 | tr '\n' ' ')"

# The verdict lines.
for arm in mview-a mview-b; do
	grep -E '^(programs|kernel vmunix|c[0-9]+ MVIEW|compositor)' "$out/$arm/summary.txt" | sed "s/^/$arm /" | tee -a "$summary"
done
for arm in off-a off-b; do
	for c in c1 c21 c62 c74; do
		want=$(echo "$EXPECT" | tr ' ' '\n' | sed -n "s/^$c=//p")
		got=$(sed -n 's/.*MVIEW SHOT run='"$c"' .*checksum=\([0-9a-f]*\).*/\1/p' "$out/$arm/$c.txt" 2>/dev/null)
		fps=$(sed -n 's/.*MVIEW OFFSCREEN run='"$c"' .* fps=\([0-9.]*\).*/\1/p' "$out/$arm/$c.txt" 2>/dev/null)
		verdict=FAIL
		[ "$got" = "$want" ] && verdict=same
		say "$arm $c fps=$fps checksum=${got:-none} expected=$want $verdict"
	done
done
grep -E '^desk[0-9]' "$out/desk/summary.txt" | grep -vE 'board-range|v3d: board' | sed 's/^/desk /' | tee -a "$summary"
grep -E '^(bin|lib) ' "$out/desk/summary.txt" | sed 's/^/desk /' | tee -a "$summary"
# The kernel's GPU fault lines of every step, the boot self-test (V9) left out.
for f in "$out"/mview-?/c*.kernel "$out"/off-?/c*.kernel; do
	[ -e "$f" ] || continue
	n=$(grep -iE 'fault|lost|hang|reset|job failed|error' "$f" | grep -vc 'V9')
	[ "$n" = 0 ] || say "kernel $f $n lines: $(grep -iE 'fault|lost|hang|reset|job failed|error' "$f" | grep -v V9 | head -2 | tr '\n' ' ')"
done
for d in "$out"/desk/desk?; do
	[ -e "$d/dmesg.txt" ] || continue
	say "kernel $d $(grep -iE 'fault|lost|hang|reset|job failed' "$d/dmesg.txt" | grep -vc 'V9') lines without the self-test"
done
# libvulkan's and the compositor's failure lines (a TLS mistake would end the program or fail here).
say "failures $(cat "$out"/mview-?/c*.txt "$out"/off-?/c*.txt "$out"/desk/desk?/session.log 2>/dev/null | grep -ciE 'device lost|handoff failed|VULKAN_ERROR|MVIEW FAILED|KWL FAILED|undefined symbol|relocation|Segmentation')"
say "end $(date +%H:%M:%S)"
sync

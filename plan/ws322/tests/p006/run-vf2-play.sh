#!/bin/sh
# ws322-p006's playback check on the VisionFive 2, in one run: chagall.bin put at the driver's path (when the board
# lacks it), BUILD's kernel (plan/ws322/tests/config-vf2-video-test.mk) booted, then two parts on that boot, each part's
# lines printed as soon as it ends (the H.264 part first: libmedia's Vulkan Video context is shared by the codecs):
#   A  H.264: media-probe --video-hash on P004/h264-1080p30.mp4 (600 frames) and on PLAY's JVT MP4s; Video Player on
#      the 1080p30 clip for 16 s in a compositor of its own (PRESENT, FRAMES, the screen's PNG 8 s in, top)
#   B  H.265: media-probe --video-hash on PLAY's H.265 MP4s (1080p30 20 s, 1080p60 10 s, two conformance streams);
#      Video Player on the 1080p30 and 1080p60 clips for 16 s each (PRESENT, FRAMES, the 1080p30 screen's PNG, top);
#      vkvideo-probe --expect on PLAY's Annex B streams (widths not a multiple of 16, three sizes in one stream) and
#      on HEVC's SAODBLK_A, SAODBLK_B and VPSSPSPPS_A (the last against PLAY's reference of the reordered copy)
# Each step with the engine's counts (/dev/wave5-test) and the kernel lines it added.  media-probe's hashes are
# compared on the host with the references, in order.  At the end the files put on the board are removed, the board's
# own kernel goes back (plan/tools/vf2/run.sh) and the board's files are checked by their cksum.  The password comes
# from the environment, never from this file.
#   VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=... VF2_PLUG=AC276E432AE6 sh plan/ws322/tests/p006/run-vf2-play.sh OUT BUILD P004 PLAY HEVC FIRMWARE
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: run-vf2-play.sh OUT BUILD P004 PLAY HEVC FIRMWARE}
build=${2:?the build directory}
c4=${3:?the p004 clips}
play=${4:?the p006 clips}
hevc=${5:?the HEVC streams}
firmware=${6:?the firmware directory}
: "${VF2_HOST:?VF2_HOST=user@host}"
: "${VF2_PASSWORD:?VF2_PASSWORD}"
: "${VF2_PLUG:?VF2_PLUG}"
export SSHPASS="$VF2_PASSWORD" VF2_PLUG
export PATH=/usr/bin:/bin:$PATH
SSHOPT="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8"
mkdir -p "$out/m"
summary=$out/summary.txt
: > "$summary"
say() { echo "$*" | tee -a "$summary"; }
b() { timeout "${BT:-60}" sshpass -e ssh $SSHOPT "$VF2_HOST" "$@"; }
bsudo() { printf '%s\n' "$VF2_PASSWORD" | b "sudo -S sh -c '$1'" 2> /dev/null; }
bcp() { timeout "${BT:-600}" sshpass -e scp $SSHOPT "$@"; }
old=q1825orig
H=/tmp/p6p
F=/lib/firmware/wave511
jvt="CAPM3_Sony_D CAWP1_TOSHIBA_E MR2_TANDBERG_E CVSE3_Sony_H"
h265="h265-1080p30 h265-1080p60 DSLICE_A_HHI_5 RAP_B_Bossen_2"

say "start $(date +%H:%M) kernel $(cksum < "$build/vmunix")"
say "board before $(b "cksum /boot/boot0/vmunix /boot/boot0/vmunix.$old /etc/group /boot/boot0/zedbsd.cfg; ls $F" 2>&1 | tr '\n' ' ')"
bcp "$VF2_HOST:/boot/boot0/vmunix.$old" "$out/board-vmunix" || { say "FAIL the board's kernel was not read back"; exit 1; }
added=0
if ! b "test -f $F/chagall.bin" > /dev/null 2>&1; then
	added=1
	bcp "$firmware/chagall.bin" "$VF2_HOST:/tmp/chagall.bin" || { say "FAIL the firmware was not copied"; exit 1; }
	bsudo "mkdir -p $F && cp /tmp/chagall.bin $F/chagall.bin && rm -f /tmp/chagall.bin && sync"
fi
sync
timeout 900 sh plan/tools/vf2/run.sh "$build/vmunix" "$out/boot" > "$out/boot.txt" 2>&1
say "boot $(tail -2 "$out/boot.txt" | tr '\n' ' ')"
b "mkdir -p $H/bin $H/lib $H/run $H/c $H/m && chmod 700 $H/run"
bcp "$build/bin/wayland" "$build/bin/files" "$build/bin/videoplayer" "$build/bin/media-probe" "$build/bin/vkvideo-probe" \
	"$build/bin/keiland-shot" "$VF2_HOST:$H/bin/" && bcp "$build"/dynamic/*.so "$VF2_HOST:$H/lib/" &&
	bcp "$c4/h264-1080p30.mp4" "$play"/mp4/*.mp4 "$play"/raw/*.h265 "$play"/raw/*.sha256 "$VF2_HOST:$H/c/" &&
	bcp "$hevc/SAODBLK_A_MainConcept_4.h265" "$hevc/SAODBLK_A_MainConcept_4.sha256" "$hevc/SAODBLK_B_MainConcept_4.h265" \
	"$hevc/SAODBLK_B_MainConcept_4.sha256" "$hevc/VPSSPSPPS_A_MainConcept_1.h265" "$VF2_HOST:$H/c/" ||
	say "FAIL the files were not copied"
/usr/bin/python3 -I plan/ws324/tests/conformance/reorder-ps.py < "$hevc/VPSSPSPPS_A_MainConcept_1.h265" |
	timeout 120 ffmpeg -hide_banner -loglevel error -nostdin -f hevc -i pipe:0 -f framehash -hash sha256 -pix_fmt nv12 - |
	grep -v '^#' | awk -F', *' '{ print $6 }' > "$out/VPSSPSPPS_A_MainConcept_1.sha256"
bcp "$out/VPSSPSPPS_A_MainConcept_1.sha256" "$VF2_HOST:$H/c/" || say "FAIL the reordered reference was not copied"

# The board's steps: the helpers, then each part.
cat > "$out/common.sh" <<'BOARD'
H=/tmp/p6p
export LD_LIBRARY_PATH=$H/lib XDG_RUNTIME_DIR=$H/run
T() { echo "$1 test $(cat /dev/wave5-test)"; }
mark() { dmesg > $H/k.before; echo clear > /dev/wave5-test; }
seen() { dmesg > $H/k.after; grep -F -x -v -f $H/k.before $H/k.after | grep -v dw-mmc | grep -iE 'wave5|vf2-vpu|video|fault|lost|fatal|panic|reset|hang|refus|fail|recover|stopped|differs' | head -${2:-6} | sed "s/^/$1 kernel /"; }
M() {
	mark
	timeout 900 $H/bin/media-probe --video-hash --time $H/c/$2.mp4 > $H/m/$2.hash 2> $H/m/$2.err
	echo "$1 $2 media-probe exit=$? $(wc -l < $H/m/$2.hash) hashes | $(grep -E 'backend|frames=|GPU holds|read back' $H/m/$2.err | head -3 | tr '\n' ' ')"
	T "$1-$2"; seen "$1-$2"
}
VP() {
	mark
	rm -f $H/run/wayland-0 $H/$2.png
	$H/bin/wayland --socket=$H/run/wayland-0 "--desktop-client=$H/bin/files --desktop" > $H/w-$2.log 2>&1 < /dev/null &
	wpid=$!
	sleep 15
	WAYLAND_DISPLAY=wayland-0 $H/bin/videoplayer --timeout-s=16 $H/c/$2.mp4 > $H/p-$2.log 2>&1 < /dev/null &
	ppid=$!
	sleep 8
	if [ "$3" = shot ]; then
		timeout 60 $H/bin/keiland-shot --socket $H/run/keiland-shot.sock $H/$2.png > $H/s-$2.txt 2>&1
		echo "$1 $2 shot exit=$? $(tr '\n' ' ' < $H/s-$2.txt)"
	fi
	top -b -n 1 2> /dev/null | head -10 | sed "s/^/$1 $2 top /"
	wait $ppid
	echo "$1 $2 player exit=$?"
	grep -E 'VIDEOPLAYER (PRESENT|FRAMES|FAILED|DONE)|GPU holds|read back' $H/p-$2.log | tail -6 | sed "s/^/$1 $2 /"
	kill $wpid 2> /dev/null; sleep 3; kill -9 $wpid 2> /dev/null
	ps -e | grep "$H/bin/files" | grep -v grep | while read pid rest; do kill $pid; done
	echo "$1 $2 compositor $(grep -cE 'KWL (FAILED|VULKAN_ERROR)' $H/w-$2.log) errors"
	T "$1-$2"; seen "$1-$2"
}
P() {
	mark
	timeout 900 $H/bin/vkvideo-probe --time --expect=$H/c/$2.sha256 $H/c/$2.h265 > $H/m/$2.txt 2>&1
	echo "$1 $2 vkvideo-probe exit=$? reference=$(wc -l < $H/c/$2.sha256) $(grep -c MISMATCH $H/m/$2.txt) mismatches | $(grep -E 'frames decoded|refused' $H/m/$2.txt | tr '\n' ' ')"
	T "$1-$2"; seen "$1-$2"
}
BOARD
cat "$out/common.sh" > "$out/a.sh"
cat >> "$out/a.sh" <<BOARD
dmesg | grep -E 'vf2-vpu' | head -2 | sed 's/^/A0 /'
M A h264-1080p30
for n in $jvt; do M A \$n; done
VP A h264-1080p30 shot
BOARD
cat "$out/common.sh" > "$out/b.sh"
cat >> "$out/b.sh" <<BOARD
for n in $h265; do M B \$n; done
VP B h265-1080p30 shot
VP B h265-1080p60 noshot
for n in odd-1016x760 odd-1000x562 sizes SAODBLK_A_MainConcept_4 SAODBLK_B_MainConcept_4 VPSSPSPPS_A_MainConcept_1; do P B \$n; done
dmesg > \$H/dmesg-end.txt
BOARD
# Compares a media-probe hash list with its reference in order.
compare() {
	/usr/bin/python3 -I - "$out/m/$2.hash" "$3" <<'PY' | sed "s/^/$1 $2 /" | tee -a "$summary"
import sys
def read(path):
    rows = []
    try:
        for line in open(path):
            parts = line.split()
            if len(parts) == 2 and len(parts[1]) == 64:
                rows.append(parts[1])
    except OSError:
        pass
    return rows
got = read(sys.argv[1])
want = read(sys.argv[2])
same = sum(1 for a, b in zip(got, want) if a == b)
first = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b), -1)
verdict = "MATCH" if got and len(got) == len(want) and same == len(want) else "DIFFER"
print("%s %d of %d frames, %d equal in order (first differing %d)" % (verdict, len(got), len(want), same, first))
PY
}
for part in a b; do
	bcp "$out/$part.sh" "$VF2_HOST:$H/$part.sh" || say "FAIL the board's script $part was not copied"
	printf '%s\n' "$VF2_PASSWORD" | BT=3600 b "sudo -S sh $H/$part.sh" > "$out/board-$part.txt" 2>&1
	sed 's/^\[sudo\][^:]*: //' "$out/board-$part.txt" | tee -a "$summary"
	bcp "$VF2_HOST:$H/m/*" "$out/m/" > /dev/null 2>&1
	bcp "$VF2_HOST:$H/*.png" "$VF2_HOST:$H/p-*.log" "$out/" > /dev/null 2>&1
	if [ "$part" = a ]; then
		compare A h264-1080p30 "$c4/h264-1080p30.ref"
		for n in $jvt; do compare A $n "$play/mp4/$n.ref"; done
		[ -f "$out/h264-1080p30.png" ] && say "A png $(cksum < "$out/h264-1080p30.png")"
	else
		for n in $h265; do compare B $n "$play/mp4/$n.ref"; done
		[ -f "$out/h265-1080p30.png" ] && say "B png $(cksum < "$out/h265-1080p30.png")"
	fi
	say "part $part done $(date +%H:%M)"
done
bcp "$VF2_HOST:$H/dmesg-end.txt" "$out/" > /dev/null 2>&1

# The board as it was: the files removed, its own kernel back, checked by cksum.
bsudo "rm -rf $H; sync"
[ "$added" = 1 ] && bsudo "rm -rf $F; sync"
sync
timeout 900 sh plan/tools/vf2/run.sh "$out/board-vmunix" "$out/restore" > "$out/restore.txt" 2>&1
say "restore $(tail -2 "$out/restore.txt" | tr '\n' ' ')"
say "board after $(b "cksum /boot/boot0/vmunix /boot/boot0/vmunix.$old /etc/group /boot/boot0/zedbsd.cfg; ls /tmp $F; uname -sm" 2>&1 | tr '\n' ' ')"
say "own kernel $(cksum < "$out/board-vmunix")"
say "end $(date +%H:%M)"
sync

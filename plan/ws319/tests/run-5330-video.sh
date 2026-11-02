#!/bin/sh
# WS319's video checks on the Latitude 5330 (i915, Alder Lake), in one run from the build host's tree top:
#   sh plan/ws319/tests/run-5330-video.sh OUT BUILD CLIPS HEVC
# BUILD is a build of plan/tools/hw5330/config-5330.mk with vmunix, bin/{wayland,files,videoplayer,media-probe,
# vkvideo-probe,keiland-shot} and dynamic/*.so; CLIPS is plan/ws319/tests/make-clips.sh's output, HEVC
# plan/ws322/tests/p006/make-hevc.sh's.  From the 5330's Debian (ssh solaris10-man) it keeps the NVMe zedBSD's kernel
# once as ~/ws319/vmunix.orig and its zedbsd.cfg for this start, puts BUILD's kernel on the NVMe (5330-nvme-write.sh),
# drops login=graphical for this start (the run's own compositor takes the display), boots zedBSD once by BootNext,
# copies the programs and clips to /tmp/p319-HHMMSS and runs the parts PARTS names (default "h264 h265 probe"; zedBSD's /tmp is a
# tmpfs, so nothing is left after the boot):
#   h264   media-probe --video-hash on the H.264 clips; Video Player (VP_GPU_PRESENT) on h264-1080p30 and -1080p60
#          for 16 s each in a compositor of its own (FRAMES, PRESENT, the screen's PNG 8 s in, top)
#   h265   the same with the H.265 clips
#   probe  vkvideo-probe --list, then vkvideo-probe --copy --expect on each HEVC stream (NAMES, or every one of
#          HEVC/list.txt), one line a stream in OUT/probe.txt
# then Debian again (a reboot), the kept zedbsd.cfg and kernel back on the NVMe and their cksums compared.  Each step
# has a time limit.  Holds /tmp/i915-hw.lock for the whole run.  The 5330 has no remote power: a zedBSD that does not
# answer is reported (exit 2) and left for a person.  Output: OUT/summary.txt (also printed), OUT/*.png, OUT/m/.
# The last line is "run-5330-video: DONE" or "run-5330-video: FAIL (...)".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: run-5330-video.sh OUT BUILD CLIPS HEVC}
build=${2:?the build directory}
clips=${3:?the clips}
hevc=${4:?the HEVC streams}
parts=${PARTS:-h264 h265 probe}
cd "$(dirname "$0")/../../.."
HOST=solaris10-man
ZB=10.0.30.3
PW=user
SO='-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -o LogLevel=ERROR -o PubkeyAuthentication=no'
NV_BOOT=/dev/disk/by-partuuid/d4dbe962-cc37-4f27-8529-bb7446730178
NV_ESP_UUID=08ab575f-f718-4eff-a3c9-10d80b1217ea
H=/tmp/p319-$(date +%H%M%S)
mkdir -p "$out/m"
summary=$out/summary.txt
: > "$summary"
say() { echo "$*" | tee -a "$summary"; }
lin() { timeout "${T:-120}" ssh -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$1"; }
zb() { timeout "${T:-60}" sshpass -p "$PW" ssh $SO "user@$ZB" "$1"; }
zput() { timeout "${T:-900}" sshpass -p "$PW" scp $SO -q "$@"; }
exec 9> /tmp/i915-hw.lock
flock 9
say "start $(date +%H:%M) kernel $(cksum < "$build/vmunix") parts: $parts"

# The helper, the kept kernel (once) and this start's zedbsd.cfg without login=graphical.
T=60 timeout 60 scp -q plan/tools/hw5330/5330-nvme-write.sh plan/tools/hw5330/ufs-grow.py "$HOST:ws245/" || { say "run-5330-video: FAIL (copy of the helper)"; exit 1; }
lin 'mkdir -p $HOME/ws319 && sudo -n sh $HOME/ws245/5330-nvme-write.sh check' > /dev/null || { say "run-5330-video: FAIL (the NVMe partitions)"; exit 1; }
lin "sudo -n mount $NV_BOOT /mnt/zb && { test -f \$HOME/ws319/vmunix.orig || cp /mnt/zb/vmunix \$HOME/ws319/vmunix.orig; } && cp /mnt/zb/zedbsd.cfg \$HOME/ws319/zedbsd.cfg.run && sudo -n sh -c 'grep -vx login=graphical /mnt/zb/zedbsd.cfg > /tmp/p319.cfg; cp /tmp/p319.cfg /mnt/zb/zedbsd.cfg' && sync && sudo -n umount /mnt/zb" ||
	{ say "run-5330-video: FAIL (the boot FAT)"; exit 1; }
say "kept: $(lin 'cksum < $HOME/ws319/vmunix.orig; cksum < $HOME/ws319/zedbsd.cfg.run' | tr '\n' ' ')"
T=120 timeout 120 scp -q "$build/vmunix" "$HOST:ws319/vmunix-run" || { say "run-5330-video: FAIL (copy of the kernel)"; exit 1; }
lin 'sudo -n sh $HOME/ws245/5330-nvme-write.sh kernel $HOME/ws319/vmunix-run' > /dev/null || { say "run-5330-video: FAIL (kernel install)"; exit 1; }

# Puts the kept kernel and zedbsd.cfg back (from Debian), with their cksums.
restore() {
	lin "sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws319/vmunix.orig && sudo -n mount $NV_BOOT /mnt/zb && sudo -n cp \$HOME/ws319/zedbsd.cfg.run /mnt/zb/zedbsd.cfg && sync && cksum < /mnt/zb/vmunix && cksum < /mnt/zb/zedbsd.cfg; sudo -n umount /mnt/zb" > "$out/restore.txt" 2>&1
	say "restored: $(tr '\n' ' ' < "$out/restore.txt")"
}

# BootNext to the NVMe's zedBSD.
entry=$(lin "sudo -n efibootmgr -v" | grep -i "$NV_ESP_UUID" | grep -i 'zedbsd.BOOTX64' | sed -n 's/^Boot\([0-9A-F]\{4\}\).*/\1/p' | head -1)
[ -n "$entry" ] || { say "run-5330-video: FAIL (no UEFI entry)"; restore; exit 1; }
lin "sudo -n efibootmgr -n $entry > /dev/null && sudo -n systemctl reboot" > /dev/null 2>&1
sleep 20
start=$(date +%s)
until zb 'uname -s' 2> /dev/null | grep -qi zedbsd; do
	if [ $(($(date +%s) - start)) -gt 300 ]; then
		say "run-5330-video: FAIL (zedBSD did not answer in 5 minutes; the 5330 needs a person)"
		exit 2
	fi
	sleep 5
done
say "zedBSD up after $(($(date +%s) - start + 20)) s"
sleep "${SETTLE:-30}"

# The programs and the clips.
zb "mkdir -p $H/bin $H/lib $H/run $H/c $H/m && chmod 700 $H/run"
zput "$build/bin/wayland" "$build/bin/files" "$build/bin/videoplayer" "$build/bin/media-probe" "$build/bin/vkvideo-probe" \
	"$build/bin/keiland-shot" "user@$ZB:$H/bin/" && zput "$build"/dynamic/*.so "user@$ZB:$H/lib/" &&
	zput "$clips"/mp4/*.mp4 "user@$ZB:$H/c/" || say "FAIL the programs or clips were not copied"

# The 5330's steps: the helpers, then each part's script.
{ echo "H=$H"; cat <<'BOARD'
export LD_LIBRARY_PATH=$H/lib XDG_RUNTIME_DIR=$H/run
mark() { dmesg > $H/k.before; }
seen() { dmesg > $H/k.after; grep -F -x -v -f $H/k.before $H/k.after | grep -iE 'i915|video|fault|lost|fatal|panic|reset|hang|refus|fail|skip|quarantin' | head -${2:-6} | sed "s/^/$1 kernel /"; }
M() {
	mark
	timeout 900 $H/bin/media-probe --video-hash --time $H/c/$2.mp4 > $H/m/$2.hash 2> $H/m/$2.err
	echo "$1 $2 media-probe exit=$? $(wc -l < $H/m/$2.hash) hashes | $(grep -E 'backend|frames=|GPU holds|read back' $H/m/$2.err | head -3 | tr '\n' ' ')"
	seen "$1-$2"
}
VP() {
	mark
	$H/bin/wayland --socket=$H/run/wayland-0 "--desktop-client=$H/bin/files --desktop" > $H/w-$2.log 2>&1 < /dev/null &
	wpid=$!
	sleep 15
	WAYLAND_DISPLAY=wayland-0 $H/bin/videoplayer --timeout-s=16 $H/c/$2.mp4 > $H/p-$2.log 2>&1 < /dev/null &
	ppid=$!
	sleep 8
	timeout 60 $H/bin/keiland-shot --socket $H/run/keiland-shot.sock $H/$2.png > $H/s-$2.txt 2>&1
	echo "$1 $2 shot exit=$? $(tr '\n' ' ' < $H/s-$2.txt)"
	top -b -n 1 2> /dev/null | head -10 | sed "s/^/$1 $2 top /"
	n=0
	while kill -0 $ppid 2> /dev/null && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
	kill -9 $ppid 2> /dev/null
	wait $ppid
	echo "$1 $2 player exit=$?"
	grep -E 'VIDEOPLAYER (PRESENT|FRAMES|FAILED|DONE)|GPU holds|read back|backend' $H/p-$2.log | tail -6 | sed "s/^/$1 $2 /"
	kill $wpid 2> /dev/null; sleep 3; kill -9 $wpid 2> /dev/null
	ps -e | grep "$H/bin/files" | grep -v grep | while read pid rest; do kill $pid; done
	echo "$1 $2 compositor $(grep -cE 'KWL (FAILED|VULKAN_ERROR)' $H/w-$2.log) errors"
	cp $H/p-$2.log $H/w-$2.log $H/m/ 2> /dev/null
	seen "$1-$2"
}
P() {
	mark
	timeout 600 $H/bin/vkvideo-probe --copy --expect=$H/c/$2.sha256 $H/c/$2.h265 > $H/m/$2.txt 2>&1
	echo "$1 $2 vkvideo-probe exit=$? reference=$(wc -l < $H/c/$2.sha256) $(grep -c MISMATCH $H/m/$2.txt) mismatches | $(grep -E 'frames decoded|refused|matched' $H/m/$2.txt | tail -2 | tr '\n' ' ')"
	seen "$1-$2" 3
}
BOARD
} > "$out/common.sh"
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
run_part() {
	zput "$out/$1.sh" "user@$ZB:$H/$1.sh" || say "FAIL the script $1 was not copied"
	T=3600 zb "echo $PW | sudo -S sh $H/$1.sh" > "$out/board-$1.txt" 2>&1
	sed 's/^Password://' "$out/board-$1.txt" | tee -a "$summary"
	zput "user@$ZB:$H/m/*" "$out/m/" > /dev/null 2>&1
	zput "user@$ZB:$H/*.png" "$out/" > /dev/null 2>&1
}
for part in $parts; do
	cat "$out/common.sh" > "$out/$part.sh"
	case "$part" in
	h264 | h265)
		{
			echo "dmesg | grep -iE 'i915.*(video|VCS|vcs0)' | head -4 | sed 's/^/$part boot /'"
			echo "M $part $part-1080p30"
			echo "M $part $part-1080p60"
			echo "VP $part $part-1080p30"
			echo "VP $part $part-1080p60"
		} >> "$out/$part.sh"
		run_part "$part"
		compare "$part" "$part-1080p30" "$clips/mp4/$part-1080p30.ref"
		compare "$part" "$part-1080p60" "$clips/mp4/$part-1080p60.ref"
		;;
	probe)
		names=${NAMES:-$(cut -f1 "$hevc/list.txt")}
		echo "$H/bin/vkvideo-probe --list 2>&1 | grep -iE 'h265|h264|codec|video' | head -8 | sed 's/^/probe list /'" >> "$out/probe.sh"
		: > "$out/probe-files.txt"
		for n in $names; do
			echo "$hevc/$n.h265" >> "$out/probe-files.txt"
			echo "$hevc/$n.sha256" >> "$out/probe-files.txt"
			echo "P probe $n" >> "$out/probe.sh"
		done
		T=1800 zput $(cat "$out/probe-files.txt") "user@$ZB:$H/c/" || say "FAIL the streams were not copied"
		run_part probe
		grep '^probe [^ ]* vkvideo-probe' "$out/board-probe.txt" > "$out/probe.txt"
		;;
	esac
done
zb "dmesg" > "$out/dmesg.txt" 2>&1

# Back to Debian, and the kept files back.
zb "echo $PW | sudo -S reboot" > /dev/null 2>&1
start=$(date +%s)
until T=8 lin hostname 2> /dev/null | grep -q chaos; do
	if [ $(($(date +%s) - start)) -gt 300 ]; then
		say "run-5330-video: FAIL (Debian did not come back in 5 minutes; the 5330 needs a person)"
		exit 2
	fi
	sleep 5
done
say "Debian up after $(($(date +%s) - start)) s"
restore
sync
say "run-5330-video: DONE"

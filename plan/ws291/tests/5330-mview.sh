#!/bin/sh
# ws291-p022: mview's scene on the Latitude 5330's i915 (Alder Lake), one step per command, run from the top of the
# tree.  Debian (ssh alias solaris10-man) is the 5330's normal system; zedBSD starts once from its NVMe partitions by
# UEFI BootNext (entry "zedBSD NVMe") and is reached as root with the guest harness's key (an image of
# plan/ws291/tests/config-5330.mk).  Each command takes /tmp/i915-hw.lock and prints only a short summary.
#
#   5330-mview.sh backup          (Debian) the NVMe zedBSD as it is now, once: p4's files, p5 and p6 whole (gzip),
#                                 the loader on the Debian ESP, to ~/p1-p022/orig on the Debian, with their sums
#   5330-mview.sh image IMG       (Debian) IMG written to the NVMe partitions (5330-nvme-write.sh image), zedBSD
#                                 started once; needs the backup
#   5330-mview.sh kernel VMUNIX   (zedBSD) VMUNIX put in /boot/boot0/vmunix (the one before kept as vmunix.prev),
#                                 zedBSD restarted (through Debian)
#   5330-mview.sh files DEST=SRC...  (zedBSD) files copied in, no restart
#   5330-mview.sh info            (zedBSD) the GT's EUs and frequencies, the kernel, the screen
#   5330-mview.sh run OUT [COPIES...]  (zedBSD) mview in the session for each COPIES (default 1 1030): 12 s spin,
#                                 game content, mailbox, 4 images (MVIEW_ARGS added, ZEDBSD_ENV set); MVIEW SPIN and
#                                 STAGES lines, the compositor's KWL SCANOUT lines and the kernel's new lines of i915;
#                                 OUT/summary.txt
#   5330-mview.sh shot OUT COPIES (zedBSD) mview's still view of COPIES copies (FIFO, composed) taken by keiland-shot
#                                 6 s in: OUT/shot-COPIES.png, compared with OUT/../linux-shot-COPIES.png when there
#   5330-mview.sh linux-shot OUT COPIES  (Debian) the same view on weston (--debug) taken by weston-screenshooter:
#                                 OUT/linux-shot-COPIES.png
#   5330-mview.sh linux-exec SCRIPT  (Debian) SCRIPT run there with the iGPU on i915 (vfio-pci again after), at
#                                 most 600 s (for a Linux-side diagnostic, as Mesa's INTEL_DEBUG dumps)
#   5330-mview.sh linux           Debian again (zedBSD rebooted; the BootOrder brings Debian back)
#   5330-mview.sh zedbsd          zedBSD started once from Debian (when Debian runs)
#   5330-mview.sh restore         (Debian) the backup written back to p4, p5, p6 and the ESP loader, and compared
#   5330-mview.sh linux-build     (Debian) mview for Debian (plan/tools/gpu-perf/linux/build-mview.sh) built on the
#                                 5330 from this tree's sources, in ~/p1-p022/linux-mview
#   5330-mview.sh linux-run OUT [COPIES...]  (Debian) the Linux reference: the iGPU moved from vfio-pci to i915 for the
#                                 run, weston's kiosk shell on its DRM backend (seatd), mview for each COPIES with the
#                                 same options as run; MVIEW SPIN and STAGES lines and weston's composition per repaint
#                                 ("plane-only" is the direct scanout), OUT/summary.txt; weston is ended and the iGPU
#                                 given back to vfio-pci (the boot default)
#
# Nothing is removed on either machine but zedBSD's own /tmp files of a run.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cmd=${1:?usage: 5330-mview.sh backup|image IMG|kernel VMUNIX|files DEST=SRC...|info|run OUT [COPIES...]|linux|zedbsd|restore}
shift
top=$(pwd)
HOST=solaris10-man
ZB=${ZB:-10.0.30.3}
KEY=${KEY:-$top/plan/tmp/guest/id_ed25519}
ORIG=p1-p022/orig
NV_BOOT=/dev/disk/by-partuuid/d4dbe962-cc37-4f27-8529-bb7446730178
NV_ROOT=/dev/disk/by-partuuid/24c59452-7bc3-42e7-b532-5653d935c325
NV_SWAP=/dev/disk/by-partuuid/a26f52a1-fed3-4073-b577-1bc61322882f
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
mkdir -p "$top/build/tmp"
ZSSHOPT="$SSHOPT -o ControlMaster=auto -o ControlPath=$top/build/tmp/p022-ssh-%h -o ControlPersist=300"
SESSION=/run/user/1000/session.log

exec 9> /tmp/i915-hw.lock
flock 9

say() {
	echo "5330-mview: $*"
}

deb() {
	timeout "${T:-120}" ssh -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$@"
}

zs() {
	timeout "${T:-60}" ssh -i "$KEY" $ZSSHOPT "root@$ZB" "$@"
}

zcp() {
	for try in 1 2 3; do
		timeout 180 scp -O -i "$KEY" $ZSSHOPT "$@" && return 0
		sleep 5
	done
	return 1
}

# zget DIR REMOTE...: each remote file into DIR, read with cat over SSH.
zget() {
	gdir=$1
	shift
	for gfile in "$@"; do
		T=60 zs "cat $gfile" > "$gdir/$(basename "$gfile")" < /dev/null || return 1
	done
}

is_linux() {
	[ "$(T=15 deb uname -s 2> /dev/null)" = Linux ]
}

is_zedbsd() {
	[ "$(T=15 zs uname -s 2> /dev/null)" = zedBSD ]
}

# wait_for linux|zedbsd SECONDS
wait_for() {
	end=$(($(date +%s) + $2))
	while [ "$(date +%s)" -lt "$end" ]; do
		if [ "$1" = zedbsd ]; then
			is_zedbsd && return 0
		else
			is_linux && return 0
		fi
		sleep 5
	done
	say "$1 did not come up in $2 s"
	return 1
}

to_zedbsd() {
	is_zedbsd && return 0
	is_linux || { say "neither system answers"; return 1; }
	deb '~/boot-zedbsd.sh --no-reboot' > /dev/null || { say "FAIL BootNext"; return 1; }
	deb '(sleep 1; sudo -n systemctl reboot) > /dev/null 2>&1 &'
	sleep 30
	wait_for zedbsd 300 || return 1
	# The session's compositor.
	end=$(($(date +%s) + 120))
	while [ "$(date +%s)" -lt "$end" ]; do
		T=15 zs "test -S /run/user/1000/wayland-0 && test -f $SESSION" < /dev/null && break
		sleep 3
	done
	say "zedBSD up: $(T=15 zs 'uname -v; sysctl -n kern.boot.login 2> /dev/null' < /dev/null | tr '\n' ' ')"
}

to_linux() {
	is_linux && return 0
	is_zedbsd || { say "neither system answers"; return 1; }
	T=10 zs '(sleep 1; reboot) > /dev/null 2>&1 &' < /dev/null
	sleep 30
	wait_for linux 300 || return 1
	say "Debian up: $(T=15 deb 'hostname; uname -r' | tr '\n' ' ')"
}

# The sums of the NVMe zedBSD as it is now (p4's files, p5 and p6 whole, the loader).
nvme_sums() {
	T=900 deb "sudo -n sh -c 'mkdir -p /mnt/zb && mount -o ro $NV_BOOT /mnt/zb && (cd /mnt/zb && find . -type f | sort | xargs cksum) ; umount /mnt/zb; cksum /boot/efi/EFI/zedbsd/BOOTX64.EFI; for p in $NV_ROOT $NV_SWAP; do echo \$(sha256sum < \$p | cut -c1-16) \$(readlink -f \$p); done'"
}

backup() {
	is_linux || { say "FAIL Debian is not running"; return 1; }
	if T=15 deb "test -f $ORIG/sums.txt"; then
		say "backup already there: $(T=15 deb "cat $ORIG/sums.txt | tr '\n' ' '")"
		return 0
	fi
	T=1800 deb "set -e; mkdir -p $ORIG/p4; sudo -n sh -c 'mount -o ro $NV_BOOT /mnt/zb && cp -a /mnt/zb/. $ORIG/p4/ && umount /mnt/zb; cp -p /boot/efi/EFI/zedbsd/BOOTX64.EFI $ORIG/BOOTX64.EFI; dd if=$NV_ROOT bs=4M status=none | gzip -1 > $ORIG/p5.gz; dd if=$NV_SWAP bs=4M status=none | gzip -1 > $ORIG/p6.gz; chown -R \$SUDO_USER $ORIG'" ||
	    { say "FAIL backup"; return 1; }
	nvme_sums > "$top/build/tmp/p022-sums.txt" || { say "FAIL sums"; return 1; }
	timeout 60 scp -q "$top/build/tmp/p022-sums.txt" "$HOST:$ORIG/sums.txt" || return 1
	say "backup: $(tr '\n' ' ' < "$top/build/tmp/p022-sums.txt")"
	say "backup files: $(T=15 deb "du -sh $ORIG/p4 $ORIG/p5.gz $ORIG/p6.gz | tr '\n' ' '")"
}

restore() {
	to_linux || return 1
	T=15 deb "test -f $ORIG/sums.txt" || { say "FAIL no backup"; return 1; }
	T=1800 deb "sudo -n sh -c 'set -e; gzip -dc $ORIG/p5.gz | dd of=$NV_ROOT bs=4M conv=fsync status=none; gzip -dc $ORIG/p6.gz | dd of=$NV_SWAP bs=4M conv=fsync status=none; mount $NV_BOOT /mnt/zb; cp -a $ORIG/p4/. /mnt/zb/; sync; umount /mnt/zb; cp -p $ORIG/BOOTX64.EFI /boot/efi/EFI/zedbsd/BOOTX64.EFI; sync'" ||
	    { say "FAIL restore"; return 1; }
	nvme_sums > "$top/build/tmp/p022-sums-now.txt" || return 1
	T=15 deb "cat $ORIG/sums.txt" > "$top/build/tmp/p022-sums-orig.txt"
	# p4 may hold files the backup did not (an image's); they are listed and not removed.
	if diff "$top/build/tmp/p022-sums-orig.txt" "$top/build/tmp/p022-sums-now.txt" > "$top/build/tmp/p022-sums-diff.txt"; then
		say "restore: the same as the backup ($(wc -l < "$top/build/tmp/p022-sums-now.txt") sums)"
	else
		say "restore: differs: $(tr '\n' ' ' < "$top/build/tmp/p022-sums-diff.txt")"
	fi
}

image() {
	img=$1
	to_linux || return 1
	T=15 deb "test -f $ORIG/sums.txt" || { say "FAIL no backup (run backup first)"; return 1; }
	T=60 timeout 60 scp -q plan/tools/hw5330/5330-nvme-write.sh plan/tools/hw5330/ufs-grow.py "$HOST:ws245/" || return 1
	T=1200 timeout 1200 scp -q "$img" "$HOST:p1-p022/image.img" || { say "FAIL copy"; return 1; }
	T=1500 deb 'sudo -n sh $HOME/ws245/5330-nvme-write.sh image $HOME/p1-p022/image.img' > /dev/null || { say "FAIL NVMe write"; return 1; }
	say "image $(sha256sum "$img" | cut -c1-16) written to the NVMe"
	to_zedbsd
}

kernel() {
	vm=$1
	to_zedbsd || return 1
	zcp "$vm" "root@$ZB:/tmp/vmunix.new" || return 1
	T=60 zs 'cp /boot/boot0/vmunix /boot/boot0/vmunix.prev && cp /tmp/vmunix.new /boot/boot0/vmunix && sync && cksum /boot/boot0/vmunix' < /dev/null || return 1
	say "kernel $(cksum < "$vm" | cut -d' ' -f1) put in"
	to_linux || return 1
	to_zedbsd
}

files() {
	to_zedbsd || return 1
	for pair in "$@"; do
		dest=${pair%%=*}
		src=${pair#*=}
		zcp "$src" "root@$ZB:/tmp/p022.file" || return 1
		T=60 zs "cp /tmp/p022.file '$dest' && sync" < /dev/null || return 1
		say "$dest put in ($(cksum < "$src" | cut -d' ' -f1))"
	done
}

info() {
	to_zedbsd || return 1
	T=60 zs "uname -v; dmesg | grep -E 'gt_init_mmio|rps: (started|fixed)|i915: display|edp|mode [0-9]+x[0-9]+' | head -8" < /dev/null
}

# zrun: the script on standard input run on zedBSD as root, detached from the SSH session, its end polled.
zrun() {
	T=60 zs 'cat > /tmp/zrun.sh && rm -f /tmp/zrun.done && { (sh /tmp/zrun.sh; echo $? > /tmp/zrun.done) > /dev/null 2>&1 < /dev/null & }' || return 1
	end=$(($(date +%s) + ${ZRUN_S:-600}))
	while [ "$(date +%s)" -lt "$end" ]; do
		sleep 3
		T=15 zs 'test -f /tmp/zrun.done' < /dev/null 2> /dev/null && return 0
	done
	say "the run did not end in ${ZRUN_S:-600} s"
	return 1
}

run() {
	out=$1
	shift
	mkdir -p "$out"
	summary=$out/summary.txt
	: > "$summary"
	to_zedbsd || return 1
	copies_list=${*:-1 1030}
	user=$(T=15 zs "awk -F: '\$3 == 1000 { print \$1; exit }' /etc/passwd" < /dev/null)
	for copies in $copies_list; do
		zrun << EOS || { echo "c$copies FAIL the run" >> "$summary"; continue; }
rm -f /tmp/mview.log /tmp/mview-scanout.log /tmp/mview-kernel.log
n=\$(wc -l < $SESSION)
k=\$(dmesg | wc -l)
( for i in \$(seq 1 $((${SPIN:-12} * 2 + 6))); do sysctl hw.gputelemetry | sed -n 's|.* cur_mhz=\([0-9]*\) req_mhz=\([0-9]*\).* objects_limit=\([0-9]*\).*|\1/\2/\3|p'; sleep 0.5; done > /tmp/mview-freq.log 2>&1 ) &
su $user -c 'env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 ${ZEDBSD_ENV:-} /bin/mview --token=c$copies --spin=${SPIN:-12} --copies=$copies --content=game --present=mailbox --images=4 ${MVIEW_ARGS:-}' > /tmp/mview.log 2>&1 < /dev/null
tail -n +\$n $SESSION | grep -E 'KWL SCANOUT' > /tmp/mview-scanout.log
dmesg | tail -n +\$((k + 1)) > /tmp/mview-kernel.log
wait
EOS
		zget "$out" /tmp/mview.log /tmp/mview-scanout.log /tmp/mview-kernel.log /tmp/mview-freq.log || { echo "c$copies FAIL logs" >> "$summary"; continue; }
		for f in mview mview-scanout mview-kernel mview-freq; do
			mv "$out/$f.log" "$out/c$copies-$f.log"
		done
		echo "c$copies $(grep -E 'MVIEW (SPIN|STAGES|FAILED)' "$out/c$copies-mview.log" | sed 's/frames=[0-9]* seconds=[0-9.]* //' | tr '\n' ' ')" >> "$summary"
		echo "c$copies scanout $(grep -c 'direct=1' "$out/c$copies-mview-scanout.log") direct, $(grep -c 'direct=0' "$out/c$copies-mview-scanout.log") not: $(grep 'direct=0' "$out/c$copies-mview-scanout.log" | head -1)" >> "$summary"
		echo "c$copies cur/req_mhz $(sort -n "$out/c$copies-mview-freq.log" | uniq -c | sort -rn | head -4 | awk '{printf "%s×%s ", $2, $1}')" >> "$summary"
		grep -iE 'i915|vk|fault|hang|reset|lost' "$out/c$copies-mview-kernel.log" | grep -v 'timing' | head -4 | sed "s/^/c$copies kernel /" >> "$summary"
	done
	cat "$summary"
}

linux_build() {
	to_linux || return 1
	tar -cf - userland/tests/mview userland/desktop/libwayland/content-type-v1-client-protocol.h userland/desktop/paths.h plan/tools/gpu-perf/linux |
	    T=120 deb 'mkdir -p p1-p022/src && cd p1-p022/src && tar -xf -' || return 1
	T=600 deb 'cd p1-p022/src && sh plan/tools/gpu-perf/linux/build-mview.sh $HOME/p1-p022/linux-mview' 2>&1 | tail -3
}

# The iGPU to i915 (MODE host, the render node opened to the user, who is not in its group) or back to vfio-pci (MODE vfio), as plan/ws031/tests/host/igpu-mode.sh.
igpu() {
	T=120 deb "sudo -n sh -c 'D=0000:00:02.0; S=/sys/bus/pci/devices/\$D; cur=\$(basename \$(readlink \$S/driver) 2> /dev/null); want=\$( [ $1 = host ] && echo i915 || echo vfio-pci); if [ \"\$cur\" != \$want ]; then [ -n \"\$cur\" ] && echo \$D > \$S/driver/unbind; echo \$want > \$S/driver_override; [ \$want = i915 ] && modprobe i915; echo \$D > /sys/bus/pci/drivers_probe; fi; for i in 1 2 3 4 5 6 7 8 9 10; do [ \$want = vfio-pci ] || ls /dev/dri/renderD* > /dev/null 2>&1 && break; sleep 1; done; [ \$want = i915 ] && setfacl -m u:\$SUDO_USER:rw /dev/dri/renderD*; basename \$(readlink \$S/driver)'"
}

linux_run() {
	out=$1
	shift
	mkdir -p "$out"
	summary=$out/summary.txt
	: > "$summary"
	to_linux || return 1
	echo "igpu $(igpu host)" >> "$summary"
	T=60 deb "export XDG_RUNTIME_DIR=/run/user/\$(id -u); mkdir -p \$XDG_RUNTIME_DIR; pgrep -x weston > /dev/null || (export LIBSEAT_BACKEND=seatd; nohup weston --backend=drm --shell=kiosk --continue-without-input --socket=wayland-1 --log=/tmp/p022-weston.log --logger-scopes=log,drm-backend > /tmp/p022-weston.out 2>&1 < /dev/null & sleep 6); pgrep -x weston > /dev/null && echo weston up || { echo weston down; tail -5 /tmp/p022-weston.out; }" >> "$summary" 2>&1
	T=30 deb "grep -E 'Output .*(mode|enabled)|head|1920|Chipset|Vulkan|GL_RENDERER' /tmp/p022-weston.log | head -6" >> "$out/weston-head.txt" 2>&1
	for copies in ${*:-1 1030}; do
		T=$((${SPIN:-12} + 120)) deb "export XDG_RUNTIME_DIR=/run/user/\$(id -u) WAYLAND_DISPLAY=wayland-1; n=\$(wc -l < /tmp/p022-weston.log); sudo -n setfacl -m u:\$(id -un):rw /dev/dri/renderD*; (for i in \$(seq 1 $((${SPIN:-12} * 2 + 4))); do cat /sys/class/drm/card*/gt_act_freq_mhz 2> /dev/null | head -1; sleep 0.5; done > /tmp/p022-freq.log) & VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.json ${LINUX_ENV:-} ~/p1-p022/linux-mview/bin/mview --model=\$HOME/p1-p022/linux-mview/share/mview/qs40 --token=c$copies --spin=${SPIN:-12} --copies=$copies --content=game --present=mailbox --images=4 ${MVIEW_ARGS:-} > /tmp/p022-mview.log 2>&1 < /dev/null; tail -n +\$n /tmp/p022-weston.log | grep -E 'Using (plane-only|mixed|renderer)' | sed -E 's/^\[[0-9:.]+\][[:space:]]*//' | sort | uniq -c > /tmp/p022-planes.log"
		timeout 60 scp -q "$HOST:/tmp/p022-mview.log" "$out/c$copies-mview.log"
		timeout 60 scp -q "$HOST:/tmp/p022-planes.log" "$out/c$copies-planes.log"
		timeout 60 scp -q "$HOST:/tmp/p022-freq.log" "$out/c$copies-freq.log"
		echo "c$copies $(grep -E 'MVIEW (SPIN|STAGES|FAILED)' "$out/c$copies-mview.log" | sed 's/frames=[0-9]* seconds=[0-9.]* //' | tr '\n' ' ')" >> "$summary"
		echo "c$copies act_mhz $(sort -n "$out/c$copies-freq.log" | uniq -c | sort -rn | head -4 | awk '{printf "%s×%s ", $2, $1}')" >> "$summary"
		echo "c$copies weston $(sed 's/^ *//' "$out/c$copies-planes.log" | tr '\n' ' ')" >> "$summary"
	done
	grep -m1 -E 'deviceName|device=|GPU' "$out/c${1:-1}-mview.log" >> "$summary" 2>/dev/null
	T=30 deb 'pkill -x weston; sleep 2; pgrep -x weston > /dev/null && echo weston still running || echo weston ended' >> "$summary" 2>&1
	echo "igpu $(igpu vfio)" >> "$summary"
	cat "$summary"
}

shot() {
	out=$1
	copies=$2
	mkdir -p "$out"
	to_zedbsd || return 1
	user=$(T=15 zs "awk -F: '\$3 == 1000 { print \$1; exit }' /etc/passwd" < /dev/null)
	zrun << EOS || return 1
rm -f /tmp/shot.png /tmp/mview-shot.log
su $user -c 'env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 /bin/mview --token=shot --frames=900 --content=none --present=fifo --copies=$copies' > /tmp/mview-shot.log 2>&1 < /dev/null &
sleep 6
su $user -c 'env XDG_RUNTIME_DIR=/run/user/1000 keiland-shot /tmp/shot.png' > /dev/null 2>&1 < /dev/null
wait
EOS
	zget "$out" /tmp/shot.png /tmp/mview-shot.log || return 1
	mv "$out/shot.png" "$out/shot-$copies.png"
	grep -E 'MVIEW (DONE|FAILED)' "$out/mview-shot.log"
	ref=$(dirname "$out")/linux-shot-$copies.png
	[ -f "$ref" ] && echo "against $ref: $(python3 -I plan/tools/frame-ops/shot-diff.py "$ref" "$out/shot-$copies.png")"
}

linux_shot() {
	out=$1
	copies=$2
	mkdir -p "$out"
	to_linux || return 1
	echo "igpu $(igpu host)"
	T=60 deb "export XDG_RUNTIME_DIR=/run/user/\$(id -u) LIBSEAT_BACKEND=seatd; pgrep -x weston > /dev/null || (nohup weston --debug --backend=drm --shell=kiosk --continue-without-input --socket=wayland-1 > /tmp/p022-weston-shot.out 2>&1 < /dev/null & sleep 6); pgrep -x weston > /dev/null && echo weston up || echo weston down"
	T=120 deb "export XDG_RUNTIME_DIR=/run/user/\$(id -u) WAYLAND_DISPLAY=wayland-1; cd /tmp; rm -f wayland-screenshot*.png; sudo -n setfacl -m u:\$(id -un):rw /dev/dri/renderD*; VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.json ~/p1-p022/linux-mview/bin/mview --model=\$HOME/p1-p022/linux-mview/share/mview/qs40 --token=shot --frames=600 --content=none --present=fifo --copies=$copies > /tmp/p022-mview-shot.log 2>&1 < /dev/null & sleep 6; weston-screenshooter > /dev/null 2>&1 < /dev/null; wait; grep -E 'MVIEW (DONE|FAILED)' /tmp/p022-mview-shot.log; ls /tmp/wayland-screenshot*.png | head -1" > "$out/linux-shot.txt" 2>&1
	cat "$out/linux-shot.txt"
	png=$(grep 'wayland-screenshot' "$out/linux-shot.txt" | tail -1)
	[ -n "$png" ] && timeout 60 scp -q "$HOST:$png" "$out/linux-shot-$copies.png"
	T=30 deb 'pkill -x weston; sleep 3; pgrep -x weston > /dev/null && echo weston still running || echo weston ended'
	echo "igpu $(igpu vfio)"
	ls -la "$out/linux-shot-$copies.png" 2>&1
}

linux_exec() {
	to_linux || return 1
	echo "igpu $(igpu host)"
	timeout 60 scp -q "$1" "$HOST:p1-p022/exec.sh" || return 1
	T=600 deb 'sh p1-p022/exec.sh'
	echo "igpu $(igpu vfio)"
}

case "$cmd" in
linux-exec) linux_exec "$1" ;;
shot) shot "$1" "$2" ;;
linux-shot) linux_shot "$1" "$2" ;;
linux-build) linux_build ;;
linux-run) linux_run "$@" ;;
backup) backup ;;
restore) restore ;;
image) image "${1:?IMG}" ;;
kernel) kernel "${1:?VMUNIX}" ;;
files) files "$@" ;;
info) info ;;
run) run "$@" ;;
linux) to_linux ;;
zedbsd) to_zedbsd ;;
*) say "unknown command $cmd"; exit 2 ;;
esac

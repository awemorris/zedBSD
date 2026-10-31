#!/bin/sh
# ws291: venus-reg.sh on the RTX machine's Debian (10.0.10.3) instead of the dev host (whose disk is busy), from the
# top of the tree in the machine's lock with Debian running:
#   [DEB=awe@10.0.10.3] [REMOTE=p3-venus] sh plan/ws291/tests/venus-reg-rtx.sh OUT IMAGE LIBVULKAN...
# The Venus guest runs on Debian's QEMU (qemu-system-modules-opengl) with the strict-queue renderer of main's
# build/ws035-sq-venus/install on Mesa's lavapipe, as on the dev host; the display is egl-headless on the render node of
# vgem (the RTX stays on vfio-pci, the CPU has no iGPU).  Copied to ~/REMOTE: this tree's guest and Venus scripts
# (git archive of HEAD), the guest key, IMAGE, each LIBVULKAN and the renderer.  For the run only: vgem loaded and
# /dev/kvm, /dev/udmabuf (lavapipe's exported memory) and the render node opened to all (mode 666; on the dev host the
# user is in the kvm group); after it the modes are put back (660) and vgem taken out.
# OUT/summary.txt holds venus-reg's lines and the put-back checks.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: venus-reg-rtx.sh OUT IMAGE LIBVULKAN...}
image=${2:?the image}
shift 2
DEB=${DEB:-awe@10.0.10.3}
REMOTE=${REMOTE:-p3-venus}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"
say() { echo "$*" | tee -a "$summary"; }
deb() { timeout "${DT:-120}" ssh -n $SSHOPT "$DEB" "$@"; }

say "start $(date +%H:%M)"
deb "mkdir -p $REMOTE/build/p024/img $REMOTE/build/ws035-sq $REMOTE/build/ws035-sq-venus" || { say "FAIL Debian"; exit 1; }
git archive HEAD plan/tools/guest plan/tools/gpu-boundary plan/ws035/tests plan/ws291/tests plan/tmp/guest |
    timeout 300 ssh $SSHOPT "$DEB" "tar -x -C $REMOTE && chmod 600 $REMOTE/plan/tmp/guest/id_ed25519" || { say "FAIL copy of the scripts"; exit 1; }
timeout 900 scp -q $SSHOPT "$image" "$DEB:$REMOTE/build/p024/img/hdd-image.img" || { say "FAIL copy of the image"; exit 1; }
libs=
for lv in "$@"; do
	n=$(basename "$(dirname "$(dirname "$lv")")")-$(basename "$lv")
	timeout 120 scp -q $SSHOPT "$lv" "$DEB:$REMOTE/build/p024/img/$n" || { say "FAIL copy of $lv"; exit 1; }
	libs="$libs build/p024/img/$n"
done
timeout 300 scp -q -r $SSHOPT /home/awe/zedBSD-claude1/build/ws035-sq-venus/install "$DEB:$REMOTE/build/ws035-sq-venus/" ||
    { say "FAIL copy of the renderer"; exit 1; }
say "copied $(cksum < "$image" | cut -d' ' -f1) remote $(DT=300 deb "cksum < $REMOTE/build/p024/img/hdd-image.img" | cut -d' ' -f1)"
modes=$(deb 'stat -c "%a" /dev/kvm')
deb "sudo -n modprobe vgem && sudo -n chmod 666 /dev/kvm /dev/udmabuf /dev/dri/renderD128 && stat -c '%a %n' /dev/kvm /dev/udmabuf /dev/dri/renderD128" | sed 's/^/setup /' | tee -a "$summary"
run=venus-reg-$(date +%H%M)
DT=4000 deb "cd $REMOTE && START_TIMEOUT=1200 sh plan/ws291/tests/venus-reg.sh build/tmp/$run build/p024/img/hdd-image.img$libs" 2>&1 | tee -a "$summary"
deb "cd $REMOTE && tar --exclude='*.img' --exclude='*.fd' --exclude='*.sock' -cf - build/tmp/$run" > "$out/remote.tar" 2>/dev/null
deb "pgrep -c qemu-system || true; sudo -n chmod ${modes:-660} /dev/kvm /dev/udmabuf /dev/dri/renderD128; sudo -n modprobe -r vgem; stat -c '%a %n' /dev/kvm /dev/udmabuf; ls /dev/dri 2>&1 | head -2; lspci -k -d 10de: | grep -c 'in use: vfio-pci'" |
    sed 's/^/back /' | tee -a "$summary"
say "end $(date +%H:%M)"

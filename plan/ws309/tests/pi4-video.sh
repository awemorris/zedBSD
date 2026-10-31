#!/bin/sh
# ws309-p015: videoplayer windowed and then full screen on a Raspberry Pi with a console login (no session holding
# the display), run from the top of the tree after pi-p015.sh (or the same copies) put the programs in DIR on the board:
#   PI=ADDRESS sh plan/ws309/tests/pi4-video.sh DIR
# DIR on the board holds bin/wayland, bin/videoplayer, lib/*.so and video.mp4.  The compositor of DIR runs as root on
# its own socket; each mode plays 15 s twice (libvulkan's chain log: whether the full-screen chain drew into lent images
# and the copies among its presents; and videoplayer's DRAW lines); then the compositor's game-mode lines are printed,
# it is ended, and the kernel's firmware-decoder failure lines are counted.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
D=${1:?usage: pi4-video.sh DIR}
: "${PI:?PI=the board's address}"
SO="-i plan/tmp/guest/id_ed25519 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o BatchMode=yes"
b() { timeout "${BT:-60}" ssh $SO "root@$PI" "$@"; }
ENV="XDG_RUNTIME_DIR=$D/run LD_LIBRARY_PATH=$D/lib"
b "mkdir -p $D/run && chmod 700 $D/run; rm -f $D/run/wayland-0; env $ENV nohup $D/bin/wayland --socket=$D/run/wayland-0 --desktop-client=none > $D/wayland.log 2>&1 < /dev/null & echo \$! > $D/wayland.pid; sleep 8; test -S $D/run/wayland-0 && echo compositor up"
for mode in window full; do
	opt=
	[ $mode = full ] && opt=--fullscreen
	BT=90 b "env $ENV WAYLAND_DISPLAY=$D/run/wayland-0 VULKAN_ZEDBSD_WSI_LOG=1 timeout 40 $D/bin/videoplayer --timeout-s=15 $opt $D/video.mp4" 2>&1 | grep -E 'DONE|FRAMES|VULKAN WSI|PLAYBACK failed' | tail -4 | sed "s/^/$mode /"
	BT=90 b "env $ENV WAYLAND_DISPLAY=$D/run/wayland-0 timeout 40 $D/bin/videoplayer --timeout-s=15 $opt $D/video.mp4" 2>&1 | grep -E 'DRAW' | tail -2 | sed "s/^/$mode /"
done
b "grep -E 'KWL SCANOUT|KWL MODE|lendable|FAILED' $D/wayland.log | tail -8; kill \$(cat $D/wayland.pid); echo decoder failure lines \$(dmesg | grep -cE 'came back unread|not answered|no firmware decoder|execution failed')"

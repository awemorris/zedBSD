#!/bin/sh
# The QEMU regression of the applications whose threads and sockets moved to libkeiland's layers (design §6.4), in
# one run: boots the image (Venus 1280x800 as plan/ws035/tests/zdesktop-guest.sh, a sound device for Music with
# GUEST_AUDIO=1, the boot disk's cache=unsafe), waits for the session, runs the AAT's scenarios of those applications
# (plan/tools/aat/run-aat.sh), then this workstream's own checks on the same session (aat_ws327.py), stops the guest
# and sums the verdicts up.  Run from the worktree's root:
#   sh plan/ws327/tests/guest-apps.sh IMGBUILD OUT
# IMGBUILD is the image's build:
#   plan/tools/guest/test-image.sh plan/ws327/tests/config-amd64-aat.mk IMGBUILD \
#       --file /usr/share/keiland/wallpaper.png=userland/desktop/wallpapers/Birch-Lake.png
# OUT under build/ (a new directory each run).  GUEST_RUNTIME (default build/ws327-apps-run) keeps the guest apart
# from others; VENUS_RENDERER names the Venus renderer (default the worktree's build/ws035-sq-venus/install).
# The judgement is the verdicts the helpers draw from the session's log and the screenshots (aat shot) over SSH; the
# QEMU console is not read.  Prints each verdict and "ws327-guest-apps: PASS" (no fail) or "FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
img=${1:?usage: guest-apps.sh IMGBUILD OUT}
out=${2:?usage: guest-apps.sh IMGBUILD OUT}
top=$(pwd)
mkdir -p "$out"
GUEST_RUNTIME=${GUEST_RUNTIME:-$top/build/ws327-apps-run}
export GUEST_RUNTIME
# The scenarios: Mail (its connection, TLS and the IDLE thread, with the fake server on this host), Music, Video
# Player, Photos (the small copies' threads), Settings (the wallpapers' loader, Storage), the input method (the
# dictionary's writer), Terminal.  Mail's sign-in-code (a new message told by IDLE) is a scenario for a person's hands
# in the runner, so its helper is run by itself after the others.
scenarios="apps.mailer.read-compose apps.music.play apps.videoplayer.play apps.photos.browse
	apps.settings.pages desktop.appearance.wallpaper apps.terminal.type-command desktop.input-method.japanese-textedit"

# The emulator as zdesktop-guest.sh starts it.
. plan/tools/guest/venus-hostmem.sh
export LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
VENUS_RENDERER=${VENUS_RENDERER:-$top/build/ws035-sq-venus/install}
export RENDER_SERVER_EXEC_PATH="$VENUS_RENDERER/libexec/virgl_render_server"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
extra="-object memory-backend-memfd,id=mem,size=8G,share=on -machine memory-backend=mem"
extra="$extra -device virtio-gpu-gl-pci,id=venus,venus=on,blob=on,hostmem=$VENUS_HOSTMEM,max_outputs=1"
extra="$extra -display egl-headless,rendernode=/dev/dri/renderD128 -vnc unix:$GUEST_RUNTIME/vnc.sock,display=venus"
extra="$extra -device usb-tablet,bus=xhci.0,port=4"
# The sound device only with GUEST_AUDIO=1: with intel-hda the amd64 guest once stopped on an APIC acknowledgement
# (a kernel fault outside these applications), and Music's playing with sound was seen before.
if [ "${GUEST_AUDIO:-0}" = 1 ]; then
	extra="$extra -audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0"
fi
timeout 120 python3 -I plan/tools/guest/guest.py start "$img/hdd-image.img" --symbols "$img/vmunix" \
	--qemu "$top/plan/ws314/tests/qemu-unsafe.sh" --qemu-extra "$extra" > "$out/start.log" 2>&1 ||
	{ echo "ws327-guest-apps: the guest did not start ($out/start.log)"; exit 1; }
failed=0
if ! timeout 900 python3 -I plan/tools/guest/guest.py wait --timeout 840 > "$out/wait.log" 2>&1; then
	echo "ws327-guest-apps: the guest did not answer (analyse with the gdbstub, plan/tools.md)"
	failed=1
fi

if [ $failed = 0 ]; then
	# The session's desktop is up (the runner waits for it too), then the scenarios and the checks of this workstream.
	timeout 120 python3 plan/tools/aat/aat --qemu check > "$out/check.log" 2>&1 || failed=1
	# shellcheck disable=SC2086
	timeout 5400 plan/tools/aat/run-aat.sh qemu "$top/$out/aat" $scenarios > "$out/aat.log" 2>&1
	timeout 900 python3 plan/tools/aat/scenarios/helpers_mailer.py --outdir "$top/$out/mail-code" \
		--only 'apps.mailer.sign-in-code' -- --qemu > "$out/mail-code.log" 2>&1
	timeout 1800 python3 plan/ws327/tests/aat_ws327.py --outdir "$top/$out/ws327" -- --qemu > "$out/ws327.log" 2>&1
	timeout 120 python3 -I plan/tools/guest/guest.py run 'ps -A -o pid,args' > "$out/ps-end.txt" 2>&1
fi
timeout 120 python3 -I plan/tools/guest/guest.py stop > "$out/stop.log" 2>&1

# The verdicts: a fail anywhere fails the run; needs-person is a picture to look at (listed).
for file in "$out/aat/verdicts.tsv" "$out/mail-code/verdicts.tsv" "$out/ws327/verdicts.tsv"; do
	if [ ! -s "$file" ]; then
		echo "no verdicts in $file"
		failed=1
		continue
	fi
	awk -F '\t' '{ printf "%s %s %s\n", $2, $1, $5 }' "$file"
	awk -F '\t' '$2 == "fail" || $2 == "not-run" { bad = 1 } END { exit bad }' "$file" || failed=1
done
[ $failed = 0 ] && echo "ws327-guest-apps: PASS" || echo "ws327-guest-apps: FAIL"
[ $failed = 0 ]

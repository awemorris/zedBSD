#!/bin/sh
# BUG-370: the scenario apps.photos.browse on a QEMU guest (Venus 1280x800 as plan/ws035/tests/zdesktop-guest.sh):
# boots the AAT image, waits for SSH, runs the scenario with plan/tools/aat/run-aat.sh, stops the guest and prints the
# verdict.  The judgement is the helper's verdict, drawn from the session's log and the screenshots over SSH; the QEMU
# console is not read.  Run from the worktree's root:
#   sh plan/bugs/BUG-370/photos-guest.sh IMGBUILD OUT
# IMGBUILD is plan/tools/aat/build-image.sh's build; OUT a new directory under build/.
# GUEST_RUNTIME (default build/bug370-run) keeps the guest apart from others; VENUS_RENDERER names the Venus renderer.
# The last line: "bug370-photos: PASS" or "bug370-photos: FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
img=${1:?usage: photos-guest.sh IMGBUILD OUT}
out=${2:?usage: photos-guest.sh IMGBUILD OUT}
top=$(pwd)
mkdir -p "$out"
GUEST_RUNTIME=${GUEST_RUNTIME:-$top/build/bug370-run}
export GUEST_RUNTIME

# The emulator as zdesktop-guest.sh starts it.
. plan/tools/guest/venus-hostmem.sh
export LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
VENUS_RENDERER=${VENUS_RENDERER:-/home/awe/zedBSD-claude1/build/ws035-sq-venus/install}
export RENDER_SERVER_EXEC_PATH="$VENUS_RENDERER/libexec/virgl_render_server"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
extra="-object memory-backend-memfd,id=mem,size=8G,share=on -machine memory-backend=mem"
extra="$extra -device virtio-gpu-gl-pci,id=venus,venus=on,blob=on,hostmem=$VENUS_HOSTMEM,max_outputs=1"
extra="$extra -display egl-headless,rendernode=/dev/dri/renderD128 -vnc unix:$GUEST_RUNTIME/vnc.sock,display=venus"
extra="$extra -device usb-tablet,bus=xhci.0,port=4"
timeout 120 python3 -I plan/tools/guest/guest.py start "$img/hdd-image.img" --symbols "$img/vmunix" \
	--qemu "$top/plan/ws314/tests/qemu-unsafe.sh" --qemu-extra "$extra" > "$out/start.log" 2>&1 ||
	{ echo "bug370-photos: the guest did not start ($out/start.log)"; echo "bug370-photos: FAIL"; exit 1; }
failed=0
if ! timeout 900 python3 -I plan/tools/guest/guest.py wait --timeout 840 > "$out/wait.log" 2>&1; then
	echo "bug370-photos: the guest did not answer (analyse with the gdbstub, plan/tools.md)"
	failed=1
fi

# The scenario.
if [ $failed = 0 ]; then
	timeout 120 python3 plan/tools/aat/aat --qemu check > "$out/check.log" 2>&1 || failed=1
	timeout 1800 plan/tools/aat/run-aat.sh qemu "$top/$out/aat" apps.photos.browse > "$out/aat.log" 2>&1
fi
timeout 120 python3 -I plan/tools/guest/guest.py stop > "$out/stop.log" 2>&1

# The verdict.
verdict=$(awk -F '\t' '$1 == "apps.photos.browse" { print $2 }' "$out/aat/verdicts.tsv" 2>/dev/null | tail -1)
echo "bug370-photos: apps.photos.browse ${verdict:-none}"
grep -h "^AAT " "$out/aat.log" 2>/dev/null
[ "$verdict" = pass ] || [ "$verdict" = needs-person ] || failed=1
if [ $failed = 0 ]; then
	echo "bug370-photos: PASS"
else
	echo "bug370-photos: FAIL"
fi
exit $failed

#!/bin/sh
# sq001: boot the zdesktop guest image with a Venus GPU on this host.
#
# The GPU is virtio-gpu-gl with Venus, rendered by the host's Lavapipe
# (VK_DRIVER_FILES) through virgl_render_server, and read back by the
# egl-headless display from the vgem render node, which uses Mesa's zink on
# Lavapipe for GL (llvmpipe's own GL crashed QEMU reading a blob scanout back).  The console stays on the
# standard VGA adapter; the compositor's picture is on the device "venus",
# which VNC (the socket vnc.sock) shows to
# plan/ws035/tests/zdesktop-shot.py.
#
#   plan/ws035/tests/zdesktop-guest.sh start [IMAGE]
#   plan/ws035/tests/zdesktop-guest.sh stop
#
# Host set-up, once per boot of the host (sudo is allowed on this host):
#   scp -r awe@10.0.10.25:/home/awe/zedbsd-q306-venus/dependencies/q312-quiesce/install \
#       build/ws035-sq-venus/
#   sudo modprobe vgem && sudo chmod 0666 /dev/dri/renderD128
set -eu
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
# The renderer is the strict-queue build WS014 made (q312-quiesce on the Venus
# host, copied to build/ws035-sq-venus/install): libvulkan refuses a renderer
# without strict queue completion and native quiescence, which the stock
# Debian virglrenderer lacks (it enumerates no physical device).
VENUS_RENDERER=${VENUS_RENDERER:-$PWD/build/ws035-sq-venus/install}
export RENDER_SERVER_EXEC_PATH="${RENDER_SERVER_EXEC_PATH:-$VENUS_RENDERER/libexec/virgl_render_server}"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
command=${1:-start}
case "$command" in
start)
	image=${2:-build/ws035-sq/hdd-image.img}
	exec python3 plan/tools/guest/guest.py start "$image" \
	    --symbols build/ws035-sq/vmunix \
	    --qemu-extra "-object memory-backend-memfd,id=mem,size=8G,share=on -machine memory-backend=mem -device virtio-gpu-gl-pci,id=venus,venus=on,blob=on,hostmem=256M,max_outputs=1 -display egl-headless,rendernode=/dev/dri/renderD128 -vnc unix:$GUEST_RUNTIME/vnc.sock,display=venus -device usb-tablet,bus=xhci.0,port=4"
	;;
*)
	exec python3 plan/tools/guest/guest.py "$@"
	;;
esac

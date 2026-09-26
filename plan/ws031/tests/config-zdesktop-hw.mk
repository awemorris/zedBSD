# WS035 p066: the image for zdesktop on the i915 passthrough of the 5330 (plan/ws031/tests/vkloop-hw.sh
# zdesktop).  The userland of the zdesktop guest (plan/ws035/tests/config-amd64-userland.mk) with the i915
# driver and its firmware, the Vulkan and Wayland libraries, libtruetype, the compositor and its clients.
include plan/ws035/tests/config-amd64-userland.mk
CONFIG_GPU_JOB_RESERVATION_MS := 10000
CONFIG_GPU_JOB_EXECUTION_MS := 60000
CONFIG_GPU_JOB_STOP_MS := 10000
CONFIG_GPU_CONTROL_MS := 10000
CONFIG_DRIVER_PCI_I915 := y
ZEDBSD_USER_PROGRAMS += libvulkan libwayland-client libwayland-egl libegl libglesv2 libtruetype wltest wlshm mview zdesktop-terminal egltest libgl glxtest zgears zdesktop i915-firmware

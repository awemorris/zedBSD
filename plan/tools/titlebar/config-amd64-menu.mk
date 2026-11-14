# ws070 (System Menu): a lean zdesktop guest image for the Venus tests.
# The SSH guest harness (openssh, the serial mirror) without clang, lldb and
# libcxx, the Venus driver, the Wayland and Vulkan libraries, libkeiland,
# the compositor, the terminal and the test clients (wlshm, wltest, mview,
# menu-probe).  Build:
#   plan/tools/titlebar/build-menu-image.sh [BUILD]
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/ws035/tests/config-amd64-userland.mk
override CONFIG_PCAT_SERIAL_MIRROR := y
override CONFIG_DRIVER_PCI_VENUS := y
override CONFIG_USERLAND_PROGRAMS += openssl openssh
override CONFIG_USERLAND_PROGRAMS += libvulkan libwayland libtruetype wlshm wltest mview terminal wayland menu-probe titlebar-probe popup-probe subsurface-probe seat-probe data-probe extras-probe
# For zdesktop-p070 (App Home starts X applications through zdesktop-x11: xserver since
# WS069 p008, zterm, zgears over libGL).
override CONFIG_USERLAND_PROGRAMS += libwayland-egl libegl libglesv2 libGL zgears xserver

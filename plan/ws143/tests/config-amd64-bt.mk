# ws143-p002: the image of the Bluetooth HCI node checks: the CI image (config/ci/config-amd64.mk, which has
# CONFIG_DRIVER_USB_BT, /dev/btN) with the probe bt-probe.  A test image only.  QEMU (T1): boot, and no /dev/btN without
# a controller; the 5330 with its AX211's Bluetooth (USB 8087:0033) passed through: bt-probe -r.
#   make ZEDBSD_CONFIG=plan/ws143/tests/config-amd64-bt.mk BUILD=build/ws143 image
include config/ci/config-amd64.mk
ZEDBSD_USER_PROGRAMS += bt-probe

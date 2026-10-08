# ws143-p002: the image of the Bluetooth HCI class checks: the Files image (plan/tools/files/config-amd64-files.mk, which
# has CONFIG_DRIVER_USB_BT by default) with the test kernel's loopback controller (CONFIG_BT_TEST_LOOPBACK: /dev/bt0) and
# the probe bt-probe (with runas for the other-user check).  A test image only.
# QEMU (T1): plan/ws143/tests/bt-loopback-p002.sh.  The 5330 with its AX211's Bluetooth (USB 8087:0033) passed through:
# bt-probe -r -f /dev/bt1 (bt0 is the loopback's).
#   plan/ws143/tests/build-bt-image.sh BUILD   (with the test account btuser, ws143-p004)
include plan/tools/files/config-amd64-files.mk
CONFIG_BT_TEST_LOOPBACK := y
ZEDBSD_USER_PROGRAMS += bt-probe runas bluetoothd bt
# ws143-p005: the evdev probe of the HID input glue's checks (hid-usb-p005.sh).
ZEDBSD_USER_PROGRAMS += evdev-probe

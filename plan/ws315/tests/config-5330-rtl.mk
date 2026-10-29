# The Latitude 5330 regression image (plan/tools/hw5330/config-5330.mk) with the network probes, for
# plan/ws315/tests/5330-rtl.sh: the RTL8156 in Realtek's vendor mode (usb-rtl815x), TCP both ways against this host,
# echoes at 1472 and at jumbo sizes, camcap for the built-in camera.  A test image only.
#   make ZEDBSD_CONFIG=plan/ws315/tests/config-5330-rtl.mk BUILD=<dir> disk-image
include plan/tools/hw5330/config-5330.mk
CONFIG_DRIVER_USB_RTL815X := y
ZEDBSD_USER_PROGRAMS += sysctl ifconfig ping tcp-perf udp-hold camcap

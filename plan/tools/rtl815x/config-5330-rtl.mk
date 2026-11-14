# The Latitude 5330 regression image (plan/tools/hw5330/config-5330.mk) with the network probes, for
# plan/tools/rtl815x/5330-rtl.sh: the RTL8156 in Realtek's vendor mode (usb-rtl815x), TCP both ways against this host,
# echoes at 1472 and at jumbo sizes, camcap for the built-in camera.  A test image only.
#   make CONFIGMK=plan/tools/rtl815x/config-5330-rtl.mk BUILD=<dir> disk
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/hw5330/config-5330.mk
override CONFIG_DRIVER_USB_RTL815X := y
override CONFIG_USERLAND_PROGRAMS += sysctl ifconfig ping tcp-perf udp-hold camcap

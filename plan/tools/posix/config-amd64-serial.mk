# ws056-p002: plan/tools/gnu-utils/config-amd64-base.mk with the console mirrored
# on the first serial port (plan/tools/guest/serial.py), for running a test
# on the console.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/gnu-utils/config-amd64-base.mk
override CONFIG_PCAT_SERIAL_MIRROR := y

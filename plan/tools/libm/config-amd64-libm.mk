# ws076 (moved to plan/tools/libm): the lean amd64 image (plan/tools/gnu-utils/config-amd64-base.mk) with
# the console mirrored on the serial port, so plan/tools/libm/guest-test.sh
# can run the libm test runner in the guest through plan/tools/guest.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/gnu-utils/config-amd64-base.mk
override CONFIG_PCAT_SERIAL_MIRROR := y

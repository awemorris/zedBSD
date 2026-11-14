# ws345: the guest image of shell-guest.sh and guest-latency.sh: the default x86_64 image (its programs, with the
# browser, its engine, the compositor, the zedBSD SSH server and the Venus driver) with the browser's second program,
# without the firmware packages (the QEMU tests do not take their licences) and without the input method (the
# window under test is then client 1, as the tests expect), and with the console login (the tests start the
# compositor themselves).  The guest harness's files are added by plan/tools/guest/test-image.sh.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
include $(TEST_TREE)/mk/platform-x86_64/defaults.mk
override CONFIG_GRAPHICAL_LOGIN := n
override CONFIG_USERLAND_PROGRAMS := $(filter-out %-firmware ime dict skk-dict,$(CONFIG_USERLAND_PROGRAMS)) browser-probe

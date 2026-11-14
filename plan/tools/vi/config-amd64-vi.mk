# ws228-p002: the amd64 guest T1 edits files with vi on.  It is the CI amd64
# configuration without the external packages that take hours to build in a
# fresh tree, without the GPU demos, and with the serial mirror so that
# plan/tools/guest/serial.py drives it; vi is added, since the CI
# configuration lists its programs and does not name vi yet.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
include $(TEST_TREE)/mk/platform-x86_64/defaults.mk
override CONFIG_PCAT_SERIAL_MIRROR := y
override CONFIG_USERLAND_PROGRAMS := $(filter-out libcxx emacs clang openssh openssl \
	mview vkdemo zdesktop i915-firmware intelax211-firmware \
	rtl8822b-firmware,$(CONFIG_USERLAND_PROGRAMS)) vi

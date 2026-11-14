# ws076 (moved to plan/tools/libm): the lean amd64 image with the serial mirror and browser, so
# plan/tools/libm/browser-js.sh can run the browser's own JavaScript tests
# (plan/ws074/tests/js) in the guest with the new libm.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/libm/config-amd64-libm.mk
override CONFIG_USERLAND_PROGRAMS += browser

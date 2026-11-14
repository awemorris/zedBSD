# BUG-146: the lean Files image with the input method, as a desktop image has it.  zdesktop starts keiland-ime
# itself and it connects first, so the first application a test starts is client 2; the tests find the
# applications' client numbers with plan/tools/guest/zwl-clients.sh.  Build:
#   FILES_CONFIG=plan/tools/files/config-amd64-files-ime.mk plan/tools/files/build-files-image.sh BUILD
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/files/config-amd64-files.mk
override CONFIG_USERLAND_PROGRAMS += ime dict

# ws135: the Settings image with the input method (plan/ws089/tests/config-amd64-settings-ime.mk) and the settings probe
# keiland-settings (userland/tests/keiland-settings) for the tests of WS135.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/ws089/tests/config-amd64-settings-ime.mk
override CONFIG_USERLAND_PROGRAMS += keiland-settings

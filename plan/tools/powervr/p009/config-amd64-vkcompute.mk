# ws269-p009: amd64 Venus test image with vkcompute (T1-755).
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
include $(TEST_TREE)/plan/tools/guest/config-amd64-ssh.mk
override CONFIG_USERLAND_PROGRAMS += libwayland libvulkan vkcompute

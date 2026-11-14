# ws041: the pc98 CI image with wakebench.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.pc9821.mk
include $(TEST_TREE)/mk/platform-pc9821/defaults.mk
override CONFIG_USERLAND_PROGRAMS += wakebench

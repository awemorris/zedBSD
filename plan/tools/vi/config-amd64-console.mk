# ws228-p004: the CI amd64 image with a text boot and the login on the VT
# console, so the console's cursor position and rendition can be seen.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
include $(TEST_TREE)/mk/platform-x86_64/defaults.mk
override CONFIG_BOOT_SPLASH := n
override CONFIG_GRAPHICAL_LOGIN := n

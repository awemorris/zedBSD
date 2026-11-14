# ws174: the SSH guest image with the build's graphical boot configuration
# (logo=logo.ppm login=graphical kmsg=quiet), for the boot keys' QEMU cells
# (Ctrl: kmsg=console without a logo, Shift: login=console).  Build with
#   plan/tools/guest/test-image.sh plan/tools/boot-keys/config-amd64-keys.mk build/ws174-keys
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/guest/config-amd64-ssh.mk
override CONFIG_BOOT_SPLASH := y
override CONFIG_QUIET_BOOT := y

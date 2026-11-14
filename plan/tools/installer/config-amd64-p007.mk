# The SSH guest image (plan/tools/guest/config-amd64-ssh.mk) with efiboot, for
# plan/tools/installer/p007-efiboot.sh.  Build it with
#   plan/tools/guest/test-image.sh plan/tools/installer/config-amd64-p007.mk build/amd64
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/guest/config-amd64-ssh.mk
override CONFIG_USERLAND_PROGRAMS += efiboot

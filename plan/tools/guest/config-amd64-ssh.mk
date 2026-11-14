# ws063: the SSH guest image (plan/tools/guest) without clang and libcxx,
# for the crash and regression tests of the UFS journal (plan/tools/ufs).
# Build with plan/tools/guest/build-ssh-image.sh, or
#   eval "make -j16 CONFIGMK=plan/tools/guest/config-amd64-ssh.mk \
#     BUILD=build/amd64 $(python3 plan/tools/guest/guest.py extra-files) disk"
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/ws035/tests/config-amd64-userland.mk
override CONFIG_PCAT_SERIAL_MIRROR := y
override CONFIG_USERLAND_PROGRAMS += openssl openssh

# The installer's image (make installer-image) for plan/tools/installer/p009-install.sh.  The release's
# configuration, with the test's own additions to the live root only (J4): OpenSSH and OpenSSL, so the script reaches
# the live system over SSH.  The harness's keys and net.conf come as ZEDBSD_INSTALLER_TEST_FILES on the command line
# (plan/tools/guest/guest.py extra-files); no unit or index changes with them.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
include $(TEST_TREE)/mk/platform-x86_64/defaults.mk
ZEDBSD_INSTALLER_TEST_UNITS := network-openssh security-openssl

# The installer's image (make installer-image) for the installer's screens in QEMU.  The release's
# configuration with the test's additions to the live root only (J4): OpenSSH and OpenSSL, so a script reaches the
# live system over SSH (the harness's keys and net.conf come as ZEDBSD_INSTALLER_TEST_FILES on the command line).
# The toolchain's packages (clang, libcxx) and Emacs are left out, and Clang's item with them: a test does not build them.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
include $(TEST_TREE)/mk/platform-x86_64/defaults.mk
ZEDBSD_INSTALLER_TEST_UNITS := network-openssh security-openssl
ZEDBSD_INSTALLER_TEST_OMIT := clang libcxx emacs
ZEDBSD_INSTALLER_TEST_ITEMS := plan/tools/installer/installer-items-p010

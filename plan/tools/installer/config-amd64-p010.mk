# The installer's image (make installer-image) for the installer's screens in QEMU.  The release's
# configuration with the test's additions to the live root only (J4): OpenSSH and OpenSSL, so a script reaches the
# live system over SSH (the harness's keys and net.conf come as ZEDBSD_INSTALLER_TEST_FILES on the command line).
# The toolchain's packages (clang, libcxx) and Emacs are left out, and Clang's item with them: a test does not build them.
include config/ci/config-amd64.mk
ZEDBSD_INSTALLER_TEST_UNITS := network-openssh security-openssl
ZEDBSD_INSTALLER_TEST_OMIT := clang libcxx emacs
ZEDBSD_INSTALLER_TEST_ITEMS := plan/tools/installer/installer-items-p010

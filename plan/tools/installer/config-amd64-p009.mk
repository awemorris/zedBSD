# The installer's image (make installer-image) for plan/tools/installer/p009-install.sh.  The release's
# configuration, with the test's own additions to the live root only (J4): OpenSSH and OpenSSL, so the script reaches
# the live system over SSH.  The harness's keys and net.conf come as ZEDBSD_INSTALLER_TEST_FILES on the command line
# (plan/tools/guest/guest.py extra-files); no unit or index changes with them.
include config/ci/config-amd64.mk
ZEDBSD_INSTALLER_TEST_UNITS := network-openssh security-openssl

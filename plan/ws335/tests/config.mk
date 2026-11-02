# The test image of libcrypto-compat.so: a small amd64 guest on the serial console (no SSH, no desktop) with the
# library selected, so that /lib/libcrypto-compat.so is in the image.  run-image.sh builds it, adds crypto-smoke,
# boots it and runs the program.
include config/ci/config-amd64.mk
CONFIG_PCAT_SERIAL_MIRROR := y
ZEDBSD_GRAPHICAL_BOOT := n
ZEDBSD_GRAPHICAL_LOGIN := n
ZEDBSD_USER_PROGRAMS := libcrypto-compat cat echo ls uname sync true false

# ws035-p038: the userland image for the SSH guest harness (plan/tools/guest):
# OpenSSH, lldb (from the clang package), the serial mirror for watching the
# console, and the harness keys,
# which the make command adds with "$(plan/tools/guest/guest.sh extra-files)".
include plan/ws035/tests/config-amd64-userland.mk
CONFIG_PCAT_SERIAL_MIRROR := y
ZEDBSD_USER_PROGRAMS += openssl openssh libcxx clang

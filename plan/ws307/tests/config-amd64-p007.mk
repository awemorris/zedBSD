# WS307 p007: the SSH guest image (plan/tools/guest/config-amd64-ssh.mk) with efiboot, for
# plan/ws307/tests/p007-efiboot.sh.  Build it with
#   plan/tools/guest/test-image.sh plan/ws307/tests/config-amd64-p007.mk build/amd64
include plan/tools/guest/config-amd64-ssh.mk
ZEDBSD_USER_PROGRAMS += efiboot

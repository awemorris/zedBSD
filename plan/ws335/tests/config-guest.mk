# The guest image of the TLS interop test (run-guest.sh): a small amd64 guest on the serial console (no SSH, no
# desktop) with both compat libraries and the network (networkd with DHCP on the USB adapter ue0, net.conf of
# plan/tools/guest), so that /root/tls-client reaches the host's s_server as 10.0.2.2.
include config/ci/config-amd64.mk
CONFIG_PCAT_SERIAL_MIRROR := y
ZEDBSD_GRAPHICAL_BOOT := n
ZEDBSD_GRAPHICAL_LOGIN := n
ZEDBSD_USER_PROGRAMS := libcrypto-compat libssl-compat networkd dhcpc ifconfig cat echo ls uname sync true false
